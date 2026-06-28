/**
 * socket_raii.h —— RAII Socket 封装类
 *
 * 设计目标：
 *   1. fd 生命周期由对象管理：构造获取，析构释放（不会忘关）
 *   2. 只移动不拷贝：socket fd 是 OS 资源，不能有两个对象持有同一个 fd
 *   3. 每个方法包装一个系统调用，出错抛异常，不在业务代码里散落 errno 检查
 *   4. 接口极简：只有 Socket 一个类，够用不啰嗦
 *
 * 学完这个文件，你应该能回答：
 *   1. 为什么 socket fd 要用 RAII 管理？（对比 main.cpp 里手动 close 的风险）
 *   2. 移动构造函数里为什么要把原对象的 fd_ 置为 -1？
 *   3. Socket 类为什么不支持拷贝？
 *   4. 构造时失败（比如 socket() 返回 -1）怎么处理？
 *
 * 编译：此文件被 server.cpp / client.cpp 包含，不单独编译
 */

#pragma once

#include <string>
#include <stdexcept>
#include <cstring>
#include <cerrno>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

// ============================================================
// Socket —— RAII 封装 Linux TCP socket
// ============================================================
class Socket
{
public:
    // ----------------------------------------------------------
    // 构造 / 析构
    // ----------------------------------------------------------

    /**
     * 默认构造：创建一个 TCP socket (AF_INET, SOCK_STREAM)
     *
     * 为什么在构造函数里调 socket()？
     *   —— RAII 的核心思想：资源获取即初始化
     *   对象创建 = 资源获取，对象销毁 = 资源释放
     *   不存在"创建了对象但忘了调 socket()"的中间状态
     *
     * 抛异常而不是返回错误码：
     *   —— 构造函数没有返回值，出错只能抛异常
     *   这也符合 C++ 惯例：构造失败 = 对象不可用 = 异常
     */
    Socket ()
    {
        fd_ = socket (AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0)
        {
            throw std::runtime_error (
                std::string ("socket() failed: ") + std::strerror (errno));
        }
    }

    /**
     * 从已有 fd 构造（用于 accept() 返回的 fd）
     *
     * 为什么不用 explicit？
     *   —— 实际上用了 explicit，防止隐式 int → Socket 转换
     *   这样写 Socket s = 3; 会编译报错，逼你写 Socket s(3);
     */
    explicit Socket (int fd) noexcept : fd_ (fd) {}

    /**
     * 析构：自动关闭 fd
     *
     * 为什么不需要手动调 close()？
     *   —— 这就是 RAII 的价值：离开作用域自动释放
     *   即使中间抛异常、提前 return，析构函数都会执行
     *
     * 为什么检查 fd_ >= 0？
     *   —— 移动后的对象 fd_ 被置为 -1，析构时不应 close(-1)
     */
    ~Socket () noexcept
    {
        if (fd_ >= 0)
        {
            close (fd_);
        }
    }

    // ----------------------------------------------------------
    // 移动语义（只移动不拷贝）
    // ----------------------------------------------------------

    /**
     * 移动构造：把资源从 other 转移到 this
     *
     * 关键步骤：
     *   1. 接管 other 的 fd
     *   2. 把 other.fd_ 置为 -1（关键！）
     *
     * 为什么必须把 other.fd_ 置为 -1？
     *   —— 否则 other 析构时会 close(fd_)，把刚转移的资源关掉
     *   两个对象指向同一个 fd → 双 delete 等价物 → 灾难
     *   置 -1 后，other 析构时 if (fd_ >= 0) 不成立，安全跳过
     */
    Socket (Socket&& other) noexcept : fd_ (other.fd_)
    {
        other.fd_ = -1;
    }

    Socket& operator= (Socket&& other) noexcept
    {
        if (this != &other)
        {
            // 先释放自己持有的资源
            if (fd_ >= 0)
            {
                close (fd_);
            }
            // 接管 other 的资源
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    // 禁止拷贝 —— socket fd 是独占资源
    Socket (const Socket&) = delete;
    Socket& operator= (const Socket&) = delete;

    // ----------------------------------------------------------
    // 属性访问
    // ----------------------------------------------------------

    int Fd () const noexcept
    {
        return fd_;
    }

    bool Valid () const noexcept
    {
        return fd_ >= 0;
    }

    // ----------------------------------------------------------
    // socket 配置
    // ----------------------------------------------------------

    /**
     * 设置 SO_REUSEADDR —— 允许快速重启
     *
     * 为什么开发阶段必须开？
     *   —— 服务器关闭后，端口进入 TIME_WAIT 状态（等 2MSL，约 60 秒）
     *   没这个选项，改代码重启时 bind() 报 "Address already in use"
     *   有它，直接重启，秒级迭代
     *
     * 生产环境呢？
     *   —— 通常也开，因为重启时要快速恢复服务
     *   但多进程绑同一端口时不能开（会导致多进程抢连接）
     */
    void SetReuseAddr ()
    {
        int optval = 1;
        if (setsockopt (fd_, SOL_SOCKET, SO_REUSEADDR,
                        &optval, sizeof (optval)) < 0)
        {
            throw std::runtime_error (
                std::string ("setsockopt(SO_REUSEADDR) failed: ") +
                std::strerror (errno));
        }
    }

    /**
     * 设置 SO_REUSEPORT —— 允许多进程/线程绑同一端口
     *
     * 为什么需要？
     *   —— 多进程 accept 同一端口时，内核做负载均衡
     *   主从 Reactor 模式中，多个 SubReactor 线程各绑同一个端口
     *   需要 SO_REUSEPORT 才能 bind 成功
     *
     * 注意：Linux 3.9+ 才支持
     */
    void SetReusePort ()
    {
        int optval = 1;
        if (setsockopt (fd_, SOL_SOCKET, SO_REUSEPORT,
                        &optval, sizeof (optval)) < 0)
        {
            throw std::runtime_error (
                std::string ("setsockopt(SO_REUSEPORT) failed: ") +
                std::strerror (errno));
        }
    }

    /**
     * 设置非阻塞模式 —— epoll ET 的必要前提
     *
     * fcntl(fd, F_GETFL)  → 读取当前文件状态标志
     * fcntl(fd, F_SETFL)  → 写入新的文件状态标志
     *
     * 为什么 epoll ET 必须搭配非阻塞 fd？
     *   —— ET 模式下，epoll_wait 只在状态变化时通知一次
     *   你必须循环 read/write 直到 EAGAIN，才算"把数据读完/写完"
     *   如果 fd 是阻塞的，最后一次 read 会永远卡住，整个事件循环废掉
     *
     * 非阻塞 read() 语义：
     *   有数据 → 返回读取的字节数
     *   无数据 → 返回 -1，errno = EAGAIN/EWOULDBLOCK（不是真错，只是暂时没数据）
     *   对端关闭 → 返回 0
     *
     * O_NONBLOCK vs SOCK_NONBLOCK？
     *   —— SOCK_NONBLOCK 可以在 socket() 时直接设置，但只影响 socket fd
     *   fcntl() 是通用方法，对任何 fd（socket、pipe、普通文件）都有效
     */
    void SetNonBlocking ()
    {
        int old_flags = fcntl (fd_, F_GETFL, 0);
        if (old_flags < 0)
        {
            throw std::runtime_error (
                std::string ("fcntl(F_GETFL) failed: ") + std::strerror (errno));
        }

        if (fcntl (fd_, F_SETFL, old_flags | O_NONBLOCK) < 0)
        {
            throw std::runtime_error (
                std::string ("fcntl(F_SETFL, O_NONBLOCK) failed: ") +
                std::strerror (errno));
        }
    }


    // ----------------------------------------------------------
    // 服务端三步：bind → listen → accept
    // ----------------------------------------------------------

    /**
     * bind() —— 将 socket 绑定到 IP + 端口
     *
     * 为什么 INADDR_ANY (0.0.0.0)？
     *   —— 监听本机所有网卡（localhost、局域网 IP、公网 IP……）
     *   如果只监 127.0.0.1，外部机器连不上
     *
     * 为什么 htons()？
     *   —— 网络字节序是大端（big-endian），主机可能是小端
     *   htons = Host TO Network Short（16 位端口号）
     *   不转的话，端口 8888 (0x22B8) 在网络上变成 0xB822 = 47138
     */
    void Bind (uint16_t port)
    {
        struct sockaddr_in addr;
        std::memset (&addr, 0, sizeof (addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons (port);
        addr.sin_addr.s_addr = htonl (INADDR_ANY);

        if (bind (fd_, reinterpret_cast<struct sockaddr *> (&addr),
                  sizeof (addr)) < 0)
        {
            throw std::runtime_error (
                std::string ("bind() failed: ") + std::strerror (errno));
        }
    }

    /**
     * listen() —— 主动 socket → 被动（监听）socket
     *
     * backlog 参数：
     *   —— 内核为这个 socket 维护的连接队列长度
     *   队列存的是"完成三次握手、等待 accept 取出"的连接
     *   如果队列满了，新连接会收到 SYN 被忽略 / RST
     *
     *   经典值 128，高并发设 SOMAXCONN（通常 4096）
     *   但真正的并发上限还受文件描述符数量限制
     */
    void Listen (int backlog = 128)
    {
        if (listen (fd_, backlog) < 0)
        {
            throw std::runtime_error (
                std::string ("listen() failed: ") + std::strerror (errno));
        }
    }

    /**
     * accept() —— 从连接队列取出一个已完成三次握手的连接
     *
     * 返回值：
     *   —— 一个新的 Socket 对象，代表与客户端的连接
     *   调用者拿到这个 Socket 后就可以 Recv/Send
     *
     * 阻塞行为：
     *   —— 如果队列为空，accept() 会阻塞直到有新连接到达
     *   这正是阻塞式服务器的特征：一次只能服务一个客户端
     *   后面用 epoll 改为非阻塞后，就不会卡在这里了
     *
     * 注意：返回的 Socket 使用移动语义，不产生拷贝
     */
    Socket Accept ()
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof (client_addr);

        int client_fd = accept (fd_,
                                reinterpret_cast<struct sockaddr *> (&client_addr),
                                &client_len);
        if (client_fd < 0)
        {
            throw std::runtime_error (
                std::string ("accept() failed: ") + std::strerror (errno));
        }

        // 注意：client_addr 信息在这里丢失了
        // 生产代码应通过输出参数或返回值带回对端地址
        // 这里为了教学简洁，只关注 fd
        return Socket (client_fd);
    }

    /**
     * Accept() 带对端地址输出
     *
     * 为什么有两个 Accept()？
     *   —— 第一个简洁版关注"拿到连接的 fd"
     *   这个版本额外带回对端的 IP 和端口，用于日志/监控
     */
    Socket Accept (std::string& out_ip, int& out_port)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof (client_addr);

        int client_fd = accept (fd_,
                                reinterpret_cast<struct sockaddr *> (&client_addr),
                                &client_len);
        if (client_fd < 0)
        {
            throw std::runtime_error (
                std::string ("accept() failed: ") + std::strerror (errno));
        }

        out_ip = inet_ntoa (client_addr.sin_addr);
        out_port = ntohs (client_addr.sin_port);
        return Socket (client_fd);
    }

    // ----------------------------------------------------------
    // 客户端：connect()
    // ----------------------------------------------------------

    /**
     * connect() —— 客户端发起 TCP 连接
     *
     * 三次握手在这里发生：
     *   客户端发 SYN → 服务器回 SYN+ACK → 客户端回 ACK
     *   三次握手完成后 connect() 返回，此时可以用 Send/Recv 通信
     *
     * 阻塞行为：
     *   —— 如果服务器没在监听，connect() 会返回 "Connection refused"
     *   如果服务器忙但还在队列范围内，会阻塞等待握手完成
     *
     * inet_addr() 将 "127.0.0.1" 转成网络字节序的 32 位整数
     *   如果传域名（如 "www.baidu.com"），需要用 getaddrinfo() 做 DNS 解析
     */
    void Connect (const std::string& ip, uint16_t port)
    {
        struct sockaddr_in server_addr;
        std::memset (&server_addr, 0, sizeof (server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons (port);
        server_addr.sin_addr.s_addr = inet_addr (ip.c_str ());

        if (connect (fd_, reinterpret_cast<struct sockaddr *> (&server_addr),
                     sizeof (server_addr)) < 0)
        {
            throw std::runtime_error (
                std::string ("connect() to ") + ip + ":" +
                std::to_string (port) + " failed: " + std::strerror (errno));
        }
    }

    // ----------------------------------------------------------
    // 数据收发
    // ----------------------------------------------------------

    /**
     * recv() —— 从 socket 读取数据
     *
     * 返回值含义（重要！）：
     *   > 0 : 实际读到的字节数
     *   = 0 : 对端正常关闭连接（发了 FIN），socket 半关闭
     *   < 0 : 出错（对阻塞 socket，通常是真错；非阻塞可能 EAGAIN）
     *
     * 为什么不保证一次读完你想要的 N 字节？
     *   —— TCP 是流式协议，没有消息边界
     *   你发 100 字节，recv 可能一次返回 50，下次返回 50
     *   这就是"粘包"问题的根源，需要应用层协议来界定消息
     */
    ssize_t Recv (void* buf, size_t len, int flags = 0)
    {
        return recv (fd_, buf, len, flags);
    }

    /**
     * send() —— 向 socket 写数据
     *
     * 为什么不保证一次 send() 就能写完？
     *   —— 发送缓冲区可能不够大
     *   send() 返回实际写入的字节数，可能 < len
     *   生产代码要循环 send 直到写完所有数据
     *
     * 阻塞 socket：send() 会阻塞直到所有数据被内核接收
     *   但如果对端关闭连接且发送缓冲区满，会收到 SIGPIPE 信号
     *   默认行为是进程直接退出！生产代码要忽略 SIGPIPE 或设 MSG_NOSIGNAL
     */
    ssize_t Send (const void* buf, size_t len, int flags = 0)
    {
        return send (fd_, buf, len, flags);
    }

    // ----------------------------------------------------------
    // 手动关闭
    // ----------------------------------------------------------

    /**
     * Close() —— 显式关闭 socket
     *
     * 正常不需要手动调——析构函数会自动 close
     * 但如果想在对象析构前提前释放（比如 error handling），可以显式调
     *
     * 调用后 fd_ 置 -1，析构时不会重复 close
     */
    void Close ()
    {
        if (fd_ >= 0)
        {
            close (fd_);
            fd_ = -1;
        }
    }

private:
    int fd_;  // 文件描述符，-1 表示无效
};
