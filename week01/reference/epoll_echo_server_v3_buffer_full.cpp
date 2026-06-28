/**
 * epoll_echo_server_v3_buffer_full.cpp —— Buffer 集成版 Echo Server（完整参考版）
 *
 * v2 → v3 核心变化：
 *   1. Connection 结构体：Socket + Channel + inputBuffer + outputBuffer 打包
 *   2. 用 Buffer::ReadFd() 替代原始 recv()（readv，少一次拷贝）
 *   3. 非阻塞写：FlushWrite + OnWrite，EAGAIN 不丢数据
 *
 * 编译：g++ -std=c++17 epoll_echo_server_v3_buffer_full.cpp -o epoll_echo_server_v3
 * 运行：./epoll_echo_server_v3
 * 测试：nc localhost 8888
 *
 * 设计文档：../DESIGN.md §八
 */

#include <iostream>
#include <string>
#include <cstring>
#include <unordered_map>
#include <memory>

#include "../socket_raii.h"
#include "../epoll.h"
#include "../channel.h"
#include "../buffer.h"


constexpr uint16_t PORT    = 8888;
constexpr int      BACKLOG = 128;


// ==========================================================
// EchoServer —— Buffer 集成版
// ==========================================================
class EchoServer
{
public:
    // ---- 构造 ----
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

    // ---- 启动 ----
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
    // ==========================================================
    // Connection 结构体
    // ==========================================================
    // 把每个连接的所有资源打包，一个 map 替代 v2 的两个 map。
    //
    // ★ 析构顺序（逆声明序）：
    //    outputBuffer → inputBuffer → channel（~Channel → epoll Del）
    //    → sock（~Socket → close fd）
    //    天然满足"先摘 epoll，再关 fd"的要求。
    //
    struct Connection
    {
        Socket sock;
        std::unique_ptr<Channel> channel;
        Buffer inputBuffer;
        Buffer outputBuffer;
    };

    // ==========================================================
    // OnAccept —— 新连接
    // ==========================================================
    void OnAccept()
    {
        std::string ip;
        int port = 0;

        Socket client_sock = listen_sock_.Accept(ip, port);
        client_sock.SetNonBlocking();

        int client_fd = client_sock.Fd();

        std::cout << "[+] 新客户端: " << ip << ":" << port
                  << " (fd=" << client_fd << ")" << std::endl;

        // 构造 Connection（两个 Buffer 默认空）
        Connection conn;
        conn.sock = std::move(client_sock);
        conn.channel = std::make_unique<Channel>(client_fd, epoll_.get());

        // 设置四个回调
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

    // ==========================================================
    // OnRead —— 用 Buffer::ReadFd 读数据
    // ==========================================================
    void OnRead(int client_fd)
    {
        auto it = clients_.find(client_fd);
        if (it == clients_.end())
        {
            return;
        }

        Connection& conn = it->second;
        int savedErrno = 0;

        // ★ 核心变化：用 Buffer::ReadFd 替代 recv()
        //    ReadFd 内部用 readv：优先填 buffer 可写区，溢出才用栈上 extrabuf
        ssize_t n = conn.inputBuffer.ReadFd(client_fd, &savedErrno);

        if (n > 0)
        {
            // 成功读到数据
            std::cout << "[recv " << n << "B fd=" << client_fd << "] ";

            // 打印收到的内容（去掉末尾换行便于阅读）
            std::string msg(conn.inputBuffer.Peek(), conn.inputBuffer.ReadableBytes());
            while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
            {
                msg.pop_back();
            }
            std::cout << msg << std::endl;

            // Echo：把收到的数据拷到 outputBuffer
            conn.outputBuffer.Append(conn.inputBuffer.Peek(), conn.inputBuffer.ReadableBytes());
            conn.inputBuffer.RetrieveAll();

            // 尝试发送
            FlushWrite(client_fd);
        }
        else if (n == 0)
        {
            // 对端正常关闭（readv 返回 0 = EOF）
            OnClose(client_fd);
        }
        else  // n < 0
        {
            if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK)
            {
                return;  // 非阻塞正常：没数据可读
            }
            // 真错
            std::cerr << "[ERROR] ReadFd fd=" << client_fd << ": "
                      << std::strerror(savedErrno) << std::endl;
            OnClose(client_fd);
        }
    }

    // ==========================================================
    // OnWrite —— 内核通知 socket 可写，继续发 outputBuffer
    // ==========================================================
    void OnWrite(int client_fd)
    {
        FlushWrite(client_fd);
    }

    // ==========================================================
    // FlushWrite —— 非阻塞写的核心
    // ==========================================================
    // 循环发送 outputBuffer 里的数据，直到：
    //   - 全发完 → DisableWrite（关掉 EPOLLOUT）
    //   - EAGAIN → EnableWrite 已在 epoll 注册中，直接 return
    //   - 出错   → OnClose
    //
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
            ssize_t sent = conn.sock.Send(
                conn.outputBuffer.Peek(),
                conn.outputBuffer.ReadableBytes()
            );

            if (sent > 0)
            {
                // 发出去一部分，消费掉
                conn.outputBuffer.Retrieve(sent);
            }
            else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                // TCP 发送缓冲满了，注册 EPOLLOUT，等下次可写再发
                // 数据还在 outputBuffer 里，不会丢
                conn.channel->EnableWrite();
                return;
            }
            else
            {
                // 真错或对端关闭
                if (sent < 0)
                {
                    std::cerr << "[ERROR] send() fd=" << client_fd << ": "
                              << std::strerror(errno) << std::endl;
                }
                OnClose(client_fd);
                return;
            }
        }

        // while 正常结束 = outputBuffer 全发完了
        conn.channel->DisableWrite();
    }

    // ==========================================================
    // OnClose —— 关闭连接
    // ==========================================================
    void OnClose(int client_fd)
    {
        std::cout << "[-] 客户端断开 (fd=" << client_fd << ")" << std::endl;

        // erase → Connection 析构：
        //   outputBuffer → inputBuffer → ~Channel（epoll Del）→ ~Socket（close fd）
        clients_.erase(client_fd);

        std::cout << "    [当前连接数: " << clients_.size() << "]" << std::endl;
    }

    // ==========================================================
    // OnError —— 异常处理
    // ==========================================================
    void OnError(int client_fd)
    {
        std::cerr << "[!] 客户端异常 (fd=" << client_fd << ")" << std::endl;
        OnClose(client_fd);
    }

    // ==========================================================
    // 成员变量
    // ==========================================================
    Socket listen_sock_;
    std::unique_ptr<Epoll> epoll_;
    std::unique_ptr<Channel> listen_channel_;

    // v3：一个 map 管所有（v2 是两个 map）
    std::unordered_map<int, Connection> clients_;
};


// ==========================================================
// main
// ==========================================================
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
