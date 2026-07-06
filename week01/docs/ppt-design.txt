# 高并发 HTTP 服务器 — 架构回顾 PPT（共 11 页）

## 全局样式

- 比例：16:9 宽屏
- 配色：深色背景（#0F0F14），卡片（#1A1A24），蓝色标题（#60A5FA），绿色代码（#34D399），粉色强调（#F472B6）
- 字体：标题 28-36pt，正文 14-16pt，代码 12-13pt Consolas

---

## 第 1 页：封面

**标题**：高并发 HTTP 服务器 — 架构全景回顾

**副标题**：从 OS 原语到主从 Reactor · 九层架构逐层拆解

**标签**：C++17 | epoll | Reactor | One Loop Per Thread

**右下角**：2026/07 · 4 周开发 · 压测 6234 QPS · 0 失败

---

## 第 2 页：九层架构全景图

**标题**：九层架构全景图

**内容**：纵向分层图，从上到下：

```
⑨ MultiReactorServer     — 主从调度编排（黄色背景块）
⑧ HttpParser / TimerWheel / AsyncLogger — HTTP 业务层（黄色背景块）
⑦ EventLoop              — One Loop Per Thread（蓝色背景块）
⑥ ThreadPool             — 任务队列，暂未使用（蓝色背景块）
⑤ Channel                — (fd, events, callbacks) 三元组（蓝色背景块）
④ Buffer                 — TCP 字节流组装工厂（蓝色背景块）
③ Epoll                  — epoll RAII 封装（绿色背景块）
② Socket                 — socket RAII 封装（绿色背景块）
① OS 原语               — epoll / eventfd / timerfd / readv（粉色背景块）
```

层与层之间用 ↑ 箭头连接。

---

## 第 3 页：RAII 封装层（Socket & Epoll）

**标题**：①② Socket & Epoll — RAII 封装 OS 原语

**副标题**：移动不拷贝 · 谁创建谁释放

**左栏（问题）**：
- 忘记 close → fd 泄漏 → "Too many open files"
- 拷贝 socket → 两个对象持同一个 fd → double close → 未定义行为

**右栏（解法）**：
- 构造获取资源，析构自动释放
- 移动接管所有权（`fd_ = other.fd_; other.fd_ = -1`）
- 禁止拷贝（`= delete`）
- 两个关键场景：`accept()` 返回值移动 + `shared_ptr` 跨线程传递

**底部要点**：explicit 防隐式转换 | SO_REUSEADDR 快速重启 | SetNonBlocking 配合 epoll ET

---

## 第 4 页：Buffer 应用层缓冲区

**标题**：④ Buffer — TCP 字节流的"组装工厂"

**副标题**：非阻塞 IO 为什么必须有应用层缓冲区

**两个原因**：

| 问题 | 无 Buffer | 有 Buffer |
|------|-----------|-----------|
| TCP 字节流无边界 | 半条请求无法处理，下次 read 接不上 | 攒够一条完整消息再交出 |
| 非阻塞 IO 不保证一次读完 | 栈上 buf 离开作用域即丢 | 持久化，下次 EPOLLIN 继续 append |

**readv + iovec 双缓冲**：

```
[fd] → readv(iov, 2) → [内部 buffer_] + [栈 extrabuf 64KB]
一次系统调用完成读取
  - 数据 ≤ WritableBytes → 只填内部 buffer
  - 数据 > WritableBytes → 内部填满 + 溢出到 extrabuf → 再 Append 回来
```

**底部**：kCheapPrepend = 8，预留头部空间，确保 prepend 不重新分配

---

## 第 5 页：Channel 事件分发器

**标题**：⑤ Channel — 事件分发器

**副标题**：(fd, events, callbacks) 三元组 — EventLoop 只管"有事件 → 调这个 Channel"

**核心设计**：

```
Channel ch(fd, epoll);

// 设置四个回调
ch.SetReadCallback( [...]{ OnRead(fd);  } );
ch.SetWriteCallback([...]{ OnWrite(fd); } );
ch.SetCloseCallback([...]{ OnClose(fd); } );
ch.SetErrorCallback([...]{ OnError(fd); } );

// 启用监听
ch.EnableRead();   // events_ |= EPOLLIN | EPOLLRDHUP → Update() → epoll_ctl(MOD)
ch.EnableWrite();  // events_ |= EPOLLOUT
```

**事件分发优先级**（HandleEvent 内部）：

1. EPOLLHUP（无数据未读）→ closeCallback_
2. EPOLLERR / EPOLLHUP → errorCallback_
3. EPOLLRDHUP（对端半关闭）→ closeCallback_
4. EPOLLIN → readCallback_
5. EPOLLOUT → writeCallback_

**底部对比**：没有 Channel → EventLoop 里 switch(fd) + if(events) 散落各处 → 无法维护

---

## 第 6 页：EventLoop 事件循环核心

**标题**：⑦ EventLoop — 事件循环核心

**副标题**：One Loop Per Thread + eventfd 唤醒 + swap 技巧

**核心循环**：

```
while (!quit_) {
    nfds = epoll_wait(..., 64);     // 阻塞等待
    for (i = 0..nfds) {
        channels_[fd]->HandleEvent(events);  // 分发
    }
    DoPendingFunctors();            // 跨线程任务
}
```

**三个关键设计**：

**① eventfd 跨线程唤醒**：
- 没有 eventfd → `quit_ = true` 没人看到 → epoll_wait 永远阻塞 → 进程卡死
- 有 eventfd → `Quit()` 写 8 字节 → epoll_wait 立即返回 → 安全退出
- 作用类似"门铃"，让其他线程能唤醒本线程的 epoll_wait

**② DoPendingFunctors 的 swap 技巧**：
- 加锁 → `functors.swap(pending_functors_)`（O(1) 交换 3 个指针）→ 解锁
- pending_functors_ 变空 → 其他线程可以安全 enqueue
- functors 在栈上独占 → 无竞争执行

**③ 架构价值**：无锁（fd 不跨线程共享）+ Cache 友好 + 故障隔离

---

## 第 7 页：ThreadPool 线程池

**标题**：⑥ ThreadPool — 当前未使用，但已就绪

**副标题**：为什么没用到 + 什么时候用

**为什么没用上？**

主从 Reactor 天然"一个连接一个线程负责到底"：
read → HTTP 解析 → 生成响应 → write 全在 EventLoop 线程里完成，没有需要卸掉的 CPU 密集任务。

**将来什么时候用？**

- gzip 压缩响应体 → CPU 密集，会阻塞 EventLoop
- 大文件磁盘 IO → 可能阻塞
- CGI / FastCGI 外部进程调用 → 等待时间长

**设计亮点**：
- RAII 析构安全关闭（stop_ + notify_all + join）
- `Submit()` 支持 `std::future` 获取返回值
- 关闭时拒绝新任务（stop_ 检查 → 抛异常）

---

## 第 8 页：HTTP 解析器 状态机

**标题**：⑧ HTTP 解析器 — 状态机跨 TCP 分片

**副标题**：TCP 字节流 + 状态机 + Buffer = 消息边界感知

**核心公式（居中大字）**：
TCP 字节流（无边界）+ 状态机（记住当前位置）+ Buffer（攒数据）= 感知消息边界

**三个状态**：

```
PARSE_REQUEST_LINE  →  找 \r\n → 解析 "GET /index.html HTTP/1.1"
PARSE_HEADERS       →  逐行解析 "Host: localhost" 直到空行 \r\n
PARSE_BODY          →  累积 Content-Length 字节
PARSE_DONE          →  完整的 HttpRequest 对象
```

**NEED_MORE 是精髓**：
数据不够时不报错，保留 Buffer 数据，保持当前状态 → 等下次 EPOLLIN 继续

**对比**：
- 不用状态机 → 第一次 read 拿到 "GET /inde" → 报错？丢弃？→ 第二次 read 接不上
- 用状态机 → 状态停在第 1 步，Buffer 保留，下次继续找 \r\n

---

## 第 9 页：TimerWheel 时间轮

**标题**：⑧ TimerWheel — O(1) 踢空闲连接

**副标题**：60 槽 × 1 秒 = 60 秒超时窗口 · 每个 SubReactor 独立时间轮

**核心问题**：恶意客户端 TCP 连上了但不发 HTTP 请求 → 占着 fd / 内存 / 槽位 → 必须踢掉

**O(N) 遍历 vs O(1) 时间轮**：

| | 遍历所有连接 | 时间轮 |
|--|------------|--------|
| 复杂度 | O(N) | O(1) |
| 1 万连接 | 1 万次检查/秒 | 1 个槽位/秒 |
| 10 万连接 | 10 万次检查/秒 | 1 个槽位/秒 |

**活跃 vs 超时的区分**：
- 活跃连接 → 每次 HTTP 请求后 `AddOrRefresh(fd, 60000)` → 挪到远处 → 永不超时
- 超时连接 → 一直停在原地 → `Tick()` 收割 → `close(fd)` + 释放资源

**集成方式**：
- `timerfd_create` → 1 秒周期 → EPOLLIN 触发
- 每个 SubReactor 独立 timerfd + TimerWheel → 无锁
- `Flush()` 复用同一个 timerfd 回调 → 不额外创建定时器

---

## 第 10 页：AsyncLogger 异步日志

**标题**：⑧ AsyncLogger — 异步日志系统

**副标题**：两层无锁设计 + 双缓冲流水线 · 4 个文件 4 个职责

**四个文件分工**：

| 文件 | 职责 |
|------|------|
| `log_stream.h` | LogStream — 4KB 格式化缓冲区 + operator<< |
| `log_buffer.h` | LogBuffer — 双缓冲（current_ + next_）+ Flush() + swap |
| `async_log_writer.h` | AsyncLogWriter — 后台线程 + 条件变量 + fwrite 写盘 |
| `logger.h` | Logger — RAII + 时间戳前缀 + LOG_INFO/ERROR/FATAL 宏 |

**两层无锁设计**：
1. 每线程独享 LogBuffer（thread_local 绑定）→ 线程间不共享 → 无锁
2. 双缓冲 swap 瞬间切换 current_ ↔ next_ → 前后端物理内存互不干扰

**双缓冲流水线**：

```
前端线程（SubReactor）          后端线程（AsyncLogWriter）
  LOG_INFO << "xxx"               等待条件变量
  → 写入 current_
  → Flush() 时 swap(current_, next_)  → notify
      ↓                                    ↓
  前端继续写 current_（空）         后端写 next_（满）到磁盘
```

**Flush 精妙处**：复用 timerfd 每秒回调，不额外创建定时器。连接关闭时自动触发最后一次 Flush。

---

## 第 11 页：总结

**标题**：✅ 九层架构一览 + 五大设计原则

**九层表**：

| 层 | 模块 | 核心职责 | 一句话 |
|----|------|---------|--------|
| ⑨ | MultiReactorServer | 主从调度 | Accept 轮询 → SubReactor 全程处理 |
| ⑧ | HttpParser | HTTP 状态机 | 跨 TCP 分片攒完整请求 |
| ⑧ | TimerWheel | 空闲连接回收 | O(1) Tick · 活跃刷新远处 · 超时原地收割 |
| ⑧ | AsyncLogger | 双缓冲写盘 | 每线程独享 buffer + 前后端流水线 |
| ⑦ | EventLoop | 事件循环 | One Loop Per Thread + eventfd 唤醒 |
| ⑥ | ThreadPool | 任务队列 | 暂未使用 · 预留 CPU 密集场景 |
| ⑤ | Channel | 事件分发 | (fd, events, callbacks) 三元组 |
| ④ | Buffer | 应用层缓冲 | TCP 字节流 → 完整消息的组装工厂 |
| ③② | Epoll / Socket | RAII 封装 | 移动不拷贝 · 谁创建谁释放 |
| ① | OS 原语 | 内核 | epoll / eventfd / timerfd / readv |

**五大设计原则（底部居中大字）**：
RAII 管理资源 | 移动不拷贝 | One Loop Per Thread | 无锁设计 | swap 掏空技巧

**备注**：讲完这一页留 1-2 分钟给面试官提问，准备回答："你最满意哪个模块的设计？"→ 答 EventLoop 的 eventfd + swap 组合
