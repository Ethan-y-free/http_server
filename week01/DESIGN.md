# Day 5 技术文档：Epoll + Channel 封装 + v2 Echo Server

---

## 一、我们要做什么

把 epoll_echo_server.cpp（面向过程，500 行）拆成三个类 + 一个精简版 main：

```
v1（面向过程）                          v2（面向对象）
─────────────────────────────────────   ───────────────────────────
main() 里一把梭                          main() 4 行
  ├─ 创建 socket                         EchoServer 类
  ├─ 创建 epoll                            ├─ Epoll 对象（管内核 epoll 实例）
  ├─ 事件循环                              ├─ Channel 对象 × N（每个 fd 一个）
  │   ├─ accept 新连接                     └─ 事件循环（5 行）
  │   ├─ recv 数据
  │   ├─ send 回显
  │   └─ 关闭连接
  └─ 清理
```

---

## 二、模块架构

```
┌────────────────────────────────────────────┐
│              EchoServer                     │
│  组合三个对象，编排业务流程                  │
│                                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐ │
│  │  Socket  │  │  Epoll   │  │ Channel  │ │
│  │ (已有)   │  │ (新封装)  │  │ (新封装)  │ │
│  └──────────┘  └──────────┘  └──────────┘ │
└────────────────────────────────────────────┘
```

---

## 三、Epoll 类 API

### 文件：`epoll.h`（你已经写完 ✅）

### 职责
封装 Linux epoll 实例的创建/销毁 + 三个核心系统调用

### API 清单

| 方法 | 对应系统调用 | 说明 |
|------|------------|------|
| `Epoll()` | `epoll_create1(0)` | 构造函数，创建 epoll 实例 |
| `~Epoll()` | `close(epfd)` | 析构，关闭 epoll 实例 |
| `Epoll(Epoll&&)` | — | 移动构造 |
| `Epoll& operator=(Epoll&&)` | — | 移动赋值 |
| `int Fd()` | — | 返回 epoll 的 fd |
| `void Add(fd, events)` | `epoll_ctl(ADD)` | 注册 fd，监听 events |
| `void Mod(fd, events)` | `epoll_ctl(MOD)` | 修改已注册 fd 的监听事件 |
| `void Del(fd)` | `epoll_ctl(DEL)` | 从 epoll 移除 fd |
| `int Wait(buf, max, timeout)` | `epoll_wait()` | 等待事件，返回就绪数量 |

### API 签名

```cpp
class Epoll {
public:
    Epoll();                                                    // 创建 → epoll_create1(0)
    ~Epoll() noexcept;                                          // 关闭 → close(epfd_)

    Epoll(Epoll&& other) noexcept;                              // 移动（接管 fd）
    Epoll& operator=(Epoll&& other) noexcept;                   // 移动赋值
    Epoll(const Epoll&) = delete;                               // 禁止拷贝
    Epoll& operator=(const Epoll&) = delete;

    int  Fd() const noexcept;                                   // 返回 epfd_

    void Add(int fd, uint32_t events);                          // epoll_ctl(ADD)
    void Mod(int fd, uint32_t events);                          // epoll_ctl(MOD)
    void Del(int fd);                                           // epoll_ctl(DEL)

    int  Wait(epoll_event* events, int max, int timeout = -1);  // epoll_wait
    //   返回: >0=就绪数, 0=超时/EINTR, 抛异常=真错

private:
    int epfd_ = -1;
};
```

### 调用示例

```cpp
Epoll ep;
ep.Add(listen_fd, EPOLLIN);                            // 注册 listen fd

epoll_event buf[64];
int n = ep.Wait(buf, 64);                              // 等待事件
for (int i = 0; i < n; i++) {
    int fd = buf[i].data.fd;                           // 谁就绪了
    uint32_t events = buf[i].events;                   // 发生了什么
}
```

---

## 四、Channel 类 API

### 文件：`channel.h`（你正在写 🔄）

### 职责
一个 Channel 对象 = 一个 fd + 它关心什么事件 + 事件到了调什么回调。
Channel 不拥有 fd（不负责 close），只负责向 epoll 注册/修改事件。

### API 清单

#### 构造/析构

| 方法 | 说明 |
|------|------|
| `Channel(fd, epoll*)` | 绑定 fd 和 Epoll 实例 |
| `~Channel()` | 从 epoll 注销自己（不 close fd）|

#### 属性访问

| 方法 | 返回值 |
|------|--------|
| `int Fd()` | fd_ |
| `int Events()` | events_（当前监听的事件位掩码） |

#### 设置回调（4 个 setter）

| 方法 | 对应的事件 | 什么时候触发 |
|------|-----------|------------|
| `SetReadCallback(cb)` | EPOLLIN | fd 可读了 |
| `SetWriteCallback(cb)` | EPOLLOUT | fd 可写了 |
| `SetCloseCallback(cb)` | EPOLLRDHUP | 对端发 FIN 了 |
| `SetErrorCallback(cb)` | EPOLLERR/HUP | 连接异常了 |

#### 启用/禁用事件（4 个 toggle）

| 方法 | 操作 | 效果 |
|------|------|------|
| `EnableRead()` | `events_ \|= EPOLLIN` | 开始监听可读 |
| `EnableWrite()` | `events_ \|= EPOLLOUT` | 开始监听可写 |
| `DisableWrite()` | `events_ &= ~EPOLLOUT` | 停止监听可写 |
| `DisableAll()` | `events_ = 0` | 停止一切监听 |

每个 toggle 最后都调 `Update()` → 通知 epoll 更新。

#### 事件分发

| 方法 | 说明 |
|------|------|
| `HandleEvent(revents)` | 由事件循环调用。根据 revents 调用对应回调 |

#### 内部方法

| 方法 | 说明 |
|------|------|
| `Update()` | 私有。把 events_ 同步到 epoll_。0 则 Del，非 0 则 Mod |

### API 签名

```cpp
class Channel {
public:
    using EventCallback = std::function<void()>;

    Channel(int fd, Epoll* epoll);
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    int  Fd() const noexcept;
    int  Events() const noexcept;

    void SetReadCallback(EventCallback cb);
    void SetWriteCallback(EventCallback cb);
    void SetCloseCallback(EventCallback cb);
    void SetErrorCallback(EventCallback cb);

    void EnableRead();
    void EnableWrite();
    void DisableWrite();
    void DisableAll();

    void HandleEvent(uint32_t revents);

private:
    void Update();

    int      fd_      = -1;
    Epoll*   epoll_   = nullptr;     // 不拥有，不负责 delete
    uint32_t events_  = 0;

    EventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};
```

### 调用示例

```cpp
// 1. 创建
Channel ch(client_fd, &ep);

// 2. 设回调（lambda 或函数指针）
ch.SetReadCallback( [&]() { recv(fd, buf, ...); } );
ch.SetCloseCallback( [&]() { close(fd); } );

// 3. 启用事件
ch.EnableRead();      // epoll 开始监听这个 fd 的 EPOLLIN

// 4. 事件循环里用
ep.Wait(buf, 64);
for (...) {
    ch.HandleEvent(buf[i].events);   // Channel 自己判断发生了什么，调对应回调
}
```

---

## 五、v2 EchoServer 需求

### 文件：`epoll_echo_server_v2.cpp`

### 功能
和 epoll_echo_server.cpp 完全一样：监听 8888 端口，多客户端并发 echo。

### 需要实现的方法

| 方法 | 功能 | 输入 | 输出 |
|------|------|------|------|
| `EchoServer(port)` | 初始化 | 端口号 | — |
| `~EchoServer()` | 清理（先删 channel 再关 socket） | — | — |
| `Start()` | 事件循环 | — | — |
| `OnAccept()` | 接受新连接 | — | — |
| `OnRead(fd)` | 读数据+回显 | fd | — |
| `OnClose(fd)` | 关闭连接 | fd | — |
| `OnError(fd)` | 错误处理 | fd | — |

### 成员变量

```cpp
Socket listen_sock_;                                 // 监听 socket
std::unique_ptr<Epoll> epoll_;                       // epoll 实例
std::unique_ptr<Channel> listen_channel_;            // 监听 fd 的 Channel

std::unordered_map<int, Socket> client_socks_;       // fd → Socket
std::unordered_map<int, std::unique_ptr<Channel>> client_channels_;  // fd → Channel
```

### 各方法实现要点

```
EchoServer(uint16_t port):
  1. listen_sock_ 设 ReuseAddr → Bind(port) → Listen → SetNonBlocking
  2. epoll_ = make_unique<Epoll>()
  3. listen_channel_ = make_unique<Channel>(listen_sock_.Fd(), epoll_.get())
  4. listen_channel_->SetReadCallback → 绑定 OnAccept
  5. listen_channel_->EnableRead()                                    // 注册 EPOLLIN

Start():
  epoll_event buf[64];
  while (true) {
      int n = epoll_->Wait(buf, 64, -1);
      for (i = 0..n-1) {
          fd = buf[i].data.fd
          if (fd == listen_sock_.Fd())
              listen_channel_->HandleEvent(buf[i].events);           // 新连接
          else if (client_channels_.contains(fd))
              client_channels_[fd]->HandleEvent(buf[i].events);      // 客户端数据
      }
  }

OnAccept():
  1. Socket client = listen_sock_.Accept(ip, port)
  2. client.SetNonBlocking()
  3. client_fd = client.Fd()
  4. 先 epoll_->Add(client_fd, EPOLLIN | EPOLLRDHUP)               // 加到 epoll
  5. auto ch = make_unique<Channel>(client_fd, epoll_.get())
  6. ch->SetReadCallback → [this,fd]{ OnRead(fd); }
  7. ch->SetCloseCallback → [this,fd]{ OnClose(fd); }
  8. ch->SetErrorCallback → [this,fd]{ OnError(fd); }
  9. ch->EnableRead()                                               // 同步到 epoll
  10. client_socks_[client_fd] = move(client)
  11. client_channels_[client_fd] = move(ch)

OnRead(fd):
  1. 从 client_socks_ 找 Socket
  2. recv() 读数据
  3. n>0 → send() 回显
  4. n<=0 → OnClose(fd)

OnClose(fd):
  1. print 断开日志
  2. client_channels_.erase(fd)    ← 先删 Channel（析构 → epoll Del）
  3. client_socks_.erase(fd)       ← 再删 Socket（析构 → close fd）

OnError(fd):
  1. print 错误日志
  2. 调 OnClose(fd)                 ← 同关闭处理
```

### 清理顺序（重要！）

```
先删 Channel → 再删 Socket

原因：
  Channel 析构 → epoll_->Del(fd)    ← 从 epoll 注销
  Socket 析构 → close(fd)            ← 释放 fd

  反过来：先 close(fd) → epoll_ctl(DEL, 已关闭的fd) → 行为未定义
```

---

## 六、Buffer 类 API（Day 6）

### 文件：`buffer.h` ✅

### 职责
在应用层缓冲 TCP 字节流，解决粘包/半包问题。把"从 socket 读"和"解析业务数据"解耦。

### 内存布局

```
| prependable |  readable  |  writable  |
|   (8字节)   | (未消费数据) |  (可写空间) |
^             ^            ^             ^
data()        readIndex_   writeIndex_   data()+size()

readIndex_  = 读指针（从这里开始是可读数据）
writeIndex_ = 写指针（从这里开始可以写）
```

`kCheapPrepend = 8` 字节的 prependable 区允许事后在前面插入数据——比如 HTTP 响应先写 body，回头补 Content-Length 头。

### API 清单

#### 构造

| 方法 | 说明 |
|------|------|
| `Buffer()` | 初始容量 = 8 + 1024 = 1032 字节 |

#### 属性（读状态）

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `Peek()` | `const char*` | 可读区起始指针 |
| `ReadableBytes()` | `size_t` | 可读字节数 = writeIndex_ - readIndex_ |
| `WritableBytes()` | `size_t` | 可写字节数 = size() - writeIndex_ |
| `PrependableBytes()` | `size_t` | 可在前面插入的字节数 = readIndex_ |

#### 写入

| 方法 | 说明 |
|------|------|
| `Append(data, len)` | 追加数据到写区末尾（触发扩容） |
| `Append(string)` | 同上，std::string 版本 |
| `Prepend(data, len)` | 在可读数据**前面**插入（readIndex_ 前移） |
| `beginWrite()` | 返回写区起始指针（供直接写入） |
| `HasWritten(len)` | 移动 writeIndex_（配合 beginWrite 使用） |

#### 消费（取走数据）

| 方法 | 说明 |
|------|------|
| `Retrieve(len)` | 消费 len 字节（readIndex_ 后移）；全消费时指针复位 |
| `RetrieveUntil(end)` | 消费到指定指针位置 |
| `RetrieveAll()` | 消费全部（readIndex_ = writeIndex_ = 8） |
| `RetrieveAsString(len)` | 取走 len 字节返回 std::string |
| `RetrieveAllAsString()` | 取走全部返回 std::string |

#### 核心：从 socket 读

| 方法 | 说明 |
|------|------|
| `ReadFd(fd, &savedErrno)` | **Buffer 的核心方法**。用 `readv` 一次系统调用读到两个内存块，返回读取字节数 |

`ReadFd` 工作原理：

```
readv(fd, iov, 2)
  iov[0] = {buffer可写区, 可写大小}     ← 优先填 buffer
  iov[1] = {栈上临时buf, 65536}        ← 数据太多时兜底

场景1: 数据 ≤ WritableBytes  → 直接进 buffer，不经过栈
场景2: 数据 > WritableBytes  → 填满 buffer，剩余进 extrabuf，再 Append
```

优势：比"先读到栈上 buf，再拷进 Buffer"少一次内存拷贝。

#### 扩容

| 方法 | 说明 |
|------|------|
| `EnsureWritableBytes(len)` | 保证写区 ≥ len。先尝试前移可读数据腾空间，不够再 resize |

扩容策略：

```
1. WritableBytes() >= len  → 直接返回
2. readIndex_ + WritableBytes() >= len + kCheapPrepend
   → 把可读数据 memmove 到前面，合并碎片空间（不调 alloc）
3. → resize(writeIndex_ + len)（调 alloc）
```

#### 工具

| 方法 | 说明 |
|------|------|
| `Swap(rhs)` | 交换两个 Buffer（O(1)，只交换指针） |
| `FindCRLF()` | 在可读区查找 `\r\n`，返回首次出现位置（HTTP 协议解析用） |
| `Shrink()` | 释放多余空间 |
| `InternalCapacity()` | 返回实际 capacity（调试用） |

### API 签名

```cpp
class Buffer {
public:
    static constexpr size_t kCheapPrepend = 8;
    static constexpr size_t kInitialSize  = 1024;

    Buffer();

    // 属性
    size_t      ReadableBytes() const;
    size_t      WritableBytes() const;
    size_t      PrependableBytes() const;
    const char* Peek() const;

    // 消费
    void        Retrieve(size_t len);
    void        RetrieveUntil(const char* end);
    void        RetrieveAll();
    std::string RetrieveAsString(size_t len);
    std::string RetrieveAllAsString();

    // 写入
    void        Append(const char* data, size_t len);
    void        Append(const std::string& str);
    void        Prepend(const void* data, size_t len);

    // 扩容
    void        EnsureWritableBytes(size_t len);

    // 写指针
    char*       beginWrite();
    void        HasWritten(size_t len);

    // 核心：从 socket 读
    ssize_t     ReadFd(int fd, int* savedErrno);

    // 工具
    void        Swap(Buffer& rhs);
    const char* FindCRLF() const;
    void        Shrink();
    size_t      InternalCapacity() const;

private:
    std::vector<char> buffer_;
    size_t readIndex_  = 0;
    size_t writeIndex_ = 0;
};
```

### 调用示例

```cpp
Buffer buf;

// 从 socket 读
int savedErrno = 0;
ssize_t n = buf.ReadFd(client_fd, &savedErrno);
if (n > 0) {
    // buf 里现在有 n 字节可读数据
}

// 解析 HTTP 请求行：找 \r\n
const char* crlf = buf.FindCRLF();
if (crlf) {
    std::string requestLine = buf.RetrieveAsString(crlf - buf.Peek());
    buf.Retrieve(2);  // 跳过 \r\n
}

// 回显：Peek 不消费，Send 后 Retrieve
ssize_t sent = send(fd, buf.Peek(), buf.ReadableBytes(), 0);
if (sent > 0) {
    buf.Retrieve(sent);
}
```

---

## 七、文件清单与依赖关系

```
week01/
├── socket_raii.h              ✅ Socket RAII 类
├── epoll.h                    ✅ Epoll RAII 封装
├── channel.h                  ✅ Channel 事件分发器
├── buffer.h                   ✅ Buffer 应用层缓冲区（Day 6 新增）
├── tcp_server.cpp             ✅ 阻塞式 echo server
├── tcp_client.cpp             ✅ 阻塞式 echo client
├── epoll_echo_server.cpp      ✅ epoll echo server（面向过程）
├── epoll_echo_server_v2.cpp   ✅ epoll echo server（OO 版）
├── CMakeLists.txt             ✅
└── reference/
    ├── epoll_full.h
    ├── channel_full.h
    ├── socket_raii_full.h
    ├── epoll_echo_server_full.cpp
    ├── epoll_echo_server_v2_full.cpp
    └── epoll_echo_server_v3_buffer_full.cpp  ← Day 7 新增
    ├── tcp_server_full.cpp
    └── tcp_client_full.cpp
```

依赖关系：
```
epoll_echo_server_v2.cpp
  ├── socket_raii.h
  ├── epoll.h        ───→  channel.h ──→ epoll.h
  └── buffer.h             （独立，仅依赖 <vector>/<sys/uio.h>）
```

---

## 八、Buffer 集成设计（Day 7 — 6/18）

### 8.1 目标

把 Buffer 类集成到 epoll echo server，解决两个问题：

| 问题 | v2 现状 | v3 改进 |
|------|---------|---------|
| 栈缓冲区 | `char buf[4096]` 在 OnRead 栈上 | 每个连接持有 `Buffer`（可在堆上，活得和连接一样长） |
| 非阻塞写 | send EAGAIN 直接 return，**数据丢失** | 数据暂存 outputBuffer → EnableWrite → OnWrite 继续发 |

### 8.2 新成员：Connection 结构体

v2 用两个 map 分别管理 Socket 和 Channel：

```cpp
// v2（旧）
std::unordered_map<int, Socket> client_socks_;
std::unordered_map<int, std::unique_ptr<Channel>> client_channels_;
```

v3 打包成一个结构体，一个 map 管所有：

```cpp
// v3（新）
struct Connection
{
    Socket sock;                        // ① 最后析构（close fd）
    std::unique_ptr<Channel> channel;   // ② 倒数第二析构（epoll Del）
    Buffer inputBuffer;                 // ③ 输入缓冲
    Buffer outputBuffer;                // ④ 输出缓冲（非阻塞写用）
};

std::unordered_map<int, Connection> clients_;
```

**析构顺序保证**：成员按声明逆序析构 → `outputBuffer` → `inputBuffer` → `channel`（epoll Del）→ `sock`（close fd），天然满足"先删 Channel 再关 Socket"的要求。

### 8.3 数据流

```
客户端发送 "hello\n"
        │
        ▼
   EPOLLIN 触发 OnRead(fd)
        │
        ▼
   inputBuffer.ReadFd(fd)   ← 用 readv 一次读到 buffer（或栈兜底）
        │
        ▼
   inputBuffer → outputBuffer   （echo：把收到的拷到发送区）
        │
        ▼
   FlushWrite(fd)
        │
        ├─ send() 成功 → outputBuffer.Retrieve(sent)
        │   └─ 全发完 → channel->DisableWrite()
        │
        └─ send() 返回 EAGAIN → channel->EnableWrite()
               │
               ▼
          下次 EPOLLOUT 触发 OnWrite(fd) → FlushWrite(fd) 继续发
```

### 8.4 核心方法：FlushWrite

这是非阻塞写的关键。**不是**在 `OnRead` 里直接 `send()`，而是写到一个可重试的辅助方法：

```
FlushWrite(fd):
  while (conn.outputBuffer.ReadableBytes() > 0):
    sent = conn.sock.Send(outputBuffer.Peek(), outputBuffer.ReadableBytes())
    
    if sent > 0:
      outputBuffer.Retrieve(sent)           // 发多少，消费多少
      
    else if sent < 0 && errno == EAGAIN:
      conn.channel->EnableWrite()           // 注册 EPOLLOUT，等内核通知再发
      return                                // 不关连接，不丢数据
      
    else:
      OnClose(fd)                           // 真错或对端关闭
      return
  
  // while 正常结束 = 全发完
  conn.channel->DisableWrite()              // 关掉 EPOLLOUT，省 CPU
```

**为什么用 while 循环？** send() 可能只发出去一部分（TCP 滑动窗口满了），必须循环到全发完或 EAGAIN。

### 8.5 各方法变化清单

#### 构造 `EchoServer(port)` —— 不变

#### `Start()` —— 不变

事件循环不变，但 `HandleEvent` 现在会正确路由 EPOLLOUT 到 `OnWrite`（Channel 已有 `writeCallback_` 支持）。

#### `OnAccept()` —— 小改

```
1. Socket client = listen_sock_.Accept(ip, port)
2. client.SetNonBlocking()
3. client_fd = client.Fd()
4. auto conn = Connection{...}   ← 新：构造 Connection（含两个空 Buffer）
5. conn.channel = make_unique<Channel>(client_fd, epoll_.get())
6. 设置四个回调：ReadCallback / WriteCallback / CloseCallback / ErrorCallback
7. epoll_->Add(client_fd, EPOLLIN | EPOLLRDHUP)  ← 初始不监听 EPOLLOUT
8. clients_.emplace(client_fd, std::move(conn))
```

#### `OnRead(fd)` —— 核心重写

```
1. auto it = clients_.find(fd)
2. int savedErrno = 0
3. ssize_t n = it->second.inputBuffer.ReadFd(fd, &savedErrno)  ← 关键变化
4. if n>0:
     // echo：拷到 outputBuffer
     conn.outputBuffer.Append(conn.inputBuffer.Peek(), conn.inputBuffer.ReadableBytes())
     conn.inputBuffer.RetrieveAll()
     FlushWrite(fd)   ← 尝试发送（可能 EAGAIN）
5. if n==0: OnClose(fd)     ← 对端关闭
6. if n<0 && EAGAIN: return ← 没数据（非阻塞正常）
7. if n<0 && 其他错误: OnClose(fd)
```

#### `OnWrite(fd)` —— 新增

```
1. FlushWrite(fd)   ← EPOLLOUT 触发时，继续把 outputBuffer 发完
```

#### `OnClose(fd)` —— 简化

```
1. print 日志
2. clients_.erase(fd)   ← Connection 析构：Channel 先析构（epoll Del），Socket 后析构（close fd）
```

#### `OnError(fd)` —— 不变

### 8.6 非阻塞写完整时序

这是今天的核心理解点。看一个具体例子：

```
假设客户端发了 8KB 数据，但 TCP 发送缓冲区只剩 3KB 空间。

时间线：
─────────────────────────────────────────────────────────
① EPOLLIN → OnRead(fd)
  inputBuffer.ReadFd() 读到 8KB
  拷到 outputBuffer（8KB 待发）
  FlushWrite(fd):
    send() 第1次 → 发出 3KB（滑窗满了）
    outputBuffer.Retrieve(3KB)  → 剩余 5KB
    send() 第2次 → -1, EAGAIN
    channel->EnableWrite()  → 注册 EPOLLOUT
  return（OnRead 结束，不丢数据！）

② （TCP 滑窗有空了）EPOLLOUT → OnWrite(fd)
  FlushWrite(fd):
    send() → 发出 5KB
    outputBuffer.Retrieve(5KB) → 剩余 0
  channel->DisableWrite()
─────────────────────────────────────────────────────────
```

**对比 v2 的问题**：v2 遇到 EAGAIN 只 `return`，buf 里的数据下次循环就丢了 — 因为 buf 是栈上局部变量，OnRead 返回就释放了。

### 8.7 要点总结

| 要点 | 说明 |
|------|------|
| `Buffer::ReadFd` | 用 readv 一次读到 buffer + 栈兜底，比 "栈→拷进 Buffer" 少一次拷贝 |
| `FlushWrite` | 可重试的发送逻辑，EAGAIN 时注册 EPOLLOUT 而不是丢数据 |
| EnableWrite/DisableWrite | 按需打开 EPOLLOUT，平时不监听（避免 busy loop） |
| Connection 析构顺序 | 成员声明顺序保证 Channel 先于 Socket 析构 |
| while(send) | send 可能只发一部分，必须循环 |

### 8.8 你要改的文件

1. **修改** `week01/epoll_echo_server_v2.cpp`：按上面设计替换 OnRead + 新增 Connection 结构体 + 新增 OnWrite/FlushWrite
2. **新增依赖**：`#include "buffer.h"`
3. **删除**：`BUF_SIZE` 常量、两个 client map

### 8.9 测试方法

```bash
# 编译
g++ -std=c++17 epoll_echo_server_v2.cpp -o epoll_echo_server_v2

# 基本测试
nc localhost 8888

# 大量数据测试（验证非阻塞写）
cat /dev/urandom | head -c 100000 | nc localhost 8888 > /dev/null

# 多客户端
# 开3个终端，每个 nc localhost 8888，同时发数据
```

### 8.10 参考文件

完整参考实现：`reference/epoll_echo_server_v3_buffer_full.cpp`
实现遇到困难时对照参考，但**先自己写**。

## 九、EventLoop v1 实现设计（Day 9 — 6/19 晚）

### 9.1 目标

把 v3 `EchoServer::Start()` 里的事件循环逻辑抽成一个可复用的类。

**v1 范围**：比 v3 多三样 — 能停 + 有门铃 + fd→Channel 映射。**不加** pending_functors / RunInLoop（Day 11 再扩展）。

### 9.2 新建文件

`week01/eventloop.h`

依赖：
```cpp
#include <sys/eventfd.h>
#include <unistd.h>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "epoll.h"
#include "channel.h"
```

### 9.3 API 签名

```cpp
class EventLoop
{
public:
    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // ===== 核心 =====
    void Loop();                              // 启动事件循环，阻塞当前线程
    void Quit();                              // 退出循环（线程安全）

    // ===== 线程检查 =====
    bool IsInLoopThread() const;              // 当前线程 == 绑定的线程？
    void AssertInLoopThread();                // 不是就 assert 炸掉

    // ===== Channel 管理（由 Channel 调用） =====
    void UpdateChannel(Channel* ch);          // epoll_ctl(MOD)
    void RemoveChannel(Channel* ch);          // epoll_ctl(DEL) + 从 map 移除

private:
    void HandleWakeup();                      // wakeup_fd_ 可读时的回调
    int  CreateWakeupFd();                    // eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)

    const std::thread::id tid_;                     // 绑定的线程 ID
    std::unique_ptr<Epoll> epoll_;                  // 自己的 epoll 实例

    bool looping_ = false;                          // 是否正在 Loop() 里
    std::atomic<bool> quit_ = false;                // 是否请求退出

    // 唤醒机制
    int wakeup_fd_ = -1;                            // eventfd
    std::unique_ptr<Channel> wakeup_channel_;       // 把 wakeup_fd_ 注册到 epoll

    // fd → Channel* 映射（Loop 里拿到就绪 fd 后能找到对应的 Channel）
    std::unordered_map<int, Channel*> channels_;
};
```

### 9.4 各方法实现要点

#### 构造函数

```
EventLoop():
  1. tid_ = std::this_thread::get_id()
  2. epoll_ = std::make_unique<Epoll>()
  3. wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)
     如果 < 0，抛异常
  4. wakeup_channel_ = std::make_unique<Channel>(wakeup_fd_, epoll_.get())
  5. wakeup_channel_->SetReadCallback([this]() { HandleWakeup(); })
  6. wakeup_channel_->EnableRead()
```

#### 析构函数

```
~EventLoop():
  if (wakeup_fd_ >= 0)
      ::close(wakeup_fd_)
```

#### Loop()

```
Loop():
  1. AssertInLoopThread()          // 必须在自己的线程
  2. looping_ = true
  3. quit_ = false

  4. while (!quit_)
     {
         epoll_event buf[64];
         int nfds = epoll_->Wait(buf, 64, -1);    // 阻塞等待

         for (int i = 0; i < nfds; i++)
         {
             int fd = buf[i].data.fd;
             auto it = channels_.find(fd);
             if (it != channels_.end())
             {
                 it->second->HandleEvent(buf[i].events);
             }
         }
     }

  5. looping_ = false
```

**跟 v3 Start() 的区别**：
- `while (!quit_)` 代替 `while (true)` — 能停
- `channels_[fd]->HandleEvent()` 代替 `if/else` — 通用分发
- **没有 DoPendingFunctors()** — v1 暂不需要

#### Quit()

```
Quit():
  1. quit_ = true

  2. if (!IsInLoopThread())
     {
         uint64_t one = 1;
         ::write(wakeup_fd_, &one, sizeof(one));   // 敲门唤醒
     }
```

**逻辑**：设置退出标志。如果调用者在别的线程，epoll_wait 正睡着，必须往 eventfd 写数据把它敲醒——否则它不知道该退了。

如果调用者就在 EventLoop 线程，不需要 write，下一轮 while 自然看到 quit_=true。

#### IsInLoopThread / AssertInLoopThread

```
IsInLoopThread():
  return tid_ == std::this_thread::get_id()

AssertInLoopThread():
  assert(IsInLoopThread())
```

#### HandleWakeup

```
HandleWakeup():
  uint64_t one = 0;
  ::read(wakeup_fd_, &one, sizeof(one));    // 消费事件，让它恢复不可读
```

**为什么 v1 需要这个**：wakeup_fd_ 被 Quit() 写入了数据变成可读。如果不 read 消费掉，下一轮 epoll_wait 会立刻返回（wakeup_fd_ 一直可读），造成 busy loop。

#### UpdateChannel

```
UpdateChannel(Channel* ch):
  AssertInLoopThread()
  epoll_->Mod(ch->Fd(), ch->Events())
```

#### RemoveChannel

```
RemoveChannel(Channel* ch):
  AssertInLoopThread()
  1. channels_.erase(ch->Fd())
  2. epoll_->Del(ch->Fd())
```

### 9.5 v1 不包含（Day 11 再加）

| 不加的内容 | 为什么 |
|-----------|--------|
| `RunInLoop(cb)` | 还没 ThreadPool，没有跨线程委托 |
| `QueueInLoop(cb)` | 同上 |
| `DoPendingFunctors()` | 同上 |
| `pending_functors_` + `mutex_` | 同上 |
| `active_channels_` | 直接遍历 buf 就够了，不需要额外保存 |

### 9.6 编译验证

```bash
# 只验证能编译（还没集成到 echo server）
g++ -std=c++17 -c eventloop.h -o /dev/null

# 或写一个简单的 main 测试：创建 EventLoop，调 Loop，另一个线程调 Quit
```

### 9.7 参考文件

`reference/reactor_eventloop_ref.h` — 完整版的类声明骨架，包含了 v1 不需要的 RunInLoop/QueueInLoop/pending_functors_。**v1 只取其中你需要的部分。**

---

## 十、EventLoop v2 完整版（Day 9 — 升级）

### 10.1 目标

在 v1 基础上新增跨线程委托能力。v1 够用于单线程 echo server，v2 为 Day 11 单 Reactor + ThreadPool 做准备。

**v2 改动**：以 v1 代码为基础，不删任何东西，只追加。

### 10.2 新增成员变量

```cpp
// 跨线程回调队列
std::mutex mutex_;
std::vector<Functor> pending_functors_;      // 其他线程塞进来的任务（guarded by mutex_）

// 就绪 Channel 暂存
std::vector<Channel*> active_channels_;      // 每轮 Wait 后用，不用每次 new
```

### 10.3 新增类型别名

```cpp
public:
    using Functor = std::function<void()>;
```

### 10.4 新增公有方法

```cpp
// 在 EventLoop 线程执行 cb。线程安全。
// 如果在 EventLoop 线程 → 直接执行
// 如果在其他线程 → 入队，唤醒 EventLoop
void RunInLoop(Functor cb);

// 入队并唤醒（即便在 EventLoop 线程也入队，不直接执行）
void QueueInLoop(Functor cb);
```

### 10.5 新增私有方法

```cpp
void DoPendingFunctors();   // 在 Loop() 末尾调用，执行 pending_functors_ 里的回调
```

### 10.6 各方法实现要点

#### 构造函数 —— 不变

v1 的构造函数不变。

#### Loop() —— 改动一处

```
Loop():
  1. AssertInLoopThread()
  2. looping_ = true; quit_ = false

  3. while (!quit_)
     {
         active_channels_.clear();                    // 新增

         int nfds = epoll_->Wait(buf, 64, -1);

         for (i = 0; i < nfds; i++)
         {
             fd = buf[i].data.fd;
             Channel* ch = channels_[fd];             // 跟 v1 一样
             ch->HandleEvent(buf[i].events);
         }

         DoPendingFunctors();                         // 新增：每轮末尾执行委托
     }

  4. looping_ = false
```

#### RunInLoop

```
RunInLoop(Functor cb):
  1. if (IsInLoopThread())
     {
         cb();                    // 就在本线程 → 直接执行
     }
     else
     {
         QueueInLoop(std::move(cb));  // 其他线程 → 入队唤醒
     }
```

#### QueueInLoop

```
QueueInLoop(Functor cb):
  1. {
         std::lock_guard<std::mutex> lock(mutex_);
         pending_functors_.push_back(std::move(cb));
     }

  2. if (!IsInLoopThread())
     {
         uint64_t one = 1;
         ::write(wakeup_fd_, &one, sizeof(one));   // 敲门
     }
```

**为什么 IsInLoopThread 时也要 QueueInLoop？** 如果调用者就在 EventLoop 线程，但不在 Loop 的 `for` 循环里（比如在 `DoPendingFunctors` 的回调中间），不需要 write——Loop 末尾自然会调 `DoPendingFunctors()`。但 `pending_functors_` 还是要 push，保证执行时机正确。

#### DoPendingFunctors

```
DoPendingFunctors():
  1. std::vector<Functor> functors;     // 临时容器

  2. {
         std::lock_guard<std::mutex> lock(mutex_);
         functors.swap(pending_functors_);   // 交换：拿走队列，尽快释放锁
     }

  3. for (auto& f : functors)
     {
         f();                           // 在 EventLoop 线程逐个执行
     }
```

**为什么 swap 而不是直接遍历？** 直接遍历 pending_functors_ 期间持锁，如果某个回调很慢，其他线程的 RunInLoop 都被锁挡在外面。swap 后释放锁再执行，不影响其他线程塞任务。

#### HandleWakeup —— 不变

v1 的 HandleWakeup 不变（只 read 消费）。因为 DoPendingFunctors 在 Loop() 末尾自动调用，不需要在这里触发。

#### Quit / UpdateChannel / RemoveChannel / IsInLoopThread / AssertInLoopThread —— 全部不变

### 10.7 v2 完整 API 一览

```cpp
class EventLoop
{
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // 核心
    void Loop();

    // 停止（线程安全）
    void Quit();

    // 线程检查
    bool IsInLoopThread() const;
    void AssertInLoopThread();

    // 跨线程委托 ← 新增
    void RunInLoop(Functor cb);
    void QueueInLoop(Functor cb);

    // Channel 管理
    void UpdateChannel(Channel* ch);
    void RemoveChannel(Channel* ch);

private:
    void HandleWakeup();
    int  CreateWakeupFd();
    void DoPendingFunctors();          // ← 新增

    const std::thread::id tid_;
    std::unique_ptr<Epoll> epoll_;
    bool looping_ = false;
    std::atomic<bool> quit_ = false;

    // 唤醒
    int wakeup_fd_ = -1;
    std::unique_ptr<Channel> wakeup_channel_;

    // fd → Channel 映射
    std::unordered_map<int, Channel*> channels_;

    // 跨线程委托 ← 新增
    std::mutex mutex_;
    std::vector<Functor> pending_functors_;
};
```

### 10.8 v1 → v2 改动清单

| 项目 | 类型 | 说明 |
|------|------|------|
| `using Functor = ...` | 新增 | 类型别名 |
| `RunInLoop` | 新增 | 跨线程委托执行 |
| `QueueInLoop` | 新增 | 入队 + 唤醒 |
| `DoPendingFunctors` | 新增 | Loop 末尾执行 pending |
| `mutex_` | 新增 | 保护 pending_functors_ |
| `pending_functors_` | 新增 | 跨线程任务队列 |
| `Loop()` | 修改 | 末尾加 `DoPendingFunctors()` |
| `#include <mutex>` | 新增 | 头文件依赖 |

就这些。在 v1 基础上加，已有的代码不动。

---

## 十一、ThreadPool 设计（Day 10 — 6/21）

### 11.1 目标

为单 Reactor 提供线程池，将耗时计算从 IO 线程拆出去。EventLoop 线程只做网络读写，ThreadPool 做 HTTP 解析和业务逻辑。

### 11.2 新建文件

`week01/threadpool.h`

依赖：
```cpp
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <condition_variable>
```

### 11.3 API 签名

```cpp
class ThreadPool
{
public:
    using Task = std::function<void()>;

    explicit ThreadPool(size_t numThreads);   // 创建并启动 numThreads 个工作线程
    ~ThreadPool();                            // 等待所有任务完成，停止线程

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // ===== 提交任务 =====

    // 提交一个任务（无返回值）
    void Run(Task task);

    // 提交一个任务（返回 future，可获取结果）
    template <typename F, typename... Args>
    auto Submit(F&& f, Args&&... args) -> std::future<decltype(f(args...))>;

private:
    void WorkerLoop();    // 工作线程的循环函数

    std::vector<std::thread> workers_;

    std::queue<Task> tasks_;         // 任务队列（guarded by mutex_）
    std::mutex mutex_;
    std::condition_variable cv_;

    bool stop_ = false;              // 停止标志
};
```

### 11.4 各方法实现要点

#### 构造函数

```
ThreadPool(size_t numThreads):
  1. stop_ = false

  2. for (i = 0; i < numThreads; i++)
     {
         workers_.emplace_back([this]() { WorkerLoop(); });
     }
```

#### 析构函数

```
~ThreadPool():
  1. {
         std::lock_guard lock(mutex_);
         stop_ = true;
     }

  2. cv_.notify_all();              // 唤醒所有在等的 worker

  3. for (auto& w : workers_)
     {
         w.join();                  // 等所有 worker 退出
     }
```

#### WorkerLoop

```
WorkerLoop():
  1. while (true)
     {
         Task task;

         {
             std::unique_lock lock(mutex_);
             cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
             // 醒来：要么该停了，要么有活干了

             if (stop_ && tasks_.empty())
             {
                 return;            // 停 + 没活 → 退出
             }

             task = std::move(tasks_.front());
             tasks_.pop();
         }

         task();                    // 不持锁执行
     }
```

#### Run

```
Run(Task task):
  1. {
         std::lock_guard lock(mutex_);
         tasks_.push(std::move(task));
     }

  2. cv_.notify_one();              // 唤醒一个 worker
```

### 11.5 和 EventLoop 的关系

```
EventLoop 线程                     ThreadPool Worker 线程
══════════════                     ════════════════════

OnRead(fd)                         WorkerLoop()
{                                   {
  input.ReadFd(fd);                   cv_.wait();  // 等活
                                      
  pool->Run([=]{            ────→     // 醒来，取任务
      解析 HTTP 请求                    解析 HTTP 请求
      读磁盘文件                        读磁盘文件
      生成响应                          生成响应
                                        
      loop->RunInLoop([=]{  ────→     // 通知 EventLoop 发送
          FlushWrite(fd);                  
      });                           });
  });                                 cv_.wait();
}  // OnRead 立刻返回，不阻塞        }

                                  // 两个线程各跑各的，互不阻塞
```

### 11.6 ThreadPool vs EventLoop 的跨线程机制对比

| | ThreadPool | EventLoop |
|--------|------------|------------|
| 等什么 | `cv_.wait()` 等任务 | `epoll_wait()` 等 fd |
| 唤醒方式 | `cv_.notify_one()` | `::write(wakeup_fd_, ...)` |
| 任务队列 | `std::queue<Task>` + `mutex_` | `std::vector<Functor>` + `mutex_` |
| 执行时机 | 谁抢到谁执行（并行） | EventLoop 线程串行执行 |

### 11.7 参考文件

`reference/threadpool_ref.h` — 完整参考实现。

---

## 十二、单 Reactor 拼装：EventLoop + ThreadPool + EchoServer（Day 11 — 6/27）

### 12.1 目标

把前三天攒的组件拼成一个完整的单 Reactor 服务器：

```
EventLoop（IO 线程）         ThreadPool（计算线程）
═══════════════════          ═══════════════════
                                    
epoll_wait() 等 fd           cv_.wait() 等任务
  │                              │
  ├─ OnAccept                    │
  ├─ OnRead ──→ pool->Run() ──→  解析/计算
  │                              │
  └─ RunInLoop(send) ←──────── loop->RunInLoop(响应)
```

**跟 v3 的核心区别**：

| | v3 EchoServer | v4 单 Reactor |
|---|---|---|
| 事件循环 | `Start()` 里自己写 `while(true)` | 交给 `EventLoop::Loop()` |
| 业务计算 | 在 OnRead 里直接 echo（同步） | 提交到 ThreadPool（异步） |
| 结果发送 | OnRead 里直接 FlushWrite | ThreadPool → RunInLoop → FlushWrite |
| 线程模型 | 单线程 | 1 IO + N 计算 |
| 能停吗 | 不能（`while(true)`） | 能（`Quit()`） |

### 12.2 EventLoop 小改动：新增 AddChannel

当前 EventLoop 只有 `UpdateChannel`（调 `epoll_->Mod`）和 `RemoveChannel`（调 `epoll_->Del`），缺少首次注册。新增一个 `AddChannel` 方法：

```cpp
// 新增公有方法（加在 UpdateChannel 前面）
void AddChannel(Channel* ch)
{
    AssertInLoopThread();
    int fd = ch->Fd();
    channels_[fd] = ch;              // 记录 fd→Channel 映射
    epoll_->Add(fd, ch->Events());   // 注册到内核 epoll
}
```

**为什么不在 UpdateChannel 里合并 ADD/MOD？**
- 语义清晰：Add = 第一次注册，Update = 事件变更
- channels_ map 只在 Add 时插入，Remove 时删除
- 跟 muduo 的 `updateChannel`（自动判断 ADD/MOD）略有不同，但更显式，不易出错

### 12.3 新建文件

`week01/single_reactor_server.cpp`

依赖：
```cpp
#include "socket_raii.h"
#include "epoll.h"
#include "channel.h"
#include "buffer.h"
#include "eventloop.h"
#include "threadpool.h"

#include <iostream>
#include <string>
#include <unordered_map>
#include <memory>
#include <thread>
#include <chrono>
```

### 12.4 Server 类设计

```cpp
class SingleReactorServer
{
public:
    SingleReactorServer(EventLoop* loop, uint16_t port, ThreadPool* pool);
    ~SingleReactorServer();

    // 禁止拷贝/移动
    SingleReactorServer(const SingleReactorServer&) = delete;
    SingleReactorServer& operator=(const SingleReactorServer&) = delete;

private:
    void OnAccept();
    void OnRead(int fd);
    void OnWrite(int fd);
    void OnClose(int fd);
    void OnError(int fd);
    void FlushWrite(int fd);

    EventLoop* loop_;       // 不拥有
    ThreadPool* pool_;      // 不拥有

    Socket listen_sock_;
    std::unique_ptr<Channel> listen_channel_;

    std::unordered_map<int, Connection> clients_;
};
```

**注意**：`Connection` 结构体跟 v3 完全一样（Socket + Channel + inputBuffer + outputBuffer），可以直接复用。

### 12.5 各方法实现要点

#### 构造函数

```
SingleReactorServer(EventLoop* loop, uint16_t port, ThreadPool* pool):
  1. loop_ = loop, pool_ = pool

  2. listen_sock_.SetReuseAddr()
  3. listen_sock_.Bind(port)
  4. listen_sock_.Listen(128)
  5. listen_sock_.SetNonBlocking()

  6. 打 banner（端口号、线程池大小）

  7. listen_channel_ = make_unique<Channel>(listen_sock_.Fd(), loop_->epoll())
     注意：v3 里 Channel 构造接受 Epoll*，这里需要拿到 EventLoop 的 Epoll*
     如果 EventLoop 没有暴露 epoll() getter，需要加一个：
     Epoll* EpollPtr() const { return epoll_.get(); }

  8. listen_channel_->SetReadCallback([this]() { OnAccept(); })

  9. loop_->AddChannel(listen_channel_.get())   ← 注册到 EventLoop
  10. listen_channel_->EnableRead()              ← 开始监听 EPOLLIN
```

**关键点**：Channel 构造需要一个 `Epoll*`。v3 里直接传 `epoll_.get()`，但 v4 里 Epoll 归 EventLoop 所有，服务器不直接持有。所以 EventLoop 需要暴露一个 getter：`Epoll* EpollPtr() const`。

给 EventLoop 新增：
```cpp
// 公有方法
Epoll* EpollPtr() const { return epoll_.get(); }
```

#### Start() — 不需要了！

v3 有 `Start()` 写事件循环。v4 不需要——`main()` 直接调 `loop->Loop()`，服务器只管设好回调。

#### OnAccept

```
OnAccept():
  1. Socket client = listen_sock_.Accept(ip, port)
  2. client.SetNonBlocking()
  3. client_fd = client.Fd()
  4. 打印连接日志

  5. Connection conn
  6. conn.sock = std::move(client)
  7. conn.channel = make_unique<Channel>(client_fd, loop_->EpollPtr())

  8. 设置四个回调：
     conn.channel->SetReadCallback([this, fd=client_fd]() { OnRead(fd); })
     conn.channel->SetWriteCallback([this, fd=client_fd]() { OnWrite(fd); })
     conn.channel->SetCloseCallback([this, fd=client_fd]() { OnClose(fd); })
     conn.channel->SetErrorCallback([this, fd=client_fd]() { OnError(fd); })

  9. loop_->AddChannel(conn.channel.get())     ← 注册到 EventLoop
  10. conn.channel->EnableRead()                ← 开始监听

  11. clients_.emplace(client_fd, std::move(conn))
  12. 打印当前连接数
```

**跟 v3 OnAccept 的区别**：v3 直接调 `epoll_->Add()` + 手写 lambda 捕获 fd。v4 通过 `loop_->AddChannel()` 注册，fd→Channel 映射由 EventLoop 内部维护。

#### OnRead — 核心变化

```
OnRead(fd):
  1. 从 clients_ 找到 Connection
  2. inputBuffer.ReadFd(fd, &savedErrno)

  3. if n > 0:
       // ═══ 关键变化：不在这里直接 echo，而是提交到线程池 ═══

       // 把收到的数据拷贝到 outputBuffer（在 IO 线程做，不耗时）
       conn.outputBuffer.Append(conn.inputBuffer.Peek(), conn.inputBuffer.ReadableBytes())
       size_t len = conn.inputBuffer.ReadableBytes()
       conn.inputBuffer.RetrieveAll()

       // 把"业务处理 + 通知发送"提交到线程池
       pool_->Run([this, fd, len]()
       {
           // ===== 这段在线程池的工作线程执行 =====
           
           // 模拟业务处理（将来换成 HTTP 解析）
           std::string msg = fmt::format("[Worker {}] 处理了 {} 字节",
                                         std::this_thread::get_id(), len);
           // 用 sleep 模拟耗时操作（如读磁盘、查数据库）
           // std::this_thread::sleep_for(std::chrono::milliseconds(10));

           // 通知 IO 线程发送
           loop_->RunInLoop([this, fd]()
           {
               // ===== 这段回到 EventLoop 线程执行 =====
               FlushWrite(fd);
           });
       });

  4. if n == 0: OnClose(fd)
  5. if n < 0 && EAGAIN: return
  6. if n < 0 && 其他错: OnClose(fd)
```

**数据流**：
```
EPOLLIN → OnRead(fd)
            │
            ├─ ReadFd → inputBuffer（IO 线程）
            ├─ 数据移到 outputBuffer（IO 线程）
            └─ pool_->Run(lambda)       异步提交
                  │
                  ▼
               [Worker 线程]
               "处理业务"（模拟耗时）
                  │
                  └─ loop_->RunInLoop(lambda)
                        │
                        ▼
                     [IO 线程]
                     FlushWrite(fd)  ← 非阻塞发送
```

**为什么要把数据先拷到 outputBuffer？** 因为 `pool_->Run` 是异步的——lambda 不立即执行。如果不在提交前拷好，worker 执行时 inputBuffer 可能已经被下一轮 OnRead 覆盖了。

#### OnWrite / FlushWrite / OnClose / OnError —— 跟 v3 一样

这四个方法完全不变。唯一差异：`OnClose` 里用 `loop_->RemoveChannel(channel.get())` 代替 v3 的隐式删除（Channel 析构时调 `epoll_->Del`）。

```
OnClose(fd):
  1. 打印日志
  2. auto it = clients_.find(fd)
  3. if it != end:
       loop_->RemoveChannel(it->second.channel.get())  ← 从 EventLoop 注销
       clients_.erase(it)                               ← Connection 析构
  4. 打印当前连接数
```

**注意清理顺序**：必须先 `RemoveChannel`（从 EventLoop 的 channels_ map 和 epoll 移除），再 `erase`（Connection 析构 → Channel 析构 → close fd）。如果先 erase，Channel 析构时调 `epoll_->Del()` 会抛异常（fd 已经从 epoll 移除了一次）。

#### 析构函数

```
~SingleReactorServer():
  // 关闭所有客户端连接
  for (auto& [fd, conn] : clients_)
  {
      loop_->RemoveChannel(conn.channel.get());
  }
  clients_.clear();

  // 关闭监听
  loop_->RemoveChannel(listen_channel_.get());
```

### 12.6 main() 函数

```cpp
int main()
{
    constexpr uint16_t PORT = 8888;
    constexpr size_t   THREADS = 4;

    try
    {
        EventLoop loop;
        ThreadPool pool(THREADS);

        SingleReactorServer server(&loop, PORT, &pool);

        std::cout << "[启动] 进入 EventLoop..." << std::endl;
        loop.Loop();   // 阻塞，直到 Quit()
        std::cout << "[退出] EventLoop 正常结束" << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[FATAL] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
```

**main 只有 4 步**：
1. 创建 EventLoop
2. 创建 ThreadPool
3. 创建 Server（内部注册所有 Channel + 回调）
4. `loop.Loop()` 启动事件循环

对比 v3 的 `server.Start()`（手写事件循环），v4 把事件循环完全交给 EventLoop。

### 12.7 改动清单

| 文件 | 改动 | 说明 |
|------|------|------|
| `eventloop.h` | 新增 `AddChannel(Channel*)` | 首次注册 fd |
| `eventloop.h` | 新增 `Epoll* EpollPtr()` | 暴露 epoll 实例给 Channel 构造 |
| **新增** `single_reactor_server.cpp` | 完整实现 | 单 Reactor 服务器 |

不需要改的文件：`epoll.h`、`channel.h`、`buffer.h`、`socket_raii.h`、`threadpool.h`

### 12.8 完整数据流（一次 echo 请求）

```
客户端: nc localhost 8888
  │
  │  TCP 连接
  ▼
[listen_fd EPOLLIN]
  │
  └─ EventLoop::Loop() 的 epoll_wait 返回
       └─ listen_channel_->HandleEvent(EPOLLIN)
            └─ OnAccept()
                 ├─ accept() → client_fd
                 ├─ 创建 Connection + Channel
                 └─ loop_->AddChannel(client_channel)
                       └─ epoll_->Add(client_fd, EPOLLIN)

客户端发送 "hello\n"
  │
  ▼
[client_fd EPOLLIN]
  │
  └─ EventLoop::Loop() 的 epoll_wait 返回
       └─ client_channel->HandleEvent(EPOLLIN)
            └─ OnRead(fd)
                 ├─ inputBuffer.ReadFd(fd)  → 读到 "hello\n"
                 ├─ 数据移到 outputBuffer
                 └─ pool_->Run(lambda)
                      │
                      ▼  [Worker 线程]
                      │   "处理业务"（echo 不需要处理）
                      │   loop_->RunInLoop(lambda)
                      │
                      ▼  [IO 线程 — 下一轮 DoPendingFunctors]
                      └─ FlushWrite(fd)
                           ├─ send() → 发出 "hello\n"
                           └─ outputBuffer.Retrieve(发送的字节数)
```

### 12.9 测试方法

```bash
# 编译
g++ -std=c++17 single_reactor_server.cpp -o single_reactor_server -pthread

# 基本测试
nc localhost 8888
hello
hello    # 应该收到回显

# 多客户端（3 个终端同时）
nc localhost 8888

# 大流量测试
cat /dev/urandom | head -c 1000000 | nc localhost 8888 > /dev/null

# 多客户端 + 大流量
# 开 3 个终端，每个发 1MB 数据
for i in 1 2 3; do
    cat /dev/urandom | head -c 1000000 | nc localhost 8888 > /dev/null &
done
```

### 12.10 与 Day 12 主从 Reactor 的对比预览

| | Day 11 单 Reactor | Day 12 主从 Reactor |
|---|---|---|
| EventLoop 数量 | 1 个 | 1 个 Main + N 个 Sub |
| accept 谁做 | EventLoop | MainReactor |
| IO 谁做 | EventLoop | SubReactor（每个线程一个） |
| ThreadPool | 做计算 | 做计算 |
| RunInLoop | 从 ThreadPool 回到 EventLoop | 从 ThreadPool 回到 SubReactor |
| 适用场景 | 学习过渡 | 生产环境 |

Day 11 是理解 Reactor 模式的关键一步——你写完就能感受到"IO 线程不阻塞"带来的好处。

### 12.11 参考文件

`reference/single_reactor_server_full.cpp` — 完整参考实现。同样，**先根据 DESIGN.md 自己写，遇到困难再看参考**。
