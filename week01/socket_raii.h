#pragma once

#include <string>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <cerrno>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>


class Socket
{
public:
    // ==========================================================
    // ① 构造 / 析构
    // ==========================================================

    // 默认构造：调用 socket(AF_INET, SOCK_STREAM, 0)，失败抛异常
    Socket()
    {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0)
        {
            throw std::runtime_error("socket() failed: " + std::string(std::strerror(errno)));
        }
    }

    // 从已有 fd 构造（accept 返回的 fd），explicit 防止隐式转换
    explicit Socket(int fd) noexcept : fd_(fd) {}

    // 析构：fd_ >= 0 时 close(fd_)
    ~Socket() noexcept
    {
        if (fd_ >= 0)
        {
            close(fd_);
        }
    }

    // ==========================================================
    // ② 移动 / 拷贝
    // ==========================================================

    // TODO: 移动构造 —— 接管 other.fd_，把 other.fd_ 置为 -1
    Socket(Socket&& other) noexcept : fd_(other.fd_)
    {
        other.fd_ = -1;
    }

    // TODO: 移动赋值 —— 先释放自身，再接管 other
    Socket& operator= (Socket&& other) noexcept
    {
        if (this != &other)
        {
            if (fd_ >= 0)
            {
                close(fd_);
            }
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    // 禁止拷贝
    Socket (const Socket&) = delete;
    Socket& operator= (const Socket&) = delete;


    // ==========================================================
    // ③ 属性访问
    // ==========================================================

    int  Fd ()    const noexcept  // 返回 fd_
    {
        return fd_;
    }

    bool Valid () const noexcept  // fd_ >= 0 ?
    {
        return fd_ >= 0;
    }


    // ==========================================================
    // ④ socket 配置
    // ==========================================================

    // TODO: setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &1, sizeof(1))
    // 开发需要 快速重启
    void SetReuseAddr() 
    {
        int optval = 1;
        if (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
        {
            throw std::runtime_error(std::string("setsockopt(SO_REUSEADDR) failed: ") + std::strerror(errno));
        }
    }

    // TODO: setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &1, sizeof(1))
    // 设置 SO_REUSEPORT —— 允许多进程 / 线程绑同一端口
    void SetReusePort()
    {
        int optval = 1;
        if (setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) < 0)
        {
            throw std::runtime_error(std::string("setsockopt(SO_REUSEPORT) failed: ") + std::strerror(errno));
        }
    }

    // TODO: SetNonBlocking() —— 用 fcntl(fd_, F_SETFL, flags | O_NONBLOCK) 设为非阻塞
    //   epoll ET 模式必须搭配非阻塞 fd，否则 read/write 可能阻塞整个事件循环
    //   提示：先 fcntl(fd_, F_GETFL, 0) 拿到旧 flags，再 fcntl(fd_, F_SETFL, old_flags | O_NONBLOCK)
    void SetNonBlocking()
    {
        // TODO: 实现
        int old_flags = fcntl(fd_, F_GETFL, 0);
        if (old_flags < 0)
        {
            throw std::runtime_error(std::string("fcntl(F_GETFL) failed: ") + std::strerror(errno));
        }
        if (fcntl(fd_, F_SETFL, old_flags | O_NONBLOCK) < 0)
        {
            throw std::runtime_error(std::string("fcntl(F_SETFL, O_NONBLOCK) failed: ") + std::strerror(errno));
        }
    }


    // ==========================================================
    // ⑤ 服务端三步
    // ==========================================================

    // TODO: sockaddr_in 填 AF_INET + htons(port) + htonl(INADDR_ANY)
    //       调 bind(fd_, ...)
    void Bind(uint16_t port)
    {
        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(fd_, (sockaddr*)&addr, sizeof(addr)) < 0)
        {
            throw std::runtime_error(std::string("bind() failed: ") + std::strerror(errno));
        }
    }

    // TODO: listen(fd_, backlog)
    void Listen(int backlog = 128)
    {
        if (listen(fd_, backlog) < 0)
        {
            throw std::runtime_error(std::string("listen() failed: ") + std::strerror(errno));
        }
    }

    // TODO: accept(fd_, ...) → 返回 Socket(client_fd)
    Socket Accept()
    {
        sockaddr_in caddr;
        socklen_t caddrlen = sizeof(caddr);
        int cfd = accept(fd_, (sockaddr*)&caddr, &caddrlen);

        if (cfd < 0)
        {
            throw std::runtime_error(std::string("accept() failed: ") + std::strerror(errno));
        }

        return Socket (cfd);
    }

    // 带对端地址输出的 Accept
    Socket Accept(std::string& out_ip, int& out_port)
    {
        sockaddr_in caddr;
        socklen_t caddrlen = sizeof(caddr);
        int cfd = accept(fd_, (sockaddr*)&caddr, &caddrlen);

        if (cfd < 0)
        {
            throw std::runtime_error(std::string("accept() failed: ") + std::strerror(errno));
        }

        out_ip = inet_ntoa(caddr.sin_addr);   // 网络字节序的 IP → "192.168.1.5"
        out_port = ntohs(caddr.sin_port);     // 网络字节序的端口 → 54321

        return Socket(cfd);
    }


    // ==========================================================
    // ⑥ 客户端连接
    // ==========================================================

    // TODO: sockaddr_in 填 AF_INET + htons(port) + inet_addr(ip)
    //       调 connect(fd_, ...)
    void Connect(const std::string& ip, uint16_t port)
    {
        sockaddr_in saddr;
        std::memset(&saddr, 0, sizeof(saddr));
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons(port);
        saddr.sin_addr.s_addr = inet_addr(ip.c_str());

        if (connect(fd_, (sockaddr*)&saddr, sizeof(saddr)) < 0)
        {
            throw std::runtime_error(std::string("connect() to ") + ip + ":" + std::to_string(port) + " failed: " + std::strerror(errno));
        }
    }


    // ==========================================================
    // ⑦ 数据收发
    // ==========================================================

    ssize_t Recv(void* buf, size_t len, int flags = 0)   // → recv(fd_, ...)
    {
        return recv(fd_, buf, len, flags);
    }

    ssize_t Send(const void* buf, size_t len, int flags = 0) // → send(fd_, ...)
    {
        return send(fd_, buf, len, flags);
    }


    // ==========================================================
    // ⑧ 手动关闭
    // ==========================================================

    // TODO: 如果 fd_ >= 0，close(fd_)，然后 fd_ = -1
    void Close()
    {
        if (fd_ >= 0)
        {
            close (fd_);
            fd_ = -1;
        }
    }

    int ReleaseFd() noexcept
    {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_ = -1;   // 文件描述符，-1 = 无效，默认初始化为 -1
};
