#include <iostream>
#include <string>
#include <cstring>
#include <unordered_map>
#include <memory>

#include "socket_raii.h"
#include "epoll.h"
#include "channel.h"
#include "buffer.h"

constexpr uint16_t PORT = 8888;
constexpr int BACKLOG = 128;

struct Connection
{
	Socket sock;
	std::unique_ptr<Channel> channel;
	Buffer inputBuffer;
	Buffer outputBuffer;
};

class EchoServer
{
public:
    EchoServer(uint16_t port)
    {
        listen_sock_.SetReuseAddr();
        listen_sock_.Bind(port);
        listen_sock_.Listen(BACKLOG);
        listen_sock_.SetNonBlocking();

        std::cout << "\n========================================" << std::endl;
        std::cout << "  Epoll Echo Server v3 (Buffer 集成版)" << std::endl;
        std::cout << "  readv + 非阻塞写 + outputBuffer" << std::endl;
        std::cout << "  测试: nc localhost " << port << std::endl;
        std::cout << "========================================\n" << std::endl;

        epoll_ = std::make_unique<Epoll>();

        listen_channel_ = std::make_unique<Channel>(listen_sock_.Fd(), epoll_.get());
        listen_channel_->SetReadCallback([this]() { OnAccept(); });
        listen_channel_->EnableRead();
    }

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
                    auto it = clients_.find(fd);
                    if (it != clients_.end())
                    {
                        it->second.channel->HandleEvent(buf[i].events);
                    }
                }
            }
        }
    }

private:
    void OnAccept()
    {
        std::string ip;
        int port = 0;

        Socket client_sock = listen_sock_.Accept(ip, port);
        if (client_sock.Fd() < 0) return;
        client_sock.SetNonBlocking();

        std::cout << "[+] 新客户端: " << ip << ":" << port << " (fd=" << client_sock.Fd() << ")" << std::endl;

        int client_fd = client_sock.Fd();

        Connection conn;
        conn.sock = std::move(client_sock);
        conn.channel = std::make_unique<Channel>(client_fd, epoll_.get());

        conn.channel->SetReadCallback([this, client_fd]() { OnRead(client_fd); });
        conn.channel->SetWriteCallback([this, client_fd]() { OnWrite(client_fd); });
        conn.channel->SetCloseCallback([this, client_fd]() { OnClose(client_fd); });
        conn.channel->SetErrorCallback([this, client_fd]() { OnError(client_fd); });

        // 初始只监听 EPOLLIN + EPOLLRDHUP（不监听 EPOLLOUT）
        epoll_->Add(client_fd, EPOLLIN | EPOLLRDHUP);
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
                return;  // 非阻塞正常情况
            }
            std::cerr << "[ERROR] ReadFd fd=" << client_fd << ": " << std::strerror(savedErrno) << std::endl;
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
            std::string msg(conn.inputBuffer.Peek(), conn.inputBuffer.ReadableBytes());
            while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
            {
                msg.pop_back();
            }
            std::cout << "[recv " << n << "B fd=" << client_fd << "] " << msg << std::endl;

            conn.outputBuffer.Append(conn.inputBuffer.Peek(), conn.inputBuffer.ReadableBytes());
            conn.inputBuffer.RetrieveAll();

            // 尝试发送
            FlushWrite(client_fd);
        }
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

        clients_.erase(client_fd);

        std::cout << "    [当前连接数: " << clients_.size() << "]" << std::endl;
    }

    void OnError(int client_fd)
    {
        std::cerr << "[!] 客户端异常 (fd=" << client_fd << ")" << std::endl;
        OnClose(client_fd);
    }

    // 成员变量
    Socket listen_sock_;
    std::unique_ptr<Epoll> epoll_;
    std::unique_ptr<Channel> listen_channel_;

    std::unordered_map<int, Connection> clients_;
};



int main()
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
