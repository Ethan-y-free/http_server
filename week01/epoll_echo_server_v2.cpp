/**
 * epoll_echo_server_v2.cpp —— 基于 Epoll + Channel 的并发 Echo Server（骨架版）
 *
 * 用刚封装的 Epoll 类和 Channel 类重写 echo server。
 * 和 epoll_echo_server.cpp 功能一样，但代码结构从"面向过程"升级为"面向对象"。
 *
 * 对比 v1 看进步：原来 main 里一把梭的事，现在拆成了类
 *
 * 编译：g++ -std=c++17 epoll_echo_server_v2.cpp -o epoll_echo_server_v2
 * 运行：./epoll_echo_server_v2
 * 测试：nc localhost 8888
 *
 * 完整参考版：reference/epoll_echo_server_v2_full.cpp
 */

#include <iostream>
#include <string>
#include <cstring>
#include <unordered_map>
#include <memory>

#include "socket_raii.h"
#include "epoll.h"
#include "channel.h"


// ==========================================================
// 常量
// ==========================================================
constexpr uint16_t PORT = 8888;
constexpr int      BACKLOG = 128;
constexpr size_t   BUF_SIZE = 4096;


// ==========================================================
// EchoServer 类
// ==========================================================
// 把原来的 main 逻辑封装进类
// TODO: class EchoServer
class Socket;
class Channel;

class EchoServer
{
public:
    // ---- 构造 ----
    // TODO: EchoServer(uint16_t port)
    //   1. 创建 listen_sock_ + SetReuseAddr + Bind + Listen + SetNonBlocking
    //   2. 创建 epoll_ (new Epoll)
    //   3. 创建 listen_channel_ (new Channel(listen_fd, epoll_))
    //   4. listen_channel_->SetReadCallback 绑定 this->OnAccept
    //   5. listen_channel_->EnableRead
    EchoServer(uint16_t port)
    {
        listen_sock_.SetReuseAddr();
        listen_sock_.Bind(port);
        listen_sock_.Listen(BACKLOG);
        listen_sock_.SetNonBlocking();

        std::cout << "\n========================================" << std::endl;
        std::cout << "  Epoll Echo Server v2 (OO 版)" << std::endl;
        std::cout << "  Epoll + Channel 封装" << std::endl;
        std::cout << "  测试: nc localhost " << port << std::endl;
        std::cout << "========================================\n" << std::endl;

        epoll_ = std::make_unique<Epoll>();

        listen_channel_ = std::make_unique<Channel>(listen_sock_.Fd(), epoll_.get());
        listen_channel_->SetReadCallback([this]() { OnAccept(); });
        listen_channel_->EnableRead();  // 注册 EPOLLIN
    }

    // ---- 启动 ----
    // TODO: void Start()
    //   事件循环：while(true) { epoll_->Wait() → 遍历 ready → channel->HandleEvent }
    void Start()
    {
        std::cout << "[启动] 进入事件循环..." << std::endl;

        constexpr int MAX_EVENTS = 64;
        epoll_event buf[MAX_EVENTS];

        while (true)
        {
            int nfds = epoll_->Wait(buf, MAX_EVENTS);
            for (int i = 0; i < nfds; ++i)
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

    // ---- 析构 ----
    // TODO: ~EchoServer()
    //   清理 clients_ 中的 Channel（先删 channel 再关 socket）
    ~EchoServer()
    {
        client_channels_.clear();
    }

private:
    // ---- 处理新连接 ----
    // TODO: void OnAccept()
    //   1. listen_sock_.Accept(ip, port)
    //   2. client_sock.SetNonBlocking
    //   3. new Channel(client_fd, epoll_.get())
    //   4. 设置三个回调：读/关闭/错误
    //   5. channel->EnableRead
    //   6. 把 client_sock 和 channel 存入 clients_
    void OnAccept()
    {
        std::string ip;
        int port = 0;

        Socket client_sock = listen_sock_.Accept(ip, port);
        if (client_sock.Fd() < 0) return;
        client_sock.SetNonBlocking();

        std::cout << "[+] 新客户端: " << ip << ":" << port << " (fd=" << client_sock.Fd() << ")" << std::endl;

        int client_fd = client_sock.Fd();

        auto channel = std::make_unique<Channel>(client_fd, epoll_.get());

        channel->SetReadCallback([this, client_fd]() { OnRead(client_fd); });
        channel->SetCloseCallback([this, client_fd]() { OnClose(client_fd); });
        channel->SetErrorCallback([this, client_fd]() { OnError(client_fd); });

        epoll_->Add(client_fd, EPOLLIN | EPOLLRDHUP);
        channel->EnableRead();

        client_socks_.emplace(client_fd, std::move(client_sock));
        client_channels_.emplace(client_fd, std::move(channel));

        std::cout << "    [当前连接数: " << client_channels_.size() << "]" << std::endl;
    }

    // ---- 处理客户端数据 ----
    // TODO: void OnRead(int client_fd)
    //   1. 从 clients_ 找到 client_sock
    //   2. recv(buf)
    //   3. if (n<=0) → 关闭连接
    //   4. else → send 回显
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
            std::cerr << "[ERROR] recv() fd=" << client_fd << ": " << std::strerror(errno) << std::endl;
            OnClose(client_fd);
            return;
        }
        else if (n == 0)
        {
            // 对端正常关闭
            OnClose(client_fd);
            return;
        }
        else
        {
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
                std::cerr << "[ERROR] send() fd=" << client_fd << ": " << std::strerror(errno) << std::endl;
                OnClose(client_fd);
            }
        }
    }

    // ---- 关闭客户端 ----
    // TODO: void OnClose(int client_fd)
    //   打印断开信息，从 clients_ 移除
    void OnClose(int client_fd)
    {
        std::cout << "[-] 客户端断开 (fd=" << client_fd << ")" << std::endl;

        client_channels_.erase(client_fd);
        client_socks_.erase(client_fd);

        std::cout << "    [当前连接数: " << client_channels_.size() << "]" << std::endl;
    }

    // ---- 处理错误 ----
    // TODO: void OnError(int client_fd)
    //   打印错误信息，关闭连接
    void OnError(int client_fd)
    {
        std::cerr << "[!] 客户端异常 (fd=" << client_fd << ")" << std::endl;
        OnClose(client_fd);
    }

    // 成员变量
    // TODO:
    Socket listen_sock_;
    std::unique_ptr<Epoll> epoll_;
    std::unique_ptr<Channel> listen_channel_;

    std::unordered_map<int, Socket> client_socks_;
    std::unordered_map<int, std::unique_ptr<Channel>> client_channels_;
};


// ==========================================================
// main
// ==========================================================

int main ()
{
    try
    {
        // TODO: EchoServer server(PORT); server.Start();
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
