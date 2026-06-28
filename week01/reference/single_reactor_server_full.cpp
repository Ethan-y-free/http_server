/**
 * single_reactor_server_full.cpp — 单 Reactor 服务器完整参考实现
 *
 * 架构：EventLoop（IO 线程）+ ThreadPool（计算线程）+ EchoServer（回调编排）
 *
 * 读这个文件前，请先根据 DESIGN.md §十二 自己实现。
 * 这个文件是你写不出来时的"参考答案"。
 */

#include <iostream>
#include <string>
#include <cstring>
#include <unordered_map>
#include <memory>
#include <thread>
#include <chrono>
#include <format>

#include "socket_raii.h"
#include "epoll.h"
#include "channel.h"
#include "buffer.h"
#include "eventloop.h"
#include "threadpool.h"

constexpr uint16_t PORT = 8888;
constexpr int BACKLOG = 128;
constexpr size_t THREADS = 4;

// Connection 跟 v3 完全一样
struct Connection
{
    Socket sock;
    std::unique_ptr<Channel> channel;
    Buffer inputBuffer;
    Buffer outputBuffer;
};

class SingleReactorServer
{
public:
    SingleReactorServer(EventLoop* loop, uint16_t port, ThreadPool* pool)
        : loop_(loop), pool_(pool)
    {
        listen_sock_.SetReuseAddr();
        listen_sock_.Bind(port);
        listen_sock_.Listen(BACKLOG);
        listen_sock_.SetNonBlocking();

        std::cout << "\n========================================" << std::endl;
        std::cout << "  单 Reactor Echo Server" << std::endl;
        std::cout << "  EventLoop + ThreadPool(" << THREADS << ")" << std::endl;
        std::cout << "  测试: nc localhost " << port << std::endl;
        std::cout << "========================================\n" << std::endl;

        listen_channel_ = std::make_unique<Channel>(listen_sock_.Fd(), loop_->EpollPtr());
        listen_channel_->SetReadCallback([this]() { OnAccept(); });

        loop_->AddChannel(listen_channel_.get());
        listen_channel_->EnableRead();
    }

    ~SingleReactorServer()
    {
        // 关闭所有客户端
        for (auto& [fd, conn] : clients_)
        {
            loop_->RemoveChannel(conn.channel.get());
        }
        clients_.clear();

        // 关闭监听
        if (listen_channel_)
        {
            loop_->RemoveChannel(listen_channel_.get());
        }
    }

    SingleReactorServer(const SingleReactorServer&) = delete;
    SingleReactorServer& operator=(const SingleReactorServer&) = delete;

private:
    void OnAccept()
    {
        std::string ip;
        int port = 0;

        Socket client_sock = listen_sock_.Accept(ip, port);
        client_sock.SetNonBlocking();

        int client_fd = client_sock.Fd();

        std::cout << "[+] 新客户端: " << ip << ":" << port << " (fd=" << client_fd << ")" << std::endl;

        Connection conn;
        conn.sock = std::move(client_sock);
        conn.channel = std::make_unique<Channel>(client_fd, loop_->EpollPtr());

        conn.channel->SetReadCallback([this, client_fd]() { OnRead(client_fd); });
        conn.channel->SetWriteCallback([this, client_fd]() { OnWrite(client_fd); });
        conn.channel->SetCloseCallback([this, client_fd]() { OnClose(client_fd); });
        conn.channel->SetErrorCallback([this, client_fd]() { OnError(client_fd); });

        loop_->AddChannel(conn.channel.get());
        conn.channel->EnableRead();

        clients_.emplace(client_fd, std::move(conn));

        std::cout << "    [当前连接数: " << clients_.size() << "]" << std::endl;
    }

    void OnRead(int client_fd)
    {
        auto it = clients_.find(client_fd);
        if (it == clients_.end())
        {
            return;
        }

        Connection& conn = it->second;
        int savedErrno = 0;

        ssize_t n = conn.inputBuffer.ReadFd(client_fd, &savedErrno);
        if (n < 0)
        {
            if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK)
            {
                return;
            }
            std::cerr << "[ERROR] ReadFd fd=" << client_fd << ": " << std::strerror(savedErrno) << std::endl;
            OnClose(client_fd);
            return;
        }
        else if (n == 0)
        {
            OnClose(client_fd);
            return;
        }

        // 正常收到数据：打印日志
        std::string msg(conn.inputBuffer.Peek(), conn.inputBuffer.ReadableBytes());
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
        {
            msg.pop_back();
        }
        std::cout << "[recv " << n << "B fd=" << client_fd << "] " << msg << std::endl;

        // 关键：在 IO 线程把数据移到 outputBuffer，然后提交到线程池
        conn.outputBuffer.Append(conn.inputBuffer.Peek(), conn.inputBuffer.ReadableBytes());
        size_t len = conn.inputBuffer.ReadableBytes();
        conn.inputBuffer.RetrieveAll();

        // 提交到线程池做"业务处理"
        pool_->Run([this, client_fd, len]()
        {
            // ===== Worker 线程 =====
            std::cout << std::format("  [Worker {}] 处理 {} 字节", std::this_thread::get_id(), len) << std::endl;

            // 模拟耗时操作（将来换成 HTTP 解析/读文件/查数据库）
            // std::this_thread::sleep_for(std::chrono::milliseconds(10));

            // 通知 IO 线程发送响应
            loop_->RunInLoop([this, client_fd]()
            {
                // ===== 回到 EventLoop 线程 =====
                FlushWrite(client_fd);
            });
        });
    }

    void OnWrite(int client_fd)
    {
        FlushWrite(client_fd);
    }

    void FlushWrite(int client_fd)
    {
        auto it = clients_.find(client_fd);
        if (it == clients_.end())
        {
            return;
        }

        Connection& conn = it->second;

        while (conn.outputBuffer.ReadableBytes() > 0)
        {
            ssize_t sent = conn.sock.Send(conn.outputBuffer.Peek(), conn.outputBuffer.ReadableBytes());
            if (sent > 0)
            {
                conn.outputBuffer.Retrieve(sent);
            }
            else if (sent < 0 && errno == EAGAIN)
            {
                conn.channel->EnableWrite();
                return;
            }
            else
            {
                OnClose(client_fd);
                return;
            }
        }

        conn.channel->DisableWrite();
    }

    void OnClose(int client_fd)
    {
        std::cout << "[-] 客户端断开 (fd=" << client_fd << ")" << std::endl;

        auto it = clients_.find(client_fd);
        if (it != clients_.end())
        {
            loop_->RemoveChannel(it->second.channel.get());
            clients_.erase(it);
        }

        std::cout << "    [当前连接数: " << clients_.size() << "]" << std::endl;
    }

    void OnError(int client_fd)
    {
        std::cerr << "[!] 客户端异常 (fd=" << client_fd << ")" << std::endl;
        OnClose(client_fd);
    }

    EventLoop* loop_;
    ThreadPool* pool_;

    Socket listen_sock_;
    std::unique_ptr<Channel> listen_channel_;

    std::unordered_map<int, Connection> clients_;
};

int main()
{
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
