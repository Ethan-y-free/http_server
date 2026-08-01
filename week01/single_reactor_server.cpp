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
        listen_sock_.Listen(128);
        listen_sock_.SetNonBlocking();

        std::cout << "\n========================================" << std::endl;
        std::cout << "  单 Reactor Echo Server" << std::endl;
        std::cout << "  EventLoop + ThreadPool" << std::endl;
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

    // 禁止拷贝/移动
    SingleReactorServer(const SingleReactorServer&) = delete;
    SingleReactorServer& operator=(const SingleReactorServer&) = delete;

private:
    void OnAccept()
    {
        std::string ip;
        int port = 0;
        Socket client = listen_sock_.Accept(ip, port);
        if (client.Fd() < 0) return;
        client.SetNonBlocking();
        int client_fd = client.Fd();

        Connection conn;
        conn.sock = std::move(client);
        conn.channel = std::make_unique<Channel>(client_fd, loop_->EpollPtr());

        conn.channel->SetReadCallback([this, fd = client_fd]() { OnRead(fd); });
        conn.channel->SetWriteCallback([this, fd = client_fd]() { OnWrite(fd); });
        conn.channel->SetCloseCallback([this, fd = client_fd]() { OnClose(fd); });
        conn.channel->SetErrorCallback([this, fd = client_fd]() { OnError(fd); });
        loop_->AddChannel(conn.channel.get());
        conn.channel->EnableRead();

        clients_.emplace(client_fd, std::move(conn));
        std::cout << "    [当前连接数: " << clients_.size() << "]" << std::endl;
    }

    void OnRead(int fd)
    {
        auto it = clients_.find(fd);
        if (it == clients_.end())
        {
            return;
        }
        Connection& conn = it->second;

        int savedErrno = 0;
        ssize_t n = conn.inputBuffer.ReadFd(fd, &savedErrno);

        if (n < 0)
        {
            if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK)
            {
                return;
            }
            std::cerr << "[ERROR] ReadFd fd=" << fd << ": " << std::strerror(savedErrno) << std::endl;
            OnClose(fd);
            return;
        }
        else if (n == 0)
        {
            OnClose(fd);
            return;
        }

        std::string msg(conn.inputBuffer.Peek(), conn.inputBuffer.ReadableBytes());
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
        {
            msg.pop_back();
        }
        std::cout << "[recv " << n << "B fd=" << fd << "] " << msg << std::endl;

        conn.outputBuffer.Append(conn.inputBuffer.Peek(), conn.inputBuffer.ReadableBytes());
        conn.inputBuffer.RetrieveAll();

        pool_->Run([this, fd]()
        {
            loop_->RunInLoop([this, fd]
            {
                FlushWrite(fd);
            });
        });
    }

    void OnWrite(int fd)
    {
        FlushWrite(fd);
    }

    void OnClose(int fd)
    {
        std::cout << "[-] 客户端断开 (fd=" << fd << ")" << std::endl;

        auto it = clients_.find(fd);
        if (it != clients_.end())
        {
            loop_->RemoveChannel(it->second.channel.get());
            clients_.erase(it);
        }

        std::cout << "    [当前连接数: " << clients_.size() << "]" << std::endl;
    }

    void OnError(int fd)
    {
        std::cerr << "[!] 客户端异常 (fd=" << fd << ")" << std::endl;
        OnClose(fd);
    }

    void FlushWrite(int fd)
    {
        auto it = clients_.find(fd);
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
                OnClose(fd);
                return;
            }
        }

        conn.channel->DisableWrite();
    }

    EventLoop* loop_;       // 不拥有
    ThreadPool* pool_;      // 不拥有

    Socket listen_sock_;
    std::unique_ptr<Channel> listen_channel_;

    std::unordered_map<int, Connection> clients_;
};

constexpr uint16_t PORT = 8888;
constexpr size_t   THREADS = 4;

int main()
{
    try
    {
        EventLoop loop;
        ThreadPool pool(THREADS);

        SingleReactorServer server(&loop, PORT, &pool);

        std::cout << "[启动] 进入 EventLoop..." << std::endl;
        loop.Loop();

        std::cout << "[退出] EventLoop 正常结束" << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[FATAL] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}