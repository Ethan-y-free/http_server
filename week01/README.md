# 高并发 HTTP 服务器 — 项目一

> C++17 | epoll | 主从 Reactor | 时间轮定时器 | 异步日志
>
> 大一暑期项目 · 2026 年 6 月–7 月

---

## 一、项目概述

从零构建一个**生产级高并发 HTTP/1.1 静态文件服务器**，逐步演进：

```
阻塞式 echo server  →  epoll 非阻塞  →  单 Reactor  →  主从 Reactor
    →  HTTP 协议解析  →  静态文件服务  →  Keep-Alive  →  时间轮定时器
    →  异步日志  →  模块化重构（TcpConnection / SubReactor / TcpServer）
```

最终版本 `v1_http_server` 是一个**模块化、可复用的网络库雏形**，约 30 行组装代码即可启动完整的 HTTP 服务器。

---

## 二、架构总览

### 2.1 主从 Reactor + one loop per thread

```
                        ┌───────────────────────┐
                        │     MainReactor        │
                        │   (main thread)        │
                        │                        │
                        │  epoll_wait(listen_fd) │
                        │         │              │
                        │    OnAccept()          │
                        │    RoundRobin ─────────┼──────┐
                        └───────────────────────┘      │
                                                       │
              ┌────────────────────────────────────────┘
              │           client_fd 分发
              ▼
   ┌──────────────────────┐  ┌──────────────────────┐
   │   SubReactor[0]      │  │   SubReactor[1] ...   │
   │   (worker thread)    │  │   (worker thread)     │
   │                      │  │                       │
   │  EventLoop           │  │  EventLoop            │
   │   ├─ epoll_wait()    │  │   ├─ epoll_wait()     │
   │   ├─ timerfd (1s)    │  │   ├─ timerfd (1s)     │
   │   └─ clients_ map    │  │   └─ clients_ map     │
   │       ├─ TcpConn[fd₁]│  │       ├─ TcpConn[fd₃] │
   │       ├─ TcpConn[fd₂]│  │       └─ TcpConn[fd₄] │
   │       └─ ...         │  │                       │
   └──────────────────────┘  └──────────────────────┘
```

- **MainReactor**：只做 `accept()`，通过 `std::atomic<int>` RoundRobin 无锁分发到 SubReactor
- **SubReactor**：每个子线程一个 EventLoop，管理一组客户端连接的所有 IO
- **线程安全**：`RunInLoop` + `QueueInLoop`（eventfd 唤醒）保证 SubReactor 内部数据无竞争

### 2.2 模块关系图

```
┌─────────────────────────────────────────────────────┐
│                   v1_http_server.cpp                  │
│                  (~30 行组装代码)                      │
│                                                       │
│  AsyncLogWriter → HttpStaticHandler → onMessage      │
│       → TcpServerConfig → TcpServer::Start()          │
└──────┬──────────────┬──────────────┬─────────────────┘
       │              │              │
       ▼              ▼              ▼
┌──────────────┐ ┌──────────┐ ┌──────────────────────┐
│  TcpServer   │ │  HTTP 层  │ │  异步日志             │
│              │ │           │ │                      │
│ TcpServer.h  │ │ http_     │ │ async_logger/        │
│ SubReactor.h │ │ parser.h  │ │  ├─ log_stream.h     │
│ TcpConn…on.h │ │ http_     │ │  ├─ log_buffer.h     │
│              │ │ request.h │ │  ├─ async_log_writer │
│              │ │ http_     │ │  └─ logger.h          │
│              │ │ static_   │ │                      │
│              │ │ handler.h │ │                      │
└──────┬───────┘ └──────────┘ └──────────────────────┘
       │
       ▼
┌─────────────────────────────────────────────────────┐
│                    核心层                             │
│                                                       │
│  eventloop.h    — one loop per thread, RunInLoop     │
│  epoll.h        — Epoll RAII 封装                    │
│  channel.h      — fd + 事件回调                       │
│  socket_raii.h  — Socket RAII, Accept/ReleaseFd      │
│  buffer.h       — 应用层读写缓冲区                     │
│  timer_wheel.h  — O(1) 时间轮踢空闲连接               │
│  threadpool.h   — C++11 线程池（早期版本使用）         │
└─────────────────────────────────────────────────────┘
```

---

## 三、核心模块说明

| 模块 | 文件 | 行数 | 职责 |
|------|------|------|------|
| **Socket** | `socket_raii.h` | ~200 | RAII 管理 fd，Accept/ReleaseFd 所有权移交 |
| **Epoll** | `epoll.h` | ~130 | epoll 实例 RAII，Add/Mod/Del/Wait |
| **Channel** | `channel.h` | ~170 | fd + 四种事件回调（Read/Write/Close/Error） |
| **Buffer** | `buffer.h` | ~110 | 应用层缓冲区，ReadFd/Append/Retrieve |
| **EventLoop** | `eventloop.h` | ~100 | one loop per thread，RunInLoop/QueueInLoop |
| **TcpConnection** | `tcp_connection.h` | ~180 | TCP 连接生命周期：OnRead → messageCallback → Send → OnWrite |
| **SubReactor** | `sub_reactor.h` | ~150 | 子线程 EventLoop + 连接池 + timerfd 定时器 |
| **TcpServer** | `tcp_server.h` | ~80 | 三段式 Start()：建 SubReactor → bind/listen → 启动线程 |
| **HttpParser** | `http_parser.h` | ~120 | HTTP/1.1 状态机，PARSE_OK/NEED_MORE/ERROR |
| **HttpStaticHandler** | `http_static_handler.h` | ~220 | 静态文件服务，MIME 映射，200/404/500 |
| **TimerWheel** | `timer_wheel.h` | ~40 | 60 槽时间轮，O(1) 插入/删除，1 秒 tick |
| **AsyncLogger** | `async_logger/` | ~300 | 双缓冲（4MB×2）+ 后台线程写盘 |

---

## 四、编译与运行

### 4.1 环境要求

| 项目 | 要求 |
|------|------|
| 操作系统 | Linux（Ubuntu 22.04+ / WSL2） |
| 编译器 | GCC 11+ 或 Clang 14+ |
| 构建工具 | CMake 3.10+ |
| 依赖 | pthread（系统自带） |

### 4.2 编译

```bash
cd projects/http_server/week01
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make v1_http_server -j$(nproc)
```

### 4.3 运行

```bash
./v1_http_server
# 输出：服务启动在 0.0.0.0:8888
# 日志写入 build/server.log
```

浏览器访问 `http://localhost:8888/` → 校园导览页面（login → welcome → 3D map）

### 4.4 其他构建目标

```bash
# 基础 TCP Echo
make tcp_server tcp_client -j$(nproc)

# Epoll Echo Server（三个版本：LT 原始 / 封装 / Buffer）
make epoll_echo_server epoll_echo_server_v2 epoll_echo_server_v3 -j$(nproc)

# Reactor Echo Server（单 / 主从）
make single_reactor_server multi_reactor_server -j$(nproc)

# HTTP Server（单 Reactor / 主从 / v1.0 模块化）
make single_reactor_http multi_reactor_http -j$(nproc)
```

---

## 五、压测报告

### 5.1 测试环境

| 项目 | 值 |
|------|-----|
| 硬件 | VMware Ubuntu，4 核，8G 内存 |
| 工具 | wrk（v1）、ab（早期版本） |
| 测试文件 | `/index.html` |
| 并发模型 | v1_http_server: 1 MainReactor + 4 SubReactor |

### 5.2 wrk 压测（v1_http_server，2026/07/24）

```
wrk -t4 -c100 -d30s http://localhost:8888/index.html
```

| 指标 | 数值 |
|------|------|
| **QPS** | **6,486 req/s** |
| 线程数 | 4 |
| 并发连接 | 100 |
| 持续时间 | 30 秒 |
| 失败请求 | 0 |

### 5.3 ab 压测（早期版本对比，2026/07/01）

| 场景 | 单 Reactor | 主从 Reactor |
|------|-----------|-------------|
| Keep-Alive, 100 并发 | 8,113 QPS | 3,528 QPS |
| Keep-Alive, 1000 并发 | 8,521 QPS | 3,257 QPS |
| 短连接, 200 并发 | 2,278 QPS | 782 QPS |

> **结论**：在低~中并发下，单 Reactor 无线程切换开销，QPS 更高。主从 Reactor 的优势在高并发（万级以上）场景中体现——accept 和 IO 分离，避免 accept 惊群。v1 模块化版本通过减少锁竞争、优化内存分配，在 wrk 测试中达到 6,486 QPS。

### 5.4 性能分析

v1 相比早期 multi_reactor_http 的改进：
- **无锁 RoundRobin**：`std::atomic<int>::fetch_add(1)` 分发连接，替代 mutex
- **延迟删除**：`QueueInLoop` 延迟 delete，避免 HandleEvent 栈内 use-after-free
- **异步日志**：双缓冲 + 后台线程，IO 线程不阻塞在磁盘写入

当前瓶颈：单机 VM 4 核环境，CPU 密集（HTTP 解析 + 文件 IO + 系统调用）。进一步优化方向：内存池、零拷贝 sendfile、LFU 缓存。

---

## 六、项目文件结构

```
week01/
├── README.md                   ← 本文件
├── DESIGN.md                   ← 完整技术文档（~4300 行，21 节）
├── CMakeLists.txt              ← 构建配置（11 个 target）
│
├── v1_http_server.cpp          ← ★ 模块化 HTTP Server v1.0（~30 行组装）
│
├── 核心层（header-only）
│   ├── socket_raii.h           ← Socket RAII + Accept/ReleaseFd
│   ├── epoll.h                 ← Epoll 实例 RAII
│   ├── channel.h               ← fd + 事件回调注册
│   ├── buffer.h                ← 应用层缓冲区
│   ├── eventloop.h             ← one loop per thread
│   ├── timer_wheel.h           ← 时间轮定时器
│   └── threadpool.h            ← C++11 线程池
│
├── 网络层（header-only）
│   ├── tcp_connection.h        ← TCP 连接生命周期管理
│   ├── sub_reactor.h           ← 子线程 EventLoop + 连接池
│   └── tcp_server.h            ← TcpServer + 配置 + RoundRobin
│
├── HTTP 层（header-only）
│   ├── http_request.h          ← HttpRequest 数据结构
│   ├── http_parser.h           ← HTTP/1.1 状态机解析器
│   └── http_static_handler.h   ← 静态文件服务 + MIME
│
├── 异步日志（header-only）
│   └── async_logger/
│       ├── log_stream.h        ← 4KB 格式化缓冲区
│       ├── log_buffer.h        ← 双缓冲（4MB×2）
│       ├── async_log_writer.h  ← 后台线程集中写盘
│       └── logger.h            ← Logger RAII + 宏
│
├── 历史版本（演进过程）
│   ├── tcp_server.cpp          ← 阻塞式 TCP echo server
│   ├── tcp_client.cpp          ← 阻塞式 TCP echo client
│   ├── epoll_echo_server.cpp   ← epoll LT 原始版
│   ├── epoll_echo_server_v2.cpp← epoll + 封装版
│   ├── epoll_echo_server_v3.cpp← epoll + Buffer 集成版
│   ├── single_reactor_server.cpp← 单 Reactor Echo Server
│   ├── multi_reactor_server.cpp ← 主从 Reactor Echo Server
│   ├── single_reactor_http.cpp  ← 单 Reactor HTTP Server
│   └── multi_reactor_http.cpp   ← 主从 Reactor HTTP Server（旧版）
│
├── reference/                  ← 参考实现（完整版对照）
├── scripts/
│   └── bench.sh                ← ab 压测脚本（v1 vs multi_reactor 对比）
├── docs/                       ← PPT/文档生成脚本
└── www/                        ← 前端静态资源（校园导览站点）
    ├── index.html / login.html / welcome.html / map.html
    ├── style.css / login.css / welcome.css
    └── photos/                 ← 校园实景照片
```

---

## 七、关键技术点

### 7.1 为什么单 Reactor QPS 反而更高？

在 4 核 VM、100~1000 并发的测试条件下：
- **单 Reactor**：一个线程处理 accept + 所有 IO，无上下文切换 → CPU 缓存友好
- **主从 Reactor**：MainReactor + 4 SubReactor = 5 个线程，RunInLoop/eventfd 跨线程通信有开销

主从 Reactor 的收益在**万级以上并发**时才显现——accept 成为瓶颈时需要分离。这也说明：**架构选型要看场景，没有银弹**。

### 7.2 epoll ET + 非阻塞 IO

- **ET（边缘触发）**：只在 fd 状态变化时通知一次，必须循环读到 EAGAIN
- **非阻塞 fd**：`SetNonBlocking()` 必须设，否则 ET 下 `read()` 可能永久阻塞
- **EPOLLONESHOT**：每次就绪后自动摘除，处理完再注册，避免多线程竞争同一 fd

### 7.3 RunInLoop 线程桥

```
线程 A (MainReactor)                 线程 B (SubReactor)
      │                                     │
      │  subReactor->AddConnection(fd)       │
      │  → RunInLoop(lambda)                 │
      │     ├─ pending_functors_.push()      │
      │     └─ eventfd write (敲门)          │
      │                                     │  epoll_wait 被唤醒
      │                                     │  → 读 eventfd
      │                                     │  → 执行 pending_functors_
      │                                     │  → new TcpConnection(fd)
```

### 7.4 延迟删除防止 use-after-free

```cpp
// OnClose 中不能直接 delete this：
//   TcpConnection::OnClose → ... → SubReactor 从 map 中 erase
//   → unique_ptr 析构 → ~TcpConnection → ~Channel
//   → Epoll::Del(fd) → 返回 HandleEvent → 访问已析构的 Channel → 💥

// 修复：QueueInLoop 延迟到下一轮事件循环
conn->SetCloseCallback([this](TcpConnection* c) {
    int fd = c->Fd();
    loop_->QueueInLoop([this, fd]() {
        connections_.erase(fd);  // 安全：HandleEvent 已返回
    });
});
```

---

## 八、简历素材

> **高并发 HTTP 服务器** | C++17, epoll, Reactor 模式
>
> - 实现主从 Reactor + one loop per thread 架构，MainReactor 负责 accept，SubReactor 处理 IO
> - 基于状态机的 HTTP/1.1 协议解析，支持 GET 静态文件、Keep-Alive 长连接、pipeline
> - 自研时间轮定时器（O(1) 插入/删除），踢除空闲连接防止 Slowloris 攻击
> - 双缓冲异步日志系统（4MB×2），后台线程集中写盘，IO 线程零阻塞
> - RoundRobin 无锁分发连接（std::atomic），QueueInLoop 延迟删除保障线程安全
> - 修复 7 个编译/运行时 bug（use-after-free、ET 阻塞、事件循环断言等）
> - wrk 压测 6,486 QPS（4 核 8G VM，100 并发，零失败）

---

## 九、学习笔记

完整开发笔记见 `projects/notes/c++ http服务器开发.txt`，涵盖：
- Socket API 逐函数精讲
- epoll LT/ET/ONESHOT 四种模式对比
- Reactor 模式演进（单 Reactor → 主从 Reactor → 模块化重构）
- HTTP/1.1 协议状态机设计
- 踩坑记录：use-after-free、fd 泄漏、epoll ET 阻塞、跨线程安全

---

## 十、下一步

本项目的网络层（TcpConnection / SubReactor / TcpServer / EventLoop）已具备**可复用网络库雏形**。

**项目二**（计划 7 月中–8 月底）：仿 muduo 高性能网络库
- 重构 EventLoop/Channel/Poller 对标 muduo 接口
- 新增 TcpClient / Connector / Acceptor
- 内存池 + LFU 缓存
- Google Test 全覆盖 + Docker 化

---

> 📅 创建时间：2026/07/25 · Day 21 阶段总结
