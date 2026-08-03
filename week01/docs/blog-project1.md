# 从零手写高并发 HTTP 服务器：架构选型、权衡与踩坑全记录

> C++17 · epoll · 主从 Reactor · one loop per thread · 时间轮 · 异步日志
> 大一暑期 · 28 天 · 4 核 8G VM · **6,486 QPS** · **Valgrind 97 alloc / 97 free 零泄漏**

这是一篇**面试向**的项目复盘。我尽量不写"我做了什么"，而是写"我为什么这么做"——每个设计决策背后都有一个或多个备选方案，以及我权衡后为什么选它。面试官问的往往不是你知道什么，而是**你为什么在那么多选择里选了这一个**。

---

## 〇、先说结论：这个项目是什么

一个**模块化、可复用**的 HTTP/1.1 静态文件服务器。最终版 `v1_http_server` 只有约 30 行"组装代码"，其余功能全部拆成可独立复用的 header-only 模块：

```
Socket → Epoll → Channel → Buffer → EventLoop
  → TcpConnection → SubReactor → TcpServer     (网络层，可复用网络库雏形)
  → HttpParser → HttpStaticHandler             (HTTP 层)
  → TimerWheel                                 (定时器)
  → AsyncLogger                                (异步日志)
```

演进路径（28 天，每个阶段都是独立可运行的里程碑）：

```
阻塞式 echo server → epoll 非阻塞 → 单 Reactor → 主从 Reactor
    → HTTP 状态机解析 → 静态文件服务 → Keep-Alive → 时间轮
    → 异步日志 → 模块化重构 → 单元测试 → 调优 → 收尾
```

**一句话亮点**：从阻塞 socket 到生产级架构，每行代码自己写出来，不抄教程；最后用工程工具（单测 / GDB / Valgrind / strace）证明它"没泄漏、没崩溃、多线程模型正确"。

---

## 一、为什么用 Reactor，而不是 thread-per-connection？

这是第一个面试必问题。我先讲清楚**备选方案**：

| 方案 | 每个连接资源 | 瓶颈 | 适合场景 |
|------|------------|------|---------|
| **thread-per-connection** | 1 个线程（默认 8MB 栈） | 线程创建/切换开销，C10K 即崩 | 连接数少、连接处理耗时（如 RPC） |
| **阻塞 IO + select/poll** | O(n) 遍历 | 每次 select 全量扫描 fd_set | 连接 < 100 |
| **事件驱动 + epoll** | O(1) 注册 + 事件回调 | 代码复杂度高 | 万级连接、IO 密集 |

**权衡**：HTTP 服务器是典型的"连接多、单请求处理快"场景。1000 个连接如果开 1000 个线程，光线程栈就 8GB，还不算切换开销。而 epoll 是**就绪通知模型**——连接来了才回调，空闲连接几乎零成本。代价是**代码要反转**：从"我主动读"变成"事件来了我处理"，所有状态必须挂在每个连接上。

**我的选择**：事件驱动 + Reactor。epoll 只解决"等"的问题，Reactor 解决"怎么组织"的问题——把 fd、事件、回调绑成一个 `Channel`，事件循环统一分发。

```cpp
// channel.h —— 核心抽象：一个 fd + 它关心的事件 + 回调
class Channel
{
    int         fd_;
    uint32_t    events_;
    EventCallback readCallback_;
    EventCallback writeCallback_;
    // ...
};
```

> **面试答法**：先量化需求（连接多、处理快），再列方案，最后用"线程栈 8MB × 1万连接 = 80GB"这种具体数字说明为什么排除 thread-per-connection。

---

## 二、单 Reactor 还是主从 Reactor？——我用真实压测数据回答

这是项目里**最反直觉**的一个发现，也是最好的面试谈资。

### 两种架构

- **单 Reactor**：一个线程干所有事——`accept()` + 所有连接 IO + 定时器。
- **主从 Reactor**：`MainReactor`（主线程）只负责 `accept()`，新连接通过 **RoundRobin 无锁分发**到 `SubReactor`（每个 worker 线程一个 `EventLoop`，one loop per thread）。每个 SubReactor 管理一组连接的读写，通过 `eventfd` 实现线程间唤醒。

```
                    MainReactor (main thread)
                        │
                epoll_wait(listen_fd)
                        │ OnAccept()
                RoundRobin (std::atomic, 无锁)
                        │
        ┌───────────────┼────────────────┐
        ▼               ▼                ▼
   SubReactor[0]   SubReactor[1]   SubReactor[2] ...
   (worker thread) (worker thread) (worker thread)
   每个：epoll_wait + timerfd + 一组 TcpConnection
```

### 压测数据（ab，2026/07/01，同一台 VM）

| 场景 | 单 Reactor | 主从 Reactor |
|------|-----------|-------------|
| Keep-Alive, 100 并发 | **8,113** QPS | 3,528 QPS |
| Keep-Alive, 1000 并发 | **8,521** QPS | 3,257 QPS |
| 短连接, 200 并发 | **2,278** QPS | 782 QPS |

**单 Reactor 在低~中并发下全面碾压主从 Reactor。** 为什么？

1. **无上下文切换**：一个线程处理所有请求，CPU 缓存友好（hot cache）。
2. **无跨线程通信开销**：主从架构里每次 `RunInLoop` 都要：push 任务 → `write(eventfd)` 敲门 → 对端 `epoll_wait` 被唤醒 → 读 eventfd → 执行 lambda。这一串系统调用 + 内存屏障在并发不高时是纯开销。
3. **锁竞争**：多线程共享资源（如连接 map）需要同步。

### 那主从 Reactor 有什么用？

**当 accept() 成为瓶颈时**。单个线程 accept + 处理 IO，在**万级以上并发**下会：
- accept 惊群（多线程同时被唤醒争抢同一个连接）——主从模型里 MainReactor 独占 accept，天然避免；
- 单线程事件循环里，一个慢请求会阻塞后面所有请求（head-of-line blocking）。

**结论（面试金句）**：*架构选型要看场景，没有银弹。单 Reactor 是"简单 + 低并发够用"，主从 Reactor 是"为万级并发铺路 + 可扩展"。* 我这台 4 核 VM 体现不出主从优势，但它的价值在于**把网络层拆成了可复用的库**，为项目二（仿 muduo 网络库）打下了架构基础。

> **面试答法**：先给数据，再讲原因（上下文切换、跨线程开销、缓存局部性），最后主动说"如果面试官环境并发低，选单 Reactor 更优；高并发选主从"。展现出"我不是无脑堆线程"。

---

## 三、线程安全：RunInLoop / QueueInLoop 与 eventfd

主从 Reactor 的核心难题：**MainReactor 想把一个 fd 交给 SubReactor 管理**，但那个线程正在 `epoll_wait()` 阻塞着。怎么"打断"它？

### 备选方案

1. **`wakeup()` + 条件变量**：需要维护额外同步，复杂。
2. **`pthread_kill` 发信号打断 epoll**：信号处理的 async-signal-safe 限制太严格。
3. **`eventfd` + 写入字节唤醒**：`epoll_wait` 会因 fd 可读而返回，读完就继续循环。**这是 muduo 的标准做法，我选了它。**

### 核心机制

```cpp
// 线程 A (MainReactor) 想把任务丢给线程 B (SubReactor)
void EventLoop::QueueInLoop(Functor cb)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingFunctors_.push_back(std::move(cb));
    }
    // 用 eventfd 唤醒对方线程的 epoll_wait（"敲门"）
    uint64_t one = 1;
    write(wakeupFd_, &one, sizeof(one));
}

// 线程 B (SubReactor) 的循环：
while (!quit_)
{
    epoll_wait(...);            // 被 write(wakeupFd_) 唤醒
    DoPendingFunctors();        // 取出队列，逐个执行
    HandleEvents();             // 处理就绪的连接事件
}
```

**为什么是 eventfd 而不是普通管道？** eventfd 是一个 64 位计数器，专门为事件通知设计：
- 内核内原子计数，不需要用户态缓冲区；
- 一次 `write` 只消耗一个系统调用；
- `read` 清零后可以复用，不会像管道那样积累数据。

**为什么任务队列要加锁？** 因为 `pendingFunctors_` 是跨线程共享的，`QueueInLoop` 可能从任意线程被调用。但注意：**加锁只保护队列本身，实际 IO 仍然只在所属线程执行**——这就是 one loop per thread 的精髓：数据归线程，交互走队列。

---

## 四、延迟删除：一个真实的 use-after-free bug

这是项目里最深刻的一个 bug，也是"多线程 + RAII"最容易踩的坑。

### 问题现场

`TcpConnection` 的连接关闭流程：

```
TcpConnection::OnClose()
    → 回调 SubReactor 从 connections_ 里 erase(fd)
        → unique_ptr 析构 → ~TcpConnection → ~Channel
            → Epoll::Del(fd)        ← 正在修改 epoll 实例
    → 返回 HandleEvent
        → 访问已析构的 Channel     ← 💥 use-after-free！
```

**根因**：析构发生在**事件处理栈**里。`HandleEvent` 正在遍历就绪事件、调用 `Channel` 的回调，结果回调链内部把 `Channel` 自己给 `delete` 了，等回调返回，代码继续访问一个已经死掉的对象。

### 备选方案

1. **标记 + 下次循环清理**（`QueueInLoop` 延迟删除）——**我选了它**。
2. 引用计数（类似 shared_ptr）——能解决，但把内存管理与事件模型耦合，复杂。
3. 全程栈上管理——不适用于动态连接的场景。

### 修复

```cpp
// OnClose 里不直接删除，而是"排队"到下一轮事件循环
conn->SetCloseCallback([this](TcpConnection* c)
{
    int fd = c->Fd();
    loop_->QueueInLoop([this, fd]()
    {
        connections_.erase(fd);   // 安全：此时 HandleEvent 已返回
    });
});
```

**为什么安全**：`QueueInLoop` 里的 lambda 在**下一轮** `DoPendingFunctors()` 才执行，那时上一轮 `HandleEvent` 已经完全退出，没有栈帧再引用这个 `Channel` 了。

> **面试答法**：手动画出析构触发点在回调栈里的位置，然后说"删除操作必须离开当前事件栈"。这是 muduo 里 `TcpConnection` 也用延迟删除的同一个原因——不是我发明的，但我踩过并用同样的思路修好了。

---

## 五、epoll：为什么用 ET + 非阻塞，而不是 LT？

### LT vs ET（面试必问）

| | LT（水平触发） | ET（边缘触发） |
|---|---|---|
| 通知时机 | fd 可读/可写就一直通知 | 只在状态**变化**时通知一次 |
| read 方式 | 读多少都行，没读完下次还通知 | 必须循环读到 `EAGAIN`，否则丢数据 |
| 系统调用 | 可能多次重复通知，浪费 | 通知次数最少，最高效 |
| 编码难度 | 简单 | 难，容易漏读 |

### 我的选择：ET + 非阻塞

- **非阻塞 fd 是前提**：ET 下必须循环 `read()` 到 `EAGAIN`，如果 fd 是阻塞的，最后一次 `read()` 会永久卡死线程。
- **EPOLLONESHOT**：每次就绪后自动从就绪链表摘除，处理完再重新注册。防止多线程（虽然本项目一个连接只归一个 SubReactor，但这是标准防御）竞争同一个 fd。

```cpp
// epoll.h —— 注册一个 fd 的完整状态
void Epoll::Add(const Channel* ch)
{
    epoll_event ev;
    ev.events   = ch->events() | EPOLLET | EPOLLONESHOT;
    ev.data.ptr = const_cast<Channel*>(ch);   // 用 ptr 而非 fd！
    epoll_ctl(epollFd_, EPOLL_CTL_ADD, ch->fd(), &ev);
}
```

**细节：`data.ptr` 而不是 `data.fd`**。用指针可以直接拿到 `Channel` 对象，省一次 fd→对象 的查找；代价是要自己保证指针生命周期（配合前面的延迟删除）。这是 epoll 面试的一个加分细节。

> **面试答法**：ET 的"一次通知 + 循环读到 EAGAIN"和"必须配非阻塞"是必背的。加上 `data.ptr` 和 `EPOLLONESHOT` 两个细节，显得真的写过。

---

## 六、HTTP 解析：为什么用状态机？

请求是按字节流到的，TCP 不保证一次 `read()` 就能拿到完整请求。如果写成"读到完整数据再解析"，会卡在"怎么知道读完了"。

**状态机方案**：一次只吃一个字符/一段数据，随时可能返回三种结果：

```
PARSE_OK        ← 解析完成，可以生成响应
NEED_MORE_DATA  ← 数据不够，等下次 read 继续
PARSE_ERROR     ← 格式非法，回 400
```

```
                     +----------------+
                     |   方法 + 空格   |
                     +--------+-------+
                              v
                     请求行状态机  →  遇 \r\n
                              |
                              v
                     头部状态机 (name: value)
                              |
                              v
                     遇 空行  →  按 Content-Length 收 body
                              |
                              v
                          PARSE_OK
```

**关键好处**：
1. **天然支持粘包/半包**：一次 `read` 可能拿到 1.5 个请求，也可能只有半个请求。状态机"吃到哪算哪"，`NEED_MORE_DATA` 时把残余数据留在 Buffer 里，下个事件继续。
2. **不需要固定缓冲区**：请求头 + body 可以跨多次 read 累积。
3. **零动态分配**：解析过程只维护枚举状态 + 指针游标。

---

## 七、时间轮定时器：为什么 O(1)，而不是最小堆？

服务器要**踢掉空闲连接**（防 Slowloris 攻击、防资源泄漏），每个连接一个超时。怎么管理这些定时器？

### 备选方案对比

| 方案 | 插入 | 删除 | 触发 | 缺点 |
|------|------|------|------|------|
| **最小堆**（std::priority_queue） | O(log n) | O(log n) 惰性 | O(1) 取堆顶 | 无法精确删除指定定时器（只能惰性跳过） |
| **红黑树**（std::map） | O(log n) | O(log n) | O(1) | 实现复杂 |
| **时间轮** | **O(1)** | **O(1)** | O(1)（每 tick 检查一槽） | 精度受槽数限制 |

### 我的选择：60 槽时间轮，1 秒 tick

```cpp
// timer_wheel.h —— 简化版
constexpr int kTicksPerRound = 60;   // 60 槽，每槽 1 秒
std::unordered_map<int, std::shared_ptr<TimerNode>> slots_[kTicksPerRound];
int curSlot_ = 0;                    // 当前指针

void TimerWheel::Add(int fd, int timeout)
{
    int slot = (curSlot_ + timeout) % kTicksPerRound;  // O(1) 定位槽
    slots_[slot][fd] = MakeTimer(fd);                  // O(1) 插入
}
```

**为什么 O(1)**：插入时直接根据"当前指针 + 超时秒数"算槽位；删除时直接从槽里的哈希表移除；每 tick 只扫当前槽。都是常数时间。

**取舍（面试要讲）**：时间轮精度是"秒级"（每槽 1 秒），适合**定时踢连接**这种不需要精确到毫秒的场景。如果是定时任务、调度器这种需要精确触发的，最小堆更合适。**"按需选型"才是关键**。

每个 SubReactor 有独立 timerfd（1 秒触发一次），tick 时遍历当前槽、关闭超时连接。

---

## 八、异步日志：为什么双缓冲 + 后台线程？

日志如果直接同步写磁盘，每次 `write()` 都是系统调用 + 磁盘 IO，IO 线程会被卡住。

### 演进过程

1. **最朴素方案**：IO 线程里 `fprintf(stderr, ...)`。❌ 每个日志行都是系统调用，且日志线程和 IO 线程抢锁。
2. **异步方案（我用的）**：IO 线程只往内存缓冲区写，后台线程负责刷盘。

### 双缓冲架构

```
IO 线程 (主从 Reactor)                后台线程 (AsyncLogWriter)
     │  Logger::Log(msg)                    │
     │      → 格式化到 LogStream             │
     │      → 满 4MB 时 swap                 │
     │         ┌──────────┐                  │
     │         │ 前端 4MB  │◄─── 空闲         │
     │         └──────────┘                  │
     │  Swap()  ───────────────────────────► │
     │         ┌──────────┐                  │  读取 → fwrite → 刷盘
     │         │ 后端 4MB  │                  │
     │         └──────────┘                  │
     └───────────────────────────────────────┘
```

**为什么两块而不是一块**：
- **一块缓冲区**：IO 线程写日志时必须等后台线程读完才能复用，锁竞争严重。
- **两块（双缓冲）**：写满前端就 swap，后台写后端。IO 线程**永远只锁"交换指针"那一瞬间**，临界区极小。这就是 muduo 双缓冲日志的核心思想。

### 数据

- 每 `LogStream` 4KB 格式化缓冲；
- 每 `LogBuffer` 4MB，双缓冲共 8MB；
- IO 线程对 QPS 的影响被压到最低——日志只是 append 内存，不碰磁盘。

> **面试答法**："IO 线程零阻塞"是靠"内存写 + 双缓冲 swap + 后台刷盘"三层实现的。可以画 swap 时序图，讲清楚为什么双缓冲把锁竞争降到最低。

---

## 九、优雅关闭：为什么用 sigwait 而不是 signal()？

`Ctrl+C` 默认直接杀进程，析构函数不执行 → 内存、fd、线程全部泄漏。要优雅退出，得先想清楚信号怎么处理。

### signal() 的问题

```cpp
signal(SIGINT, [](int) { /* 在这里优雅关闭 */ });
```

信号回调运行在**信号上下文**，POSIX 规定只能调用约 120 个 **async-signal-safe** 函数（`write`/`read`/`open`/`_exit`…），**不能调** `new`、`delete`、`cout`、`mutex`、`string`、`vector`。而优雅关闭需要 `Quit()` → `epoll` 操作 → `join` 线程 → 释放内存——**全是不安全的函数**。

### 我的选择：pthread_sigmask + sigwait

```cpp
// main() 第一行，必须在创建任何线程之前！
sigset_t set;
sigemptyset(&set);
sigaddset(&set, SIGINT);
sigaddset(&set, SIGTERM);
pthread_sigmask(SIG_BLOCK, &set, nullptr);   // 阻塞信号 → 不执行默认动作

// 单独一个线程等信号
std::thread signalThread([&set]()
{
    int sig;
    sigwait(&set, &sig);                      // 在普通线程上下文"着陆"
    g_server->Quit();                         // 此时可以安全调用任意函数
});
```

**为什么信号能"着陆"在普通线程**：`pthread_sigmask(SIG_BLOCK)` 让信号不再触发默认动作而是"挂起排队"，`sigwait` 把它取出来在**普通线程上下文**执行——普通线程没有 async-signal-safe 限制，`new`、`mutex` 随便用。

### ⚠️ 最大的坑：顺序

```
❌ logWriter.Start();          // 先创建了日志线程
   pthread_sigmask(SIG_BLOCK); // 后阻塞信号
   → 日志线程不继承信号掩码 → 信号打到它 → 行为未定义！

✅ pthread_sigmask(SIG_BLOCK);  // 必须在所有 std::thread 之前
   logWriter.Start();           // 之后创建的所有线程都继承掩码
```

> **面试答法**：先讲 signal() 的 async-signal-safe 限制，再讲 sigwait 怎么绕开，最后强调"线程创建之前设掩码"这个细节。这个细节是区分"看过"和"写过"的关键。

---

## 十、工程质量：怎么证明它没泄漏、没崩溃、模型正确

项目做得再花哨，面试官最关心的是**你信不信自己的代码**。我用四个工具交叉验证：

### 10.1 单元测试（Google Test，26/26）

| 测试目标 | 用例数 | 覆盖范围 |
|----------|--------|----------|
| `buffer_test` | 13 | 读写、扩容、边界、ReadFd 错误注入 |
| `threadpool_test` | 7 | 任务提交、返回值、异常安全、关闭超时 |
| `eventloop_test` | 6 | RunInLoop、QueueInLoop、跨线程唤醒 |

设计思路：每个 TEST 只测一个行为；边界条件（空 buffer、满 buffer）+ 故障注入（ReadFd(-1)）都要覆盖。

### 10.2 GDB：可视化验证 one loop per thread

```
(gdb) break TcpConnection::OnRead        # worker 线程回调
(gdb) break SubReactor::OnAccept         # 主线程回调
(gdb) thread apply all bt                # 打印所有线程调用栈
```

结果（这就是架构正确性的直接证据）：
- **主线程**：`epoll_wait(listen_fd)` —— MainReactor 只 accept；
- **子线程**：`epoll_wait(client_fds + timerfd)` —— SubReactor 处理 IO；
- **日志线程**：`pthread_cond_wait` —— 后台刷盘。

三个线程三种职责，清清楚楚。

### 10.3 Valgrind：97 alloc / 97 free，零泄漏

| 类别 | 结果 |
|------|------|
| definitely lost | 0 bytes ✅ |
| indirectly lost | 0 bytes ✅ |
| possibly lost | 0 bytes ✅ |
| still reachable | 0 bytes ✅ |

**最有说服力的对比**：
- 旧版（无优雅关闭，直接 Ctrl+C）：**33.6MB still reachable** —— 析构链没走完；
- 新版（优雅关闭）：**0 bytes in 0 blocks**，97 alloc / 97 free，100% 释放。

> 顺带学会 Valgrind 四个泄漏分类（definitely / indirectly / possibly / still reachable）——这是面试常考概念，我现在能结合项目讲清楚每个分类的实际含义。

### 10.4 strace：追踪真实系统调用

用 `strace -f -c` 统计主/子线程的 syscall 分布，验证：
- 主线程 `accept` 后没有碰业务 IO；
- 子线程 `epoll_wait` / `read` / `write` 承担全部数据流；
- 确认没有意外的 fd 泄漏（打开的 fd 数稳定）。

---

## 十一、性能数据与瓶颈分析

### wrk 压测（v1_http_server，2026/07/24）

```
wrk -t4 -c100 -d30s http://localhost:8888/index.html
```

| 指标 | 数值 |
|------|------|
| **QPS** | **6,486** |
| 线程数 / 并发 | 4 / 100 |
| 失败请求 | **0** |

### 瓶颈在哪（主动分析，面试加分）

在 4 核 VM 上，瓶颈是 **CPU 密集**：HTTP 解析 + 文件 IO + 系统调用都吃 CPU。已识别的优化方向（不是空话，是计划）：

1. **内存池**：`malloc`/`free` 大量小对象，池化后减少系统调用（项目二要做）；
2. **零拷贝 `sendfile`**：静态文件服务从 `read` + `write` 两次拷贝降到一次内核拷贝；
3. **LFU 缓存**：热点文件缓存，减少磁盘 IO。

> **面试答法**：压测数据要能说清"环境 + 命令 + 结果 + 为什么是这个数字"。更重要的是**能指出瓶颈并给出量化优化方向**，这比报一个 QPS 数字有价值得多。

---

## 十二、踩坑合集：7 个真实 bug

这些不是虚构的面试场景，是 28 天里真实修过的。

| # | Bug | 根因 | 教训 |
|---|-----|------|------|
| 1 | **use-after-free** | `erase(fd)` 在事件回调栈内触发析构 | 删除操作必须 `QueueInLoop` 延迟到下一轮 |
| 2 | **ET 下线程卡死** | ET 必须循环读到 `EAGAIN`，fd 忘了设非阻塞 | ET 与非阻塞是绑定关系 |
| 3 | **fd 泄漏** | accept 的 fd 没有 RAII 管理，异常路径漏 close | Socket RAII，所有权显式移交 |
| 4 | **Buffer 越界** | `Avail()` 用 `strlen(buffer_)`，但 buffer 无 `\0` | 缓冲区大小用"游标差"，不用 strlen |
| 5 | **日志永远不落盘** | `Flush()` 忘调用，前端写满没提交 | 双缓冲的 swap 流程必须有闭环 |
| 6 | **线程析构 terminate** | `AsyncLogWriter` 无析构函数，`joinable` 线程直接 `std::thread` 析构 | **joinable 的 thread 析构 = 直接 terminate**，必须 join |
| 7 | **bind 失败时崩溃** | listen socket 在线程创建之后才建，bind 失败异常路径析构未 join 线程 | 资源创建顺序：先备齐资源，再启动线程 |

> 其中坑 6、7 本质是同一个根因：**C++11 `std::thread` 析构时若仍 joinable 直接 `std::terminate()`**。这解释了为什么"线程池必须能优雅关闭""资源创建要在线程启动前完成"。

---

## 十三、面试自我提问清单

把这篇博客压缩成 10 个问题，答案能答 30 分钟：

1. 为什么用 Reactor 而不是 thread-per-connection？
2. 单 Reactor 和主从 Reactor 各自优劣？你压测数据怎么说？
3. epoll LT/ET 区别？为什么 ET 必须配非阻塞？
4. `data.ptr` 和 `data.fd` 选哪个？为什么？
5. 跨线程怎么唤醒阻塞的 `epoll_wait`？为什么用 eventfd？
6. 你的连接删除怎么避免 use-after-free？（画调用栈）
7. HTTP 解析为什么用状态机？半包粘包怎么处理？
8. 时间轮和最小堆定时器怎么选？
9. 异步日志为什么双缓冲？怎么做到 IO 线程零阻塞？
10. 优雅关闭为什么用 sigwait 而不用 signal()？最大的坑是什么？

---

## 附：关键数据速查卡

```
架构      主从 Reactor + one loop per thread + eventfd 唤醒
模块      13 个 header-only 模块，v1 主程序约 30 行
压测      wrk: 6,486 QPS @ 4t/100c/30s, 0 失败
单测      26/26 (buffer 13 + threadpool 7 + eventloop 6)
内存      Valgrind: 97 alloc / 97 free, 0 leaked bytes
调试      GDB 断点验证 one loop per thread / strace 验证 syscall 分布
代码      C++17, CMake 11 targets, 历史版本全保留
```

---

> 📅 Day 28 · 项目一总结 · 2026/08/03
> 项目代码：[http_server/week01](.) · 设计文档：DESIGN.md · 学习笔记：c++ http服务器开发.txt
