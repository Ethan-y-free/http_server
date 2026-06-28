/**
 * epoll_echo_server_v2.cpp —— 基于 Epoll + Channel 的并发 Echo Server（完整参考版）
 *
 * 用刚封装的 Epoll 类和 Channel 类重写 echo server。
 * 和 epoll_echo_server.cpp 功能一样，但代码结构从"面向过程"升级为"面向对象"。
 *
 * 对比 v1 看进步：原来 main 里一把梭的事，现在拆成了类
 *
 * 编译：g++ -std=c++17 epoll_echo_server_v2.cpp -o epoll_echo_server_v2
 * 运行：./epoll_echo_server_v2
 * 测试：nc localhost 8888
 */

#include <iostream>
#include <string>
#include <cstring>
#include <unordered_map>
#include <memory>

#include "socket_raii.h"
#include "epoll.h"
#include "channel.h"


constexpr uint16_t PORT = 8888;
constexpr int      BACKLOG = 128;
constexpr size_t   BUF_SIZE = 4096;


class EchoServer
{
public:
    EchoServer(uint16_t port)
    {
        // 1. 创建 listen socket + bind + listen
        listen_sock_.SetReuseAddr();
        listen_sock_.Bind(port);
        listen_sock_.Listen(BACKLOG);
        listen_sock_.SetNonBlocking();

        std::cout << "\n========================================" << std::endl;
        std::cout << "  Epoll Echo Server v2 (OO 版)" << std::endl;
        std::cout << "  Epoll + Channel 封装" << std::endl;
        std::cout << "  测试: nc localhost " << port << std::endl;
        std::cout << "========================================\n" << std::endl;

        // 2. 创建 Epoll 实例
        epoll_ = std::make_unique<Epoll>();

        // 3. 创建 listen 的 Channel，绑定读回调 → OnAccept
        listen_channel_ = std::make_unique<Channel>(listen_sock_.Fd(), epoll_.get());
        listen_channel_->SetReadCallback([this]() { OnAccept(); });
        listen_channel_->EnableRead();  // 注册 EPOLLIN
    }

    ~EchoServer()
    {
        // 先删 channel（会从 epoll 注销），再析构 socket
        client_channels_.clear();
    }

    void Start()
    {
        std::cout << "[启动] 进入事件循环..." << std::endl;

        constexpr int MAX_EVENTS = 64;
        epoll_event buf[MAX_EVENTS];

        while (true)
        {
            int nfds = epoll_->Wait(buf, MAX_EVENTS);

            for (int i = 0; i < nfds; i++)
            {
                int fd = buf[i].data.fd;

                if (fd == listen_sock_.Fd())
                {
                    listen_channel_->HandleEvent(buf[i].events);
                }
                else
                {
                    auto it = client_channels_.find(fd);
                    if (it != client_channels_.end())
                    {
                        it->second->HandleEvent(buf[i].events);
                    }
                }
            }
        }
    }

private:
    // ---- 处理新连接 ----
    void OnAccept()
    {
        std::string ip;
        int port = 0;

        Socket client_sock = listen_sock_.Accept(ip, port);
        client_sock.SetNonBlocking();

        std::cout << "[+] 新客户端: " << ip << ":" << port
                  << " (fd=" << client_sock.Fd() << ")" << std::endl;

        int client_fd = client_sock.Fd();

        // 为客户端创建 Channel（注意：Channel 存的是裸指针，生命周期由 map 管理）
        auto channel = std::make_unique<Channel>(client_fd, epoll_.get());

        // 绑定三个回调
        channel->SetReadCallback([this, client_fd]() { OnRead(client_fd); });
        channel->SetCloseCallback([this, client_fd]() { OnClose(client_fd); });
        channel->SetErrorCallback([this, client_fd]() { OnError(client_fd); });

        // 监听可读事件 + 对端关闭事件
        // 注意：必须先存 channel，再 EnableRead（EnableRead 会调 epoll Mod）
        // 但 Add 需要先做，所以这里用 epoll_->Add 手动加
        epoll_->Add(client_fd, EPOLLIN | EPOLLRDHUP);
        channel->EnableRead();  // 内部调 epoll_->Mod（此时 fd 已注册，Mod 会生效）

        client_socks_.emplace(client_fd, std::move(client_sock));
        client_channels_.emplace(client_fd, std::move(channel));

        std::cout << "    [当前连接数: " << client_channels_.size() << "]" << std::endl;
    }

    // ---- 处理客户端数据 ----
    void OnRead(int client_fd)
    {
        auto it = client_socks_.find(client_fd);
        if (it == client_socks_.end())
        {
            return;
        }

        char buf[BUF_SIZE];
        std::memset(buf, 0, BUF_SIZE);
        ssize_t n = it->second.Recv(buf, BUF_SIZE - 1);

        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return;  // 非阻塞正常情况
            }
            std::cerr << "[ERROR] recv() fd=" << client_fd << ": "
                      << std::strerror(errno) << std::endl;
            OnClose(client_fd);
            return;
        }
        else if (n == 0)
        {
            // 对端正常关闭
            OnClose(client_fd);
            return;
        }

        // 正常收到数据
        std::string msg(buf, n);
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
        {
            msg.pop_back();
        }
        std::cout << "[recv " << n << "B fd=" << client_fd << "] " << msg << std::endl;

        // echo 回去
        ssize_t sent = it->second.Send(buf, n);
        if (sent < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return;
            }
            std::cerr << "[ERROR] send() fd=" << client_fd << ": "
                      << std::strerror(errno) << std::endl;
            OnClose(client_fd);
        }
    }

    // ---- 关闭客户端 ----
    void OnClose(int client_fd)
    {
        std::cout << "[-] 客户端断开 (fd=" << client_fd << ")" << std::endl;

        // ★ 顺序很重要：先删 channel（析构 → epoll Del），再删 socket（析构 → close fd）★
        client_channels_.erase(client_fd);
        client_socks_.erase(client_fd);

        std::cout << "    [当前连接数: " << client_channels_.size() << "]" << std::endl;
    }

    // ---- 处理错误 ----
    void OnError(int client_fd)
    {
        std::cerr << "[!] 客户端异常 (fd=" << client_fd << ")" << std::endl;
        OnClose(client_fd);
    }

    Socket listen_sock_;
    std::unique_ptr<Epoll> epoll_;
    std::unique_ptr<Channel> listen_channel_;

    // fd → 资源（先删 channel 再删 socket）
    std::unordered_map<int, Socket> client_socks_;
    std::unordered_map<int, std::unique_ptr<Channel>> client_channels_;
};


int main ()
{
    try
    {
        EchoServer server(PORT);
        server.Start();
    }
    catch (const std::exception& e)
    {
        std::cerr << "[FATAL] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
