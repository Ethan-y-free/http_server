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

---

## 十三、主从 Reactor：MainReactor + SubReactor（Day 12 — 6/29）

### 13.1 为什么需要主从 Reactor

Day 11 单 Reactor 中，**accept 和所有客户端 IO 共用一个 EventLoop**。问题：

- accept 是很快，但上千个连接的 IO 事件都在一个线程里轮询
- 如果某个客户端的数据处理卡了一下（虽然不是阻塞，但 epoll_wait 返回后遍历事件也是开销）
- 多核 CPU 下，只有一个线程做 IO 是浪费

**主从 Reactor** 的思路：

```
MainReactor（主线程）           SubReactor[0..N-1]（工作线程）
┌─────────────────┐          ┌─────────────────┐
│ listen fd       │          │ client fd 1     │
│   ↓             │          │ client fd 5     │
│ accept          │──轮询──→│ client fd 9     │
│ 新连接分发       │  分发    │   ↓             │
└─────────────────┘          │ read/write/close│
                             └─────────────────┘
```

- MainReactor **只负责 accept**，压力极小
- SubReactor 每个线程一个 EventLoop（one loop per thread），负责客户端 IO
- 新连接 Round-Robin 分发给 SubReactor

### 13.2 架构数据流

```
客户端 → TCP → MainReactor(accept)
                    │
                    │ Round-Robin 选 SubReactor
                    ▼
              SubReactor[N]（IO 线程）
                    │
                    │ read 数据
                    ▼
              ThreadPool（计算线程）
                    │
                    │ 处理完 → RunInLoop 回到 SubReactor
                    ▼
              SubReactor[N]（IO 线程）
                    │
                    │ write 响应
                    ▼
                 客户端
```

### 13.3 API 清单

```cpp
class MultiReactorServer
{
public:
    // === 构造/析构 ===
    MultiReactorServer(EventLoop* mainLoop, uint16_t port, int subReactorCount, ThreadPool* pool);
    ~MultiReactorServer();

    // 禁止拷贝/移动
    MultiReactorServer(const MultiReactorServer&) = delete;
    MultiReactorServer& operator=(const MultiReactorServer&) = delete;

private:
    // === MainReactor 回调：只在主线程执行 ===
    void OnAccept();

    // === SubReactor 回调：在归属的 SubReactor 线程执行 ===
    void OnRead  (int fd, EventLoop* subLoop);
    void OnWrite (int fd, EventLoop* subLoop);
    void OnClose (int fd, EventLoop* subLoop);
    void OnError (int fd, EventLoop* subLoop);

    // === 工具方法 ===
    void FlushWrite        (int fd);  // 非阻塞写回（SubReactor 线程）
    void RemoveClientChannel(int fd);  // 从 epoll 移除 Channel（SubReactor 线程）

    // === 数据结构 ===
    struct Connection
    {
        Socket sock;
        std::unique_ptr<Channel> channel;
        Buffer inputBuffer;
        Buffer outputBuffer;
        EventLoop* ownerLoop;  // 归属哪个 SubReactor
    };

    // === Reactor 成员 ===
    EventLoop*                          mainLoop_;   // 主Reactor（不拥有）
    std::vector<std::unique_ptr<EventLoop>> subLoops_;  // SubReactor列表（拥有）
    std::vector<std::thread>            subThreads_;  // SubReactor线程
    ThreadPool*                         pool_;       // 计算线程池（不拥有）

    // === 监听 ===
    Socket                    listenSock_;
    std::unique_ptr<Channel>  listenChannel_;

    // === 客户端管理 ===
    std::unordered_map<int, Connection>  clients_;
    std::mutex                           clientsMutex_;
    std::atomic<int>                     nextSubReactor_{0};  // Round-Robin
};
```

| 成员 | 类型 | 说明 |
|------|------|------|
| `MultiReactorServer(loop, port, N, pool)` | 构造 | N=SubReactor数量=CPU核心数 |
| `~MultiReactorServer()` | 析构 | ①移除所有Channel → ②Quit所有SubReactor → ③join线程 |
| `OnAccept()` | MainReactor | accept → Round-Robin → subLoop->RunInLoop(注册Channel) |
| `OnRead(fd, sub)` | SubReactor | 读数据 → 拷到outputBuffer → 投ThreadPool → sub->RunInLoop(FlushWrite) |
| `OnWrite(fd, sub)` | SubReactor | 直接调FlushWrite |
| `OnClose(fd, sub)` | SubReactor | RemoveChannel + erase(clients_) |
| `OnError(fd, sub)` | SubReactor | 转调OnClose |
| `FlushWrite(fd)` | SubReactor | 非阻塞send，EAGAIN时EnableWrite等下次 |
| `RemoveClientChannel(fd)` | SubReactor | 从ownerLoop的epoll中Del |

### 13.4 关键设计点

#### ① 每个 SubReactor 是独立的 EventLoop 线程

```cpp
// 构造函数中启动 N 个 SubReactor 线程
for (int i = 0; i < subReactorCount; ++i)
{
    subLoops_[i] = std::make_unique<EventLoop>();
    subThreads_.emplace_back([this, i]() {
        subLoops_[i]->Loop();  // 阻塞在这里，直到 Quit()
    });
}
```

每个 SubReactor 在自己的线程里跑 `EventLoop::Loop()`，互不干扰。

#### ② Connection 需要知道归属哪个 SubReactor

```cpp
struct Connection
{
    Socket sock;
    std::unique_ptr<Channel> channel;
    Buffer inputBuffer;
    Buffer outputBuffer;
    EventLoop* ownerLoop;  // ← 新增：归属哪个 SubReactor
};
```

Channel 的 Epoll 指针必须指向 `ownerLoop` 的 Epoll。

#### ③ MainReactor::OnAccept 分发连接

```cpp
void OnAccept()
{
    Socket clientSock = listenSock_.Accept(ip, port);
    clientSock.SetNonBlocking();

    // Round-Robin 选 SubReactor
    int idx = nextSubReactor_.fetch_add(1) % subLoops_.size();
    EventLoop* subLoop = subLoops_[idx].get();

    // 创建 Connection（Channel 指向 SubReactor 的 Epoll）
    Connection conn;
    conn.channel = std::make_unique<Channel>(fd, subLoop->EpollPtr());
    conn.ownerLoop = subLoop;

    // 回调捕获 subLoop
    conn.channel->SetReadCallback([this, fd, sub = subLoop]() { OnRead(fd, sub); });
    // ... 其他回调同理 ...

    // 插入 clients_（需要 mutex）
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        clients_.emplace(fd, std::move(conn));
    }

    // 在 SubReactor 线程中注册 Channel
    subLoop->RunInLoop([this, fd]() {
        auto it = clients_.find(fd);
        if (it != clients_.end()) {
            it->second.ownerLoop->AddChannel(it->second.channel.get());
            it->second.channel->EnableRead();
        }
    });
}
```

#### ④ ThreadPool 回调回到 SubReactor

单 Reactor 里 ThreadPool 回调通过 `loop_->RunInLoop(...)` 回到主 EventLoop。

主从 Reactor 里，回调要回到**该连接归属的 SubReactor**：

```cpp
void OnRead(int fd, EventLoop* subLoop)
{
    // ... 读数据，拷贝到 outputBuffer ...

    pool_->Run([this, fd, subLoop]() {
        // 在计算线程中
        subLoop->RunInLoop([this, fd]() {
            FlushWrite(fd);  // 回到 SubReactor IO 线程写
        });
    });
}
```

#### ⑤ clients_ 的线程安全

- `OnAccept` 在 MainReactor 线程写入 `clients_`
- `OnClose` 在 SubReactor 线程删除 `clients_`
- 因此需要 `std::mutex clientsMutex_` 保护

#### ⑥ 优雅关闭

析构顺序：
1. 遍历 clients_，通知各 SubReactor 移除 Channel
2. 移除 listen Channel
3. `Quit()` 所有 SubReactor
4. `join()` 所有 SubReactor 线程

### 13.5 类结构

```
MultiReactorServer
├── mainLoop_            EventLoop*          主 Reactor（不拥有）
├── subLoops_            vector<unique_ptr>  SubReactor 列表（拥有）
├── subThreads_          vector<thread>      SubReactor 线程
├── pool_                ThreadPool*         计算线程池（不拥有）
│
├── listenSock_          Socket             监听 socket
├── listenChannel_       unique_ptr<Channel> 监听 Channel
│
├── clients_             unordered_map       所有客户端连接
├── clientsMutex_        mutex               clients_ 的锁
└── nextSubReactor_      atomic<int>         Round-Robin 计数器
```

### 13.6 与 Day 11 单 Reactor 的核心差异

| | Day 11 单 Reactor | Day 12 主从 Reactor |
|---|---|---|
| EventLoop 数量 | 1 | 1 + N |
| accept 线程 | EventLoop 线程 | MainReactor 线程 |
| IO 线程 | EventLoop 线程 | SubReactor 线程 |
| accept 后 | 直接 AddChannel | Round-Robin → subLoop->RunInLoop(AddChannel) |
| Channel 的 Epoll* | mainLoop_->EpollPtr() | subLoop->EpollPtr() |
| ThreadPool 回调 | loop_->RunInLoop() | subLoop->RunInLoop() |
| clients_ | 无锁（单线程） | 有锁（多线程） |
| 关闭 | Quit mainLoop | Quit 所有 subLoop → join 线程 |

### 13.7 实现要点

1. **先启动 SubReactor 线程，再创建 MultiReactorServer**（或构造时启动）。SubReactor 必须先 Loop 起来，否则 OnAccept 分发时 `RunInLoop` 写 eventfd 会失败。

2. **Channel 必须用 SubReactor 的 EpollPtr**。如果在 OnAccept 里用了 mainLoop 的 Epoll，Channel 就会注册到错误的 epoll 实例。

3. **回调里捕获 `subLoop`**。每个 Connection 的回调（OnRead/OnWrite/OnClose/OnError）都需要知道归属的 SubReactor，用于 ThreadPool 回调时的 `RunInLoop`。

4. **OnClose 的线程安全**。OnClose 在 SubReactor 线程执行，但 clients_ 的写入（OnAccept）在主线程。所以 `clients_.erase(fd)` 必须加锁。

5. **禁止拷贝 EventLoop**。`EventLoop(const EventLoop&) = delete`，所以用 `vector<unique_ptr<EventLoop>>` 来管理 SubReactor。

### 13.8 测试方法

```bash
# 终端1：启动服务器
cd build && cmake .. && make multi_reactor_server && ./multi_reactor_server

# 终端2-5：模拟多客户端
nc localhost 8888
nc localhost 8888
nc localhost 8888
nc localhost 8888

# 观察输出：不同连接应该分布到不同 SubReactor
# [recv 5B fd=6 via SubReactor] hello
# [recv 5B fd=7 via SubReactor] world
```

### 13.9 参考文件

`reference/multi_reactor_server_full.cpp` — 完整参考实现。**先根据 DESIGN.md 自己写，遇到困难再看参考。**

---

# 十四、HTTP 请求解析（状态机）

> **Day 13 目标**：实现一个增量式 HTTP/1.1 请求解析器，用有限状态机从字节流中提取 method、path、headers、body。

---

## 14.1 为什么需要状态机

TCP 是字节流，客户端的一整条 HTTP 请求可能被拆成多个 TCP 段到达：

```
客户端发送: "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
                   │
    ┌──────────────┼──────────────┐
    ▼              ▼              ▼
 段1: "GET / H"   段2: "TTP/1.1\r"   段3: "\nHost: loca..."
```

回调里每次 `read()` 拿到的只是一段数据。状态机记住"当前解析到哪了"，每次新数据到达时接着解析。

---

## 14.2 HTTP/1.1 请求格式

```
┌─────────────────────────────────────────────────────────┐
│ 请求行 (Request Line)                                    │
│ GET /index.html HTTP/1.1\r\n                             │
├─────────────────────────────────────────────────────────┤
│ 请求头 (Headers)                                         │
│ Host: www.example.com\r\n                                │
│ Connection: keep-alive\r\n                               │
│ Content-Length: 13\r\n                                   │
│ \r\n                      ← 空行标记 headers 结束         │
├─────────────────────────────────────────────────────────┤
│ 请求体 (Body) — 仅 POST/PUT 等有                         │
│ Hello, World!                                            │
└─────────────────────────────────────────────────────────┘
```

### 请求行格式

```
METHOD SP PATH SP VERSION CRLF
  │      │      │       │
  │      │      │       └─ HTTP/1.1 或 HTTP/1.0
  │      │      └─ 空格分隔
  │      └─ /index.html?a=1  (含 query string)
  └─ GET / POST / HEAD
```

- **METHOD**：大小写敏感，常见 GET / POST / HEAD
- **PATH**：以 `/` 开头，可能包含 `?query=string`
- **VERSION**：`HTTP/1.1` 或 `HTTP/1.0`
- 三项之间用**单个空格**分隔，行末 `\r\n`

### 请求头格式

```
Key: Value\r\n
```

- Key 不区分大小写（习惯首字母大写）
- `:` 后面有一个空格
- 空行 `\r\n` 表示 headers 结束

### 请求体

- 只在 POST/PUT 等有请求体时存在
- 长度由 `Content-Length` 头决定
- 没有 `Content-Length` → 无 body（GET 请求）

---

## 14.3 状态机设计

```
                    ┌──────────┐
       new data ──→ │  START   │
                    └────┬─────┘
                         │
              ┌──────────▼──────────┐
              │  PARSE_REQUEST_LINE  │  逐字节扫描，找第一个 \r\n
              └──────────┬──────────┘
                         │ 找到完整请求行
              ┌──────────▼──────────┐
              │    PARSE_HEADERS     │  逐行解析 Key: Value
              └──────────┬──────────┘
                         │ 遇到空行 \r\n
              ┌──────────▼──────────┐
              │    PARSE_BODY        │  按 Content-Length 读取
              └──────────┬──────────┘
                         │ body 读完 / 无 body
              ┌──────────▼──────────┐
              │    PARSE_DONE        │  完整请求已就绪
              └─────────────────────┘
```

**关键规则**：任何状态下数据不足 → 返回"需要更多数据"，状态不变，下次新数据到达时从同一状态继续。

---

## 14.4 数据结构

### HttpRequest（解析结果）

#### 文件：`http_request.h`

#### 职责

保存一条 HTTP 请求的完整结构化数据。由 `HttpRequestParser` 填充，由服务器读取并做决策。

#### API 清单

| 分类 | 方法 | 说明 |
|------|------|------|
| Setter | `SetMethod(m)` | 设置请求方法（GET/POST/HEAD） |
| Setter | `SetPath(p)` | 设置请求路径 |
| Setter | `SetVersion(v)` | 设置 HTTP 版本 |
| Setter | `AddHeader(key, value)` | 添加一个请求头键值对 |
| Setter | `AppendBody(data, len)` | 追加请求体数据 |
| Getter | `GetMethod()` → `const string&` | 获取请求方法 |
| Getter | `GetPath()` → `const string&` | 获取请求路径 |
| Getter | `GetVersion()` → `const string&` | 获取 HTTP 版本 |
| Getter | `GetBody()` → `const string&` | 获取请求体 |
| 查询 | `GetHeader(key)` → `string` | 大小写不敏感查找 header 值 |
| 查询 | `IsKeepAlive()` → `bool` | 是否长连接 |
| 查询 | `ContentLength()` → `size_t` | Content-Length 值，无则返回 0 |
| 管理 | `Reset()` | 清空所有字段 |

#### API 签名

```cpp
class HttpRequest
{
public:
    // Setters（由解析器调用）
    void SetMethod(const std::string& m);
    void SetPath(const std::string& p);
    void SetVersion(const std::string& v);
    void AddHeader(const std::string& key, const std::string& value);
    void AppendBody(const char* data, size_t len);

    // Getters
    const std::string& GetMethod()  const;
    const std::string& GetPath()    const;
    const std::string& GetVersion() const;
    const std::string& GetBody()    const;

    // 查询
    std::string GetHeader(const std::string& key) const;
    bool IsKeepAlive() const;
    size_t ContentLength() const;

    // 管理
    void Reset();

private:
    std::string method_;
    std::string path_;
    std::string version_;
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;
};
```

#### 设计说明

- **IsKeepAlive()**：`HTTP/1.1` 默认长连接（`Connection: close` 时关闭），`HTTP/1.0` 默认短连接（`Connection: keep-alive` 时保持）
- **GetHeader()**：header 名称大小写不敏感（RFC 7230 §3.2）
- **ContentLength()**：查 `Content-Length` header，无则返回 0（GET 请求没有 body）

> **为什么 header key 用大小写不敏感比较**：客户端可能发 `Content-Length` 或 `content-length` 或 `CONTENT-LENGTH`，RFC 7230 §3.2 规定 header 名称大小写不敏感。

---

## 14.5 HttpRequestParser API

#### 文件：`http_parser.h`

#### 职责

增量式 HTTP/1.1 请求解析器。内部用有限状态机记录解析进度，每次 `Parse()` 从 Buffer 消费已解析的字节。可 `Reset()` 后重用（支持长连接多请求场景）。

#### API 清单

| 分类 | 方法 | 说明 |
|------|------|------|
| 核心 | `Parse(buffer)` → `ParseResult` | 喂数据，推进状态机，消费 buffer 中已解析字节 |
| 查询 | `IsDone()` → `bool` | 解析是否完成（状态 == PARSE_DONE） |
| 查询 | `CurrentState()` → `State` | 当前状态（调试用） |
| 查询 | `GetRequest()` → `const HttpRequest&` | 获取解析结果 |
| 管理 | `Reset()` | 重置状态机，准备解析下一个请求 |

#### 枚举定义

```cpp
enum State {
    PARSE_REQUEST_LINE,   // 正在解析请求行
    PARSE_HEADERS,        // 正在解析请求头
    PARSE_BODY,           // 正在解析请求体
    PARSE_DONE            // 解析完成
};

enum ParseResult {
    PARSE_OK,             // 状态推进了一步
    PARSE_NEED_MORE,      // 数据不足，等待下次 read
    PARSE_ERROR           // 协议错误
};
```

#### API 签名

```cpp
class HttpRequestParser
{
public:
    enum State { PARSE_REQUEST_LINE, PARSE_HEADERS, PARSE_BODY, PARSE_DONE };
    enum ParseResult { PARSE_OK, PARSE_NEED_MORE, PARSE_ERROR };

    ParseResult Parse(Buffer* buffer);
    bool IsDone() const;
    State CurrentState() const;
    const HttpRequest& GetRequest() const;
    void Reset();

private:
    ParseResult ParseRequestLine(Buffer* buffer);
    ParseResult ParseHeaders(Buffer* buffer);
    ParseResult ParseBody(Buffer* buffer);

    State state_ = PARSE_REQUEST_LINE;
    HttpRequest request_;
    size_t bodyRead_ = 0;
};
```

#### 设计决策

1. **增量解析**：`Parse()` 可多次调用，不够返回 `PARSE_NEED_MORE`，状态不变
2. **消费式**：从 `Buffer::Peek()` 查看数据，解析后用 `Retrieve(n)` 消费，不额外拷贝
3. **可重用**：`Reset()` 后解析下一个请求（HTTP keep-alive 长连接）
4. **Fail-fast**：协议错误立即返回 `PARSE_ERROR`，调用方关闭连接

---

## 14.6 各状态的解析算法

### 14.6.1 ParseRequestLine

```
输入: buffer（至少含部分请求行数据）

算法:
1. 在 buffer 中查找 \r\n
   ├── 找不到 → 返回 PARSE_NEED_MORE
   └── 找到 → 继续
2. 提取行内容（不含 \r\n）
3. 按空格拆分为 method, path, version（恰好 3 段）
   ├── 段数不对 → 返回 PARSE_ERROR
   └── 正确 → 继续
4. 验证 method 是已知方法（GET/POST/HEAD）
   ├── 不支持 → 返回 PARSE_ERROR
   └── 支持 → 继续
5. 验证 path 以 '/' 开头
   ├── 不是 → 返回 PARSE_ERROR
   └── 是 → 继续
6. 存入 request_
7. Retrieve 掉整行（含 \r\n）
8. 状态切换 → PARSE_HEADERS
9. 返回 PARSE_OK
```

**复杂度**：O(n)，n = 请求行长（通常 < 100 字节）。

### 14.6.2 ParseHeaders

```
输入: buffer（至少含部分 header 数据）

算法:
循环:
  1. 在 buffer 中查找 \r\n
     ├── 找不到 → 返回 PARSE_NEED_MORE
     └── 找到 → 继续
  2. 提取行内容
  3. 如果行为空（即 \r\n 就是空行）:
     ├── headers 结束
     ├── Retrieve 掉 2 字节（\r\n）
     ├── 检查 Content-Length
     │   ├── > 0 → 切换到 PARSE_BODY
     │   └── = 0 或不存在 → 切换到 PARSE_DONE
     └── 返回 PARSE_OK
  4. 解析 "Key: Value" 格式
     ├── 找不到 ':' → 返回 PARSE_ERROR
     ├── value 以空格开头 → 去掉前导空格
     └── 存入 request_.headers[key] = value
  5. Retrieve 掉这行（含 \r\n）
  6. 继续循环
```

**复杂度**：O(n)，n = headers 段总长（通常 < 2KB）。

> **注意**：每行最多不超过 `MAX_HEADER_LINE`（建议 8192 字节），防止恶意客户端发超长 header 耗尽内存。

### 14.6.3 ParseBody

```
输入: buffer

算法:
1. 计算还需读取的字节数: remain = contentLength - bodyRead_
2. buffer 中可读字节数: avail = buffer.ReadableBytes()
3. 比较:
   ├── avail >= remain → 可以读完
   │   ├── body.append(buffer.Peek(), remain)
   │   ├── buffer.Retrieve(remain)
   │   ├── 切换到 PARSE_DONE
   │   └── 返回 PARSE_OK
   └── avail < remain → 不够
       ├── body.append(buffer.Peek(), avail)
       ├── bodyRead_ += avail
       ├── buffer.Retrieve(avail)
       └── 返回 PARSE_NEED_MORE
```

**复杂度**：O(n)，n = body 长度。

---

## 14.7 与服务器集成

### 修改 OnRead 回调

当前 echo 逻辑：

```cpp
// 旧: echo — 读到什么就回什么
conn.outputBuffer.Append(conn.inputBuffer.Peek(), conn.inputBuffer.ReadableBytes());
conn.inputBuffer.RetrieveAll();
```

改为 HTTP 解析：

```cpp
// 新: HTTP 解析 → 生成响应
auto result = conn.parser.Parse(&conn.inputBuffer);

if (result == HttpRequestParser::PARSE_ERROR)
{
    shouldClose = true;
}
else if (result == HttpRequestParser::PARSE_OK && conn.parser.IsDone())
{
    // 解析完成，生成 HTTP 响应
    GenerateResponse(conn.parser.GetRequest(), &conn.outputBuffer);

    if (!conn.parser.GetRequest().IsKeepAlive())
    {
        shouldClose = true;  // 短连接，响应后关闭
    }
    else
    {
        conn.parser.Reset(); // 长连接，重置解析器等下一个请求
    }
}
// else: PARSE_NEED_MORE → 等下次事件
```

### Connection 结构体新增字段

```cpp
struct Connection
{
    Socket sock;
    std::unique_ptr<Channel> channel;
    Buffer inputBuffer;
    Buffer outputBuffer;
    EventLoop* ownerLoop;
    HttpRequestParser parser;    // ← 新增
};
```

### 完整数据流

```
客户端 TCP 段到达
    │
    ▼
epoll_wait 返回 EPOLLIN
    │
    ▼
OnRead(fd)
    │
    ├─ inputBuffer.ReadFd(fd)      // 从内核读到 Buffer
    │
    ├─ parser.Parse(&inputBuffer)  // 喂给状态机
    │       │
    │       ├─ 不足 → return
    │       ├─ 错误 → OnClose
    │       └─ DONE → GenerateResponse()
    │
    ├─ outputBuffer ← HTTP 响应
    │
    └─ FlushWrite(fd)              // 非阻塞写回客户端
```

---

## 14.8 HTTP 响应生成（最简版）

Day 13 只需返回一个最简单的 HTTP 响应来验证解析正确：

```cpp
void GenerateResponse(const HttpRequest& req, Buffer* output)
{
    std::string body;
    std::string status;

    if (req.GetMethod() == "GET" || req.GetMethod() == "HEAD")
    {
        status = "200 OK";
        body = "<html><body><h1>Hello from http_server!</h1>"
               "<p>Path: " + req.GetPath() + "</p>"
               "<p>Method: " + req.GetMethod() + "</p>"
               "</body></html>";
    }
    else if (req.GetMethod() == "POST")
    {
        status = "200 OK";
        body = "<html><body><h1>POST received</h1>"
               "<p>Body: " + req.GetBody() + "</p>"
               "</body></html>";
    }
    else
    {
        status = "405 Method Not Allowed";
        body = "<html><body><h1>405 Method Not Allowed</h1></body></html>";
    }

    char buf[4096];
    int len;
    if (req.GetMethod() == "HEAD")
    {
        len = snprintf(buf, sizeof(buf),
            "%s %s\r\n"
            "Server: tiny-http/1.0\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %zu\r\n"
            "Connection: %s\r\n"
            "\r\n",
            req.GetVersion().c_str(), status.c_str(),
            body.size(),
            req.IsKeepAlive() ? "keep-alive" : "close");
    }
    else
    {
        len = snprintf(buf, sizeof(buf),
            "%s %s\r\n"
            "Server: tiny-http/1.0\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %zu\r\n"
            "Connection: %s\r\n"
            "\r\n"
            "%s",
            req.GetVersion().c_str(), status.c_str(),
            body.size(),
            req.IsKeepAlive() ? "keep-alive" : "close",
            body.c_str());
    }

    output->Append(buf, len);
}
```

> Day 15 会实现完整的静态文件服务 + 404/500 处理，Day 13 只需这个最简版验证解析器正确。

---

## 14.9 文件规划

```
week01/
├── http_request.h     ← HttpRequest 数据结构（~70 行）
├── http_parser.h      ← HttpRequestParser 状态机（~120 行）
├── multi_reactor_server.cpp  ← 修改：OnRead 集成解析器 + GenerateResponse
│                                （新增 ~80 行，改动 ~20 行）
└── CMakeLists.txt     ← 无需改动（头文件自动生效）
```

> **Day 13 只需写两个头文件 `http_request.h` + `http_parser.h`，改一个 `.cpp`。**
> **所有类都是 header-only，无需改 CMakeLists.txt。**

---

## 14.10 API 清单汇总

| 类型 | 方法 | 说明 |
|------|------|------|
| `HttpRequest::Reset()` | void | 清空所有字段，准备下一次解析 |
| `HttpRequest::GetHeader(key)` | string | 大小写不敏感查找 header 值 |
| `HttpRequest::IsKeepAlive()` | bool | 判断是否长连接 |
| `HttpRequest::ContentLength()` | size_t | 返回 Content-Length 值（无则 0） |
| | | |
| `HttpRequestParser::Parse(buffer)` | ParseResult | 核心方法：增量解析，推进状态机 |
| `HttpRequestParser::IsDone()` | bool | 解析是否完成 |
| `HttpRequestParser::GetRequest()` | const HttpRequest& | 获取解析结果 |
| `HttpRequestParser::Reset()` | void | 重置状态机，准备解析下一个请求 |
| `HttpRequestParser::CurrentState()` | State | 查询当前状态（调试用） |

---

## 14.11 测试方法

### 浏览器测试

```bash
# 终端1: 启动服务器
cd build && cmake .. && make multi_reactor_server && ./multi_reactor_server

# 浏览器打开: http://localhost:8888/
# 应该看到 "Hello from http_server!" 页面
# 浏览器打开: http://localhost:8888/test.html
# Path 显示 /test.html
```

### curl 测试

```bash
# GET 请求
curl -v http://localhost:8888/

# POST 请求
curl -v -X POST http://localhost:8888/ -d "hello world"

# 查看 keep-alive 行为
curl -v http://localhost:8888/ http://localhost:8888/

# HEAD 请求
curl -v -X HEAD http://localhost:8888/
```

### nc 手动构造请求

```bash
nc localhost 8888
GET / HTTP/1.1\r\n
Host: localhost\r\n
\r\n
# 应返回 HTTP/1.1 200 OK + HTML body
```

---

## 14.12 小结：你需要做的事

1. **新建 `http_request.h`**：实现 `HttpRequest` 结构体（照 §14.4 API）
2. **新建 `http_parser.h`**：实现 `HttpRequestParser` 类（照 §14.5 API + §14.6 算法）
3. **修改 `multi_reactor_server.cpp`**：
   - `Connection` 加 `HttpRequestParser parser` 成员
   - `OnRead` 中：数据到达 → `parser.Parse()` → 完成后调用 `GenerateResponse()`
   - 添加 `GenerateResponse()` 函数（照 §14.8）
   - 长连接：`IsKeepAlive()` 为 true 时 `parser.Reset()`；否则 `OnClose`
4. **编译运行**：`cd build && cmake .. && make multi_reactor_server && ./multi_reactor_server`
5. **浏览器访问 `http://localhost:8888/`** 看到 HTML 页面即为成功 ✅

> **参考文件**：`reference/http_request_full.h` + `reference/http_parser_full.h` + `reference/multi_reactor_http_full.cpp`

---

# 十五、Week 4 周总结 + 压测对比

> **Day 14 目标**：对单 Reactor 和主从 Reactor 做压测对比，记录 QPS 数据，总结 Week 4 全部产出。

---

## 15.1 压测环境

| 项目 | 值 |
|------|-----|
| 虚拟机 | VMware Ubuntu |
| CPU | 4 核 |
| 内存 | 8G（分配） |
| 工具 | ApacheBench (`ab`) |
| 测试命令 | `ab -n 100000 -c 100 http://127.0.0.1:8888/` |
| 参数含义 | `-n 100000` 总共 10 万请求，`-c 100` 100 并发连接 |

---

## 15.2 待测目标

| # | 目标 | 架构 | 说明 |
|---|------|------|------|
| A | `single_reactor_server` | 1 EventLoop + ThreadPool | 单线程 IO，计算投线程池 |
| B | `multi_reactor_http` | MainReactor + 4 SubReactor + ThreadPool | 主从 IO，计算投线程池 |

---

## 15.3 测试步骤

```bash
# ① 编译
cd build && cmake .. && make single_reactor_server multi_reactor_http

# ② 测试目标 A：单 Reactor
./week01/single_reactor_server &
ab -n 100000 -c 100 http://127.0.0.1:8888/ > result_single.txt
fuser -k 8888/tcp

# ③ 测试目标 B：主从 Reactor HTTP
./week01/multi_reactor_http &
ab -n 100000 -c 100 http://127.0.0.1:8888/ > result_multi.txt
fuser -k 8888/tcp

# ④ 也可以测不同并发级别
for c in 10 50 100 500 1000; do
    ./week01/multi_reactor_http &
    ab -n 100000 -c $c http://127.0.0.1:8888/ > result_multi_c${c}.txt
    fuser -k 8888/tcp
done
```

---

## 15.4 关注指标

从 `ab` 输出中提取：

| 指标 | 含义 |
|------|------|
| Requests per second | **QPS** — 最核心的指标 |
| Time per request (mean) | 平均响应时间 |
| Failed requests | 失败数（应为 0） |
| Transfer rate | 吞吐量 KB/s |

---

## 15.5 预期结果

| | 单 Reactor | 主从 Reactor | 差异 |
|---|---|---|---|
| QPS | ~2-3 万 | ~4-6 万 | 主从约 2x |
| 原因 | 单线程 accept + IO，epoll_wait 下 accept 惊群效应少但 IO 瓶颈 | accept 和 IO 分离，4 个 SubReactor 分摊 IO |

> **路线图目标**：主从 Reactor 模式跑通，Webbench 压测 > 3 万 QPS。

---

## 15.6 Week 4 完成清单

| Day | 内容 | 状态 |
|-----|------|------|
| Day 8 | Reactor 模式概念理解 | ✅ |
| Day 9 | EventLoop 类实现 | ✅ |
| Day 10 | ThreadPool 类实现 | ✅ |
| Day 11 | 单 Reactor Echo Server | ✅ |
| Day 12 | 主从 Reactor Echo Server | ✅ |
| Day 13 | HTTP 请求解析（状态机） | ✅ |
| Day 14 | 压测对比 + 周总结 | ✅ |

### Week 4 新增文件

```
week01/
├── eventloop.h              # EventLoop：one loop per thread 核心
├── threadpool.h             # ThreadPool：C++11 线程池
├── single_reactor_server.cpp  # 单 Reactor：1 EventLoop + ThreadPool
├── multi_reactor_server.cpp   # 主从 Reactor：Main + N Sub
├── http_request.h           # HttpRequest 数据结构
├── http_parser.h            # HTTP/1.1 状态机解析器
└── multi_reactor_http.cpp   # 主从 Reactor HTTP Server（集大成）
```

### Week 4 核心架构

```
                    MainReactor (EventLoop ×1)
                    ┌──────────────────────┐
                    │  监听 socket          │
                    │  accept() 新连接      │
                    │  Round-Robin 分发     │
                    └──┬────┬────┬────┬────┘
                       │    │    │    │
              ┌────────▼────▼────▼────▼────────┐
              │     SubReactor (EventLoop ×N)   │
              │  ┌──────┐ ┌──────┐ ┌──────┐    │
              │  │  fd1  │ │  fd2  │ │  fd3  │    │
              │  │  IO   │ │  IO   │ │  IO   │    │
              │  └──┬───┘ └──┬───┘ └──┬───┘    │
              └─────┼────────┼────────┼────────┘
                    │        │        │
              ┌─────▼────────▼────────▼────────┐
              │      ThreadPool (计算线程池)     │
              │  HTTP 解析 → 生成响应 → 写回    │
              └────────────────────────────────┘
```

---

## 15.7 Day 14 任务清单

1. **VM 安装 ab**：`sudo apt install apache2-utils -y`
2. **跑基准测试**：单 Reactor vs 主从 Reactor，记录 QPS 数据
3. **更新笔记**：在笔记文件中补充压测数据
4. **总结**：完成 Week 4 周总结
> 先根据 DESIGN.md 自己写，遇到困难再看参考。

---

# Day 15 技术文档：HTTP 静态文件服务

---

## 十六、我们要做什么

当前 `GenerateResponse()` 是硬编码的 HTML 字符串——无论请求什么路径都返回 "Hello from http_server!"。Day 15 把它升级为真正的**静态文件服务器**：

```
现在（硬编码）                           Day 15（静态文件）
──────────────────────────             ──────────────────────────
GET /          → "Hello..."             GET /          → www/index.html
GET /foo       → "Hello... Path: /foo"  GET /style.css → www/style.css
GET /nonexist  → "Hello..."             GET /nonexist  → 404 Not Found
```

**目标**：浏览器访问 `http://localhost:8888/` 能看到真正的 HTML 页面，支持 CSS/JS/图片。

---

## 十六.1 模块架构

新增一个 `HttpStaticHandler` 类，放在 `http_static_handler.h` 中：

```
┌──────────────────────────────────────────────────────┐
│                  HttpStaticHandler                    │
│                                                      │
│  职责：URL → 文件系统路径 → 读文件 → HTTP 响应        │
│                                                      │
│  ┌─────────────┐  ┌──────────────┐  ┌─────────────┐ │
│  │  MapPath()  │  │  ReadFile()  │  │ GetMimeType()│ │
│  │ 路径映射    │  │  读文件内容   │  │  MIME 检测   │ │
│  │ + 安全检查  │  │              │  │              │ │
│  └─────────────┘  └──────────────┘  └─────────────┘ │
│                                                      │
│  HandleRequest(req, output) — 唯一对外入口            │
└──────────────────────────────────────────────────────┘
```

---

## 十六.2 HttpStaticHandler API

### 文件：`http_static_handler.h`（你需要从头写）

### 职责
URL 路径 → 文件系统路径 → 读取文件 → 生成 HTTP 响应。对外只有一个入口 `HandleRequest()`。

### API 清单

#### 对外方法（public）

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `HttpStaticHandler(root_dir)` | `const std::string&` | — | 构造函数，初始化 `root_dir_` + `index_file_`，去掉 root_dir 末尾 `/` |
| `SetIndexFile(index)` | `const std::string&` | `void` | 设置目录默认文件，默认值 `"index.html"` |
| `HandleRequest(req, output)` | `const HttpRequest&`, `Buffer*` | `void` | **唯一对外入口**：按 `HandleRequest` 流程生成 HTTP 响应 |

#### 私有方法（private）— 按参考代码顺序

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `UrlDecode(str)` | `const std::string&` | `std::string` | URL 解码；%20→空格，%2F→/；大小写不敏感 |
| `MapPath(url_path)` | `const std::string&` | `std::string` | URL 路径 → 文件系统路径；含 `..` 安全拦截，返回空串=拒绝 |
| `GetMimeType(path)` | `const std::string&` | `std::string` | 扩展名 → MIME 类型；未知返回 `application/octet-stream` |
| `ReadFile(path, content)` | `const std::string&`, `std::string&` | `bool` | 二进制模式读整个文件；成功 true，失败 false |
| `BuildResponse(...)` | 8 个参数（见签名） | `void` | 拼接完整 HTTP 响应头+体写入 Buffer |
| `IsHex(c)` | `char` | `bool` | 判断字符是否为十六进制数字（0-9/A-F/a-f） |
| `HexToInt(c)` | `char` | `int` | 十六进制字符 → 整数值（'A'→10, 'F'→15） |
| `IsDirectory(path)` | `const std::string&` | `bool` | `stat()` 判断路径是否为目录 |
| `FileExists(path)` | `const std::string&` | `bool` | `stat()` 判断路径是否为普通文件且存在 |
| `EscapeHtml(str)` | `const std::string&` | `std::string` | HTML 实体转义（`<`→`&lt;` 等），防 XSS |

#### 静态成员变量

| 变量 | 类型 | 说明 |
|------|------|------|
| `kEmptyBody_` | `const std::string` | HEAD 请求用于替代 body 的空串 |

### API 签名

```cpp
class HttpStaticHandler
{
public:
    // 构造：root_dir 可为相对路径（"./www"）或绝对路径
    // 内部处理：去掉末尾 '/'、初始化 index_file_ 为 "index.html"
    explicit HttpStaticHandler(const std::string& root_dir);

    // 设置目录默认文件
    void SetIndexFile(const std::string& index);

    // ====== 唯一对外入口 ======
    void HandleRequest(const HttpRequest& req, Buffer* output);

private:
    // ---- URL 处理 ----
    // %20 → 空格，%2F → /，+ → 空格，大小写不敏感
    static std::string UrlDecode(const std::string& str);

    // URL 路径 → 文件系统路径
    // 返回空串表示拒绝访问（检测到 ".."）
    // 前置条件：调用方必须先 UrlDecode
    std::string MapPath(const std::string& url_path);

    // ---- MIME 类型 ----
    // 提取扩展名 → 查表 → 返回 MIME 字符串
    static std::string GetMimeType(const std::string& path);

    // ---- 文件读取 ----
    // 二进制模式（std::ios::binary | std::ios::ate），成功返回 true
    static bool ReadFile(const std::string& path, std::string& content);

    // ---- 响应生成 ----
    // 拼接 HTTP 响应头 + body，写入 Buffer
    // head_only=true 时 Content-Length 保留但 body 不发送
    static void BuildResponse(int status_code,
                              const std::string& status_msg,
                              const std::string& body,
                              const std::string& mime_type,
                              bool keep_alive,
                              const std::string& version,
                              bool head_only,
                              Buffer* output);

    // ---- 工具函数 ----
    static bool IsHex(char c);
    static int  HexToInt(char c);
    static bool IsDirectory(const std::string& path);
    static bool FileExists(const std::string& path);
    static std::string EscapeHtml(const std::string& str);

    // ---- 成员变量 ----
    std::string root_dir_;
    std::string index_file_;

    // ---- 静态成员 ----
    static const std::string kEmptyBody_;
};
```

---

## 十六.3 HandleRequest 核心流程

```
HandleRequest(req, output)
    │
    ├─ 1. method 不是 GET/HEAD？
    │      └─ → 405 Method Not Allowed
    │
    ├─ 2. url_path = UrlDecode(req.GetPath())
    │
    ├─ 3. file_path = MapPath(url_path)
    │      └─ 为空（含 ..）？ → 403 Forbidden
    │
    ├─ 4. 路径是目录？
    │      └─ 拼上 index_file_ 再试
    │
    ├─ 5. ReadFile(file_path, content)
    │      ├─ 成功 → BuildResponse(200, content)
    │      └─ 失败 → BuildResponse(404)
    │
    └─ 6. HEAD 请求：body 置空，只返回头
```

---

## 十六.4 MapPath + 安全设计

**核心原则**：绝不把用户输入的路径直接拼到文件系统路径上。

```cpp
std::string MapPath(const std::string& url_path)
{
    // ① 解码 %xx（%20→空格，%2F→/）
    std::string decoded = UrlDecode(url_path);

    // ② 安全检查：禁止 ".." 目录穿越
    if (decoded.find("..") != std::string::npos)
    {
        return "";  // 空串 = 拒绝访问
    }

    // ③ 拼接到 root_dir_（root_dir_ 已是绝对路径）
    std::string result = root_dir_ + "/" + decoded;

    // ④ 规范化路径（去掉连续的 //）
    // 提示：用 while 循环替换 "//" → "/"

    return result;
}
```

### 攻击场景演示

```
正常请求：  GET /index.html        → ./www/index.html       ✅
攻击请求：  GET /../etc/passwd      → MapPath 返回 ""        → 403
攻击请求：  GET /..%2F..%2Fetc      → UrlDecode → /../../etc → MapPath 返回 "" → 403
```

> **关键**：先 `UrlDecode` 再检查 `..`，否则编码绕过。

---

## 十六.5 UrlDecode 规范

| 编码 | 解码 |
|------|------|
| `%20` | 空格 |
| `%2F` | `/` |
| `%2f` | `/`（大小写不敏感） |
| `%25` | `%` |
| `+` | 空格（query string 约定，path 里少见） |

### 算法

```
遍历字符串：
  遇到 '%' 且后面有两个 hex 字符 →
    转换为对应 ASCII 字符
  否则 → 原样保留
```

> 解码后的字符串可能比输入短（`%20` 3 字节→空格 1 字节）。

---

## 十六.6 MIME 类型映射表

`GetMimeType(path)` 提取文件扩展名，查表返回对应 MIME 类型：

| 扩展名 | MIME 类型 |
|--------|----------|
| `.html`, `.htm` | `text/html` |
| `.css` | `text/css` |
| `.js` | `application/javascript` |
| `.json` | `application/json` |
| `.png` | `image/png` |
| `.jpg`, `.jpeg` | `image/jpeg` |
| `.gif` | `image/gif` |
| `.svg` | `image/svg+xml` |
| `.ico` | `image/x-icon` |
| `.txt` | `text/plain; charset=utf-8` |
| `.xml` | `application/xml` |
| `.pdf` | `application/pdf` |
| `.woff` | `font/woff` |
| `.woff2` | `font/woff2` |
| *默认* | `application/octet-stream` |

### 实现提示

```cpp
static std::string GetMimeType(const std::string& path)
{
    // ① 找到最后一个 '.'
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";

    // ② 取扩展名（转小写）
    std::string ext = path.substr(dot);
    for (auto& c : ext) c = static_cast<char>(std::tolower(c));

    // ③ 查表（if-else 或 static unordered_map）
    static const std::unordered_map<std::string, std::string> mime = {
        {".html", "text/html; charset=utf-8"},
        {".css",  "text/css; charset=utf-8"},
        // ... 补全
    };

    auto it = mime.find(ext);
    return it != mime.end() ? it->second : "application/octet-stream";
}
```

---

## 十六.7 HTTP 响应格式

`BuildResponse` 拼接如下格式的字符串：

```http
HTTP/1.1 200 OK\r\n
Server: tiny-http/1.0\r\n
Content-Type: text/html; charset=utf-8\r\n
Content-Length: 1234\r\n
Connection: keep-alive\r\n
\r\n
<html>...</html>
```

### 各状态码的响应

| 状态码 | Status Message | 是否有 Body |
|--------|---------------|-------------|
| 200 | OK | ✅ 文件内容 |
| 403 | Forbidden | ✅ 简单 HTML 错误页 |
| 404 | Not Found | ✅ 简单 HTML 错误页 |
| 405 | Method Not Allowed | ✅ 简单 HTML 错误页 |
| 500 | Internal Server Error | ✅ 简单 HTML 错误页 |

### HEAD 请求处理

```cpp
if (req.GetMethod() == "HEAD")
{
    // 响应头完全一样，只是不发送 body
    // 提示：Content-Length 仍然写实际文件大小，但 body 部分留空
}
```

---

## 十六.8 ReadFile 实现提示

```cpp
bool ReadFile(const std::string& path, std::string& content)
{
    // ① 以二进制模式打开（图片等）
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    // ② 用 ate 模式获取文件大小
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    // ③ 预分配 + 一次性读入
    content.resize(static_cast<size_t>(size));
    file.read(&content[0], size);
    // 或：content.assign((std::istreambuf_iterator<char>(file)),
    //                     std::istreambuf_iterator<char>());

    return file.good();
}
```

> **注意**：二进制模式！文本模式在 Windows 上会把 `\r\n` 转成 `\n`，损坏图片/JS 文件。

---

## 十六.9 与现有代码集成

修改 `multi_reactor_http.cpp` 中的 `GenerateResponse`：

```cpp
// 原来：
static void GenerateResponse(const HttpRequest& req, Buffer* output)
{
    // 硬编码 HTML...
}

// 改为：
#include "http_static_handler.h"

static HttpStaticHandler g_handler("./www");  // 全局或传入

static void GenerateResponse(const HttpRequest& req, Buffer* output)
{
    g_handler.HandleRequest(req, output);
}
```

---

## 十六.10 测试静态文件目录

在项目目录下创建 `www/` 文件夹：

```
week01/
├── www/                     ← 新建
│   ├── index.html           ← 首页
│   ├── style.css            ← 样式
│   ├── 404.html             ← 自定义 404 页（可选）
│   └── images/              ← 图片目录（可选）
│       └── logo.png
├── http_static_handler.h    ← 你要写的
├── multi_reactor_http.cpp   ← 修改 GenerateResponse
└── ...
```

### 测试方法

```bash
# ① 编译
cd build && cmake .. && make multi_reactor_http

# ② 启动
./week01/multi_reactor_http &

# ③ 浏览器测试
curl -v http://localhost:8888/                    # 200 + HTML
curl -v http://localhost:8888/style.css           # 200 + CSS
curl -v http://localhost:8888/nonexist.html       # 404
curl -v --path-as-is http://localhost:8888/../etc/passwd  # 403

# ④ 浏览器
# 在 Linux VM 中启动后，宿主机浏览器访问 http://<VM_IP>:8888/
```

---

## 十六.11 Day 15 任务清单

| # | 任务 | 说明 | 状态 |
|---|------|------|:---:|
| 1 | **写 `http_static_handler.h`** | 包含完整的 HttpStaticHandler 类 | ✅ |
| 2 | **创建 `www/` 目录 + 测试文件** | 至少有 index.html 和 style.css | ✅ |
| 3 | **修改 `multi_reactor_http.cpp`** | 替换 GenerateResponse 为 HttpStaticHandler | ✅ |
| 4 | **编译 + 测试** | curl 验证 200/404/403，浏览器验证页面渲染 | ✅ |
| 5 | **更新笔记** | 记录实现要点和安全设计 | ✅ |

> 先根据 DESIGN.md 自己写，遇到困难再看 `reference/http_static_handler_full.h`。

---

## 十六.12 实现检查清单

完成 Day 15 后自查：

- [x] `MapPath` 正确拦截 `..` 路径穿越
- [x] `UrlDecode` 在 `..` 检查之前调用
- [x] `ReadFile` 使用二进制模式打开文件
- [x] `GetMimeType` 至少覆盖 .html/.css/.js/.png/.jpg 五种
- [x] HEAD 请求返回正确 Content-Length 但不含 body
- [x] 目录请求能自动 fallback 到 index 文件
- [x] 错误页（404/403/500）有简单的 HTML body
- [x] curl 测试全部通过

---

# Day 16 技术文档：HTTP/1.1 长连接（Keep-Alive 流水线）

---

## 十七、问题在哪

当前代码已经能识别 `Connection: keep-alive`，响应头也写了对应字段，`conn.parser.Reset()` 后连接也不关。但有个隐蔽 bug：

```
客户端一次发了两个请求（pipelining）：
  GET / HTTP/1.1\r\n...\r\n\r\nGET /style.css HTTP/1.1\r\n...\r\n\r\n
  └─────── 请求 1 ──────────┘└──────── 请求 2 ────────────┘

服务器处理：
  ① ReadFd → 两个请求一起读进 inputBuffer
  ② Parse → 消耗请求 1 的数据（Retrieve）
  ③ GenerateResponse → 写入 outputBuffer
  ④ parser.Reset()   ← 请求 2 的数据还在 buffer 里！
  ⑤ FlushWrite → 写完响应
  ⑥ return           ← 请求 2 被晾在 buffer 里，没人管了
```

下次 `epoll_wait` 不会再通知这个 fd（因为内核缓冲区已经读空了），请求 2 永远得不到响应——直到客户端超时断开。

## 十七.1 怎么修

`OnRead` 里的解析步骤加一个 `while` 循环：只要 buffer 里还有可读数据，就继续解析。

```
改前:  ReadFd → Parse一次 → Generate → FlushWrite → return
改后:  ReadFd → while (buffer有数据):
                   Parse → need_more? break（等下次数据）
                         → done?   Generate + Reset + 继续循环
                         → error?  关连接
                → FlushWrite
```

## 十七.2 改动范围

只改 **一个文件**：`multi_reactor_http.cpp` 的 `OnRead` 方法。

`Connection` 结构体加一个计数器：

```cpp
struct Connection
{
    // ... 原有字段 ...
    int requestCount = 0;  // ← 新增：keep-alive 已处理请求数
};
```

常量：

```cpp
constexpr int MAX_KEEPALIVE_REQUESTS = 100;  // 单连接最多处理 100 个请求
```

## 十七.3 OnRead 改动伪代码

```cpp
void OnRead(int fd, EventLoop* sub, int idx)
{
    // ====== 第一部分不变：读数据 ======
    ReadFd → 失败/EOF? → 关连接
           → EAGAIN?   → return

    // ====== 第二部分：循环解析（这是新加的 while）======
    bool shouldClose = false;
    while (conn.inputBuffer.ReadableBytes() > 0)
    {
        auto result = conn.parser.Parse(&conn.inputBuffer);

        if (result == PARSE_NEED_MORE)
        {
            break;  // 数据不完整，等下次
        }

        if (result == PARSE_ERROR)
        {
            shouldClose = true;
            break;
        }

        if (result == PARSE_OK && conn.parser.IsDone())
        {
            const HttpRequest& req = conn.parser.GetRequest();
            GenerateResponse(req, &conn.outputBuffer);
            conn.requestCount++;

            if (!req.IsKeepAlive() || conn.requestCount >= MAX_KEEPALIVE_REQUESTS)
            {
                shouldClose = true;
                break;
            }

            conn.parser.Reset();
            // ← 回到 while 顶部，检查 buffer 里是否还有数据
        }
    }

    if (shouldClose)
    {
        OnClose(fd, sub, idx);
        return;
    }

    FlushWrite(fd, idx);
}
```

## 十七.4 关键点

| 要点 | 说明 |
|------|------|
| `while` 条件 | `ReadableBytes() > 0`，buffer 有数据就继续 |
| `PARSE_NEED_MORE` | `break` 退出循环，等下次 `epoll_wait` |
| `PARSE_OK + IsDone` | 生成响应后用 `continue` 语义回到循环顶部 |
| `PARSE_ERROR` | `break` + 关连接 |
| `requestCount` | 达到上限就关，防止一个连接占用过久 |
| `FlushWrite` | 放在循环外面，所有响应拼完一次性写 |

## 十七.5 测试

```bash
# ① 编译运行
cmake --build . --target multi_reactor_http
./week01/multi_reactor_http &

# ② 单请求 keep-alive（curl 默认 HTTP/1.1 + keep-alive）
curl -v http://localhost:8888/

# ③ pipelining：一次发两个请求
echo -e "GET / HTTP/1.1\r\nHost: localhost\r\n\r\nGET /style.css HTTP/1.1\r\nHost: localhost\r\n\r\n" | nc localhost 8888

# ④ ab keep-alive 压测
ab -k -n 10000 -c 10 http://127.0.0.1:8888/
```

## 十七.6 Day 16 任务清单

| # | 任务 | 说明 |
|---|------|------|
| 1 | `Connection` 加 `requestCount` | 计数器 |
| 2 | 加 `MAX_KEEPALIVE_REQUESTS` 常量 | 100 |
| 3 | `OnRead` 加 `while` 循环 | 核心改动 |
| 4 | 编译 + curl/nc pipelining 测试 | 验证两个请求一次发送 |
| 5 | `ab -k` 压测对比 | 记录 keep-alive vs 短连接 QPS |

> 先根据 DESIGN.md 自己改，遇到困难再看参考。

---

## 十八、时间轮定时器（Day 17 — 7/3）

### 十八.1 问题在哪

当前 Keep-Alive 实现有个漏洞：客户端连上来后不发请求，连接就永远挂着。这叫**慢速攻击（Slowloris）**——恶意客户端只建连接不发数据，耗尽服务器 fd。

需要一个定时器机制：**连接 60 秒没 IO 活动就踢掉**。

### 十八.2 什么是时间轮

时间轮就是一个环形数组，每个槽代表一个时间片。指针每秒走一格，转到哪个槽就把里面的连接全踢掉。

```
      ┌───┬───┬───┬───┬───┐
      │ 0 │ 1 │ 2 │...│59 │  ← 60 个槽，每个 1 秒
      └───┴─▲─┴───┴───┴───┘
            │
       指针每秒走一格 → 刚好 60 秒一圈
```

- 新连接插入：算目标槽 → 放入 → O(1)
- 连接活跃刷新：旧槽移除 → 新槽插入 → O(1)  
- 连接关闭移除：查到所在槽 → 删除 → O(1)
- Tick：返回当前槽所有 fd → 清空 → 指针前进 → O(槽内 fd 数)

### 十八.3 集成架构：外部组件方案

不改 EventLoop。每个 SubReactor 独立挂一个 TimerWheel + timerfd：

```
SubReactor-0 线程:
  ├── EventLoop
  ├── subClients_[0]          ← 连接表（已有）
  ├── timerfd_0               ← 内核定时器（新增）
  ├── Channel(timerfd_0)      ← 把 timerfd 压入 epoll（新增）
  ├── TimerWheel              ← 时间轮数据结构（新增）
  └── 回调: timerfd 可读 → Tick() → 踢到期连接
```

为什么每个 SubReactor 一个：
- subClients_ 已经按 SubReactor 分片，时间轮管自己的连接
- 全部在同一个线程里，无锁

### 十八.4 TimerWheel API

```cpp
class TimerWheel
{
public:
    // 构造：槽数 + 每 tick 毫秒数
    // 例如 TimerWheel(60, 1000) = 60 槽 × 1 秒 = 60 秒超时
    TimerWheel(int slotCount, int tickMs);

    // 添加或刷新连接：fd 从现在起 timeoutMs 毫秒后到期
    void AddOrRefresh(int fd, int timeoutMs);

    // 移除连接（连接关闭时调用）
    void Remove(int fd);

    // 推动时间轮，返回本轮到期的 fd 列表
    // 调用方遍历列表，逐个关闭连接
    std::vector<int> Tick();

    // 当前时间轮中的连接数
    int Size() const;
};
```

**数据结构**：
```cpp
private:
    int slotCount_;                              // 槽总数
    int tickMs_;                                 // 每 tick 毫秒
    int currentSlot_;                            // 指针位置 (0 ~ slotCount_-1)
    std::vector<std::unordered_set<int>> slots_; // 每个槽存 fd 集合
    std::unordered_map<int, int> fdToSlot_;      // fd → 所在槽号（O(1) 删除用）
```

### 十八.5 AddOrRefresh 算法

```
AddOrRefresh(fd, timeoutMs):
    ① 如果 fd 已存在 → 从旧槽删除（通过 fdToSlot_ 找到旧槽）
    ② 计算目标槽：targetSlot = (currentSlot_ + timeoutMs / tickMs_) % slotCount_
    ③ slots_[targetSlot].insert(fd)
    ④ fdToSlot_[fd] = targetSlot
```

### 十八.6 Tick 算法

```
Tick():
    ① expired = {}  // 空列表
    ② 如果 slots_[currentSlot_] 非空：
        expired = 拷贝当前槽的所有 fd
        对每个 fd：fdToSlot_.erase(fd)
        清空 slots_[currentSlot_]
    ③ currentSlot_ = (currentSlot_ + 1) % slotCount_
    ④ return expired
```

### 十八.7 timerfd 是什么

Linux 内核提供的一个"定时器 fd"。创建后像普通 fd 一样使用：

```cpp
#include <sys/timerfd.h>

// ① 创建
int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

// ② 设置周期
struct itimerspec ts;
ts.it_value.tv_sec  = 1;   // 首次触发：1 秒后
ts.it_value.tv_nsec = 0;
ts.it_interval.tv_sec = 1; // 之后：每 1 秒触发
ts.it_interval.tv_nsec = 0;
timerfd_settime(tfd, 0, &ts, nullptr);

// ③ 加入 epoll（和 socket 一样）
// Channel 的 readCallback 里：
uint64_t expirations;
::read(tfd, &expirations, sizeof(expirations));
// read() 返回本次到期的次数（正常为 1，如果处理慢了可能 > 1）

timer_wheel_.Tick();  // 推动时间轮
```

### 十八.8 集成改动范围

**只需改 `multi_reactor_http.cpp`**，不改任何已有头文件。

#### 8.1 新增头文件

```cpp
#include <sys/timerfd.h>
#include "timer_wheel.h"
```

#### 8.2 MultiReactorServer 新增成员

```cpp
constexpr int TIMEOUT_SECONDS = 60;  // 空闲超时

std::vector<int>                   timerFds_;       // 每个 SubReactor 一个 timerfd
std::vector<std::unique_ptr<Channel>> timerChannels_; // timerfd 的 Channel
std::vector<TimerWheel>            timerWheels_;     // 每个 SubReactor 一个时间轮
```

#### 8.3 构造函数改动

在创建 SubReactor 线程的循环中，分配好 `timerFds_`/`timerChannels_`/`timerWheels_` 空间后，对每个 SubReactor：

```cpp
// 在 subLoops_[i]->RunInLoop(...) 中执行：

// ① 创建 timerfd
int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

// ② 设置每 1 秒触发
struct itimerspec ts;
ts.it_value.tv_sec  = TIMEOUT_TICK_SEC;
ts.it_value.tv_nsec = 0;
ts.it_interval.tv_sec = TIMEOUT_TICK_SEC;
ts.it_interval.tv_nsec = 0;
timerfd_settime(tfd, 0, &ts, nullptr);

// ③ 创建 Channel 并设置 readCallback
auto ch = std::make_unique<Channel>(tfd, sub->EpollPtr());
ch->SetReadCallback([this, idx]() {
    uint64_t expirations;
    ::read(timerFds_[idx], &expirations, sizeof(expirations));

    // 如果事件循环忙，可能错过了几次 tick，补转
    for (uint64_t i = 0; i < expirations; ++i)
    {
        auto expired = timerWheels_[idx].Tick();
        for (int fd : expired)
        {
            OnClose(fd, subLoops_[idx].get(), idx);
        }
    }
});

// ④ 注册到 EventLoop
sub->AddChannel(ch.get());
ch->EnableRead();
```

#### 8.4 OnAccept 改动

新连接接入后，注册到时间轮：

```cpp
// OnAccept 中，subClients_[idx].emplace 之后：
timerWheels_[idx].AddOrRefresh(client_fd, TIMEOUT_SECONDS * 1000);
```

#### 8.5 OnRead 改动

收到数据并成功处理后，刷新超时：

```cpp
// OnRead 中，GenerateResponse(...) 之后：
timerWheels_[idx].AddOrRefresh(fd, TIMEOUT_SECONDS * 1000);
```

#### 8.6 OnClose 改动

关闭连接时从时间轮移除：

```cpp
// OnClose 中，RemoveClientChannel 之前：
timerWheels_[idx].Remove(fd);
```

#### 8.7 析构函数改动

清理 timerfd 相关资源（在 join 子线程之前）：

```cpp
// 在 subLoops_[i]->Quit() 之前或之后，
// RunInLoop 中 RemoveChannel + close timerfd
for (int i = 0; i < subReactorCount; ++i)
{
    if (timerChannels_[i])
    {
        subLoops_[i]->RunInLoop([this, i]() {
            // timerChannels_[i].reset() 会触发 ~Channel，
            // ~Channel 已经会 DisableAll → epoll_->Del(fd_)
            timerChannels_[i].reset();
        });
    }
}
// 子线程 join 后 close timerfd
for (int i = 0; i < subReactorCount; ++i)
{
    if (timerFds_[i] >= 0)
        ::close(timerFds_[i]);
}
```

### 十八.9 测试

```bash
# ① 编译
cmake --build . --target multi_reactor_http

# ② 启动服务器
./week01/multi_reactor_http &

# ③ 测试：建立连接但不发数据，60 秒后连接被踢
nc localhost 8888
# （什么都不输入，等 60 秒，连接自动断开）

# ④ 测试：活跃连接不被踢
curl http://localhost:8888/
# 等 10 秒再 curl http://localhost:8888/
# 等 10 秒再 curl http://localhost:8888/
# ... 连接应该一直保持（每次请求刷新了定时器）

# ⑤ 验证时间轮本身
# 可以用 telnet 建多个连接，看 TimerWheel::Size() 输出
```

### 十八.10 Day 17 任务清单

| # | 任务 | 说明 |
|---|------|------|
| 1 | 创建 `timer_wheel.h` | 根据 §十八.4~6 自己实现 |
| 2 | 在 `multi_reactor_http.cpp` 中集成 | 根据 §十八.8 改动 |
| 3 | 编译通过 | `cmake --build . --target multi_reactor_http` |
| 4 | nc 不发送数据测试 | 60 秒后连接自动断开 |
| 5 | curl 多次请求测试 | 活跃连接不被踢 |
| 6 | 对照 `reference/timer_wheel_full.h` | 看自己实现和参考的差异 |

> 先根据 DESIGN.md 自己写 `timer_wheel.h` 和改 `multi_reactor_http.cpp`，遇到困难再看 reference/。
