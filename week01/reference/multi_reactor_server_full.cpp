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
#include <mutex>
#include <atomic>
#include <vector>

// ============================================================================
// Day 12：主从 Reactor（MainReactor + SubReactor）— 参考实现
//
// 架构：
//   MainReactor（主线程 EventLoop）—— 只做 accept
//       │
//       │ Round-Robin 分发新连接
//       ▼
//   SubReactor[0..N-1]（各自线程 EventLoop）—— 做客户端 IO（read/write/close）
//       │
//       │ 计算任务提交到 ThreadPool
//       ▼
//   ThreadPool —— 处理业务（echo），然后 RunInLoop 回到 SubReactor 写回
//
// 线程安全设计：
//   clients_ 被 MainReactor（OnAccept 插入）和 SubReactor（OnClose 删除）
//   同时访问，因此所有读写都必须持 clientsMutex_。
//   同一个 fd 的 OnRead→OnClose 在同一 SubReactor 线程串行，不存在
//   read/erase 竞争，但 read/emplace 跨线程仍需锁保护。
//
// 对比 Day 11 单 Reactor：
//   - accept 和 client IO 不再共用同一个 EventLoop
//   - MainReactor 只做 accept，压力小
//   - 多个 SubReactor 并行处理 IO，吞吐更高
// ============================================================================

// ---------------------------------------------------------------------------
// Connection：每个客户端连接的状态
// ---------------------------------------------------------------------------
struct Connection
{
    Socket sock;
    std::unique_ptr<Channel> channel;
    Buffer inputBuffer;
    Buffer outputBuffer;
    EventLoop* ownerLoop;  // 归属哪个 SubReactor
};

// ---------------------------------------------------------------------------
// MultiReactorServer：主从 Reactor Echo Server
// ---------------------------------------------------------------------------
class MultiReactorServer
{
public:
    MultiReactorServer(EventLoop* mainLoop, uint16_t port, int subReactorCount, ThreadPool* pool)
        : mainLoop_(mainLoop)
        , pool_(pool)
        , subLoops_(subReactorCount)
    {
        // ---- 启动所有 SubReactor 线程 ----
        // 每个 SubReactor 在自己的线程里跑 EventLoop::Loop()
        for (int i = 0; i < subReactorCount; ++i)
        {
            subLoops_[i] = std::make_unique<EventLoop>();

            subThreads_.emplace_back([this, i]()
                {
                    std::cout << "  [SubReactor #" << i << "] 线程启动" << std::endl;
                    subLoops_[i]->Loop();
                    std::cout << "  [SubReactor #" << i << "] 线程退出" << std::endl;
                });
        }

        // ---- 配置监听 socket ----
        listenSock_.SetReuseAddr();
        listenSock_.Bind(port);
        listenSock_.Listen(128);
        listenSock_.SetNonBlocking();

        std::cout << "\n========================================" << std::endl;
        std::cout << "  主从 Reactor Echo Server" << std::endl;
        std::cout << "  MainReactor x1 + SubReactor x" << subReactorCount << std::endl;
        std::cout << "  ThreadPool 线程 " << subReactorCount << " 个" << std::endl;
        std::cout << "  测试: nc localhost " << port << std::endl;
        std::cout << "========================================\n" << std::endl;

        // ---- MainReactor 注册监听 fd ----
        listenChannel_ = std::make_unique<Channel>(listenSock_.Fd(), mainLoop_->EpollPtr());
        listenChannel_->SetReadCallback([this]() { OnAccept(); });
        mainLoop_->AddChannel(listenChannel_.get());
        listenChannel_->EnableRead();
    }

    ~MultiReactorServer()
    {
        // ① 先停 MainReactor 的监听（不再 accept 新连接）
        if (listenChannel_)
        {
            mainLoop_->RemoveChannel(listenChannel_.get());
        }

        // ② 停所有 SubReactor 线程——必须先停线程再清数据！
        for (auto& subLoop : subLoops_)
        {
            subLoop->Quit();
        }
        for (auto& t : subThreads_)
        {
            if (t.joinable())
            {
                t.join();
            }
        }

        // ③ SubReactor 线程全部退出后，安全清理 clients_
        //    此时不会有任何线程访问 clients_，无需加锁
        for (auto& [fd, conn] : clients_)
        {
            // Channel 析构时自动从 epoll 移除，fd 由 Socket 析构关闭
            (void)fd;
        }
        clients_.clear();
    }

    MultiReactorServer(const MultiReactorServer&) = delete;
    MultiReactorServer& operator=(const MultiReactorServer&) = delete;

private:
    // ========================================================================
    // MainReactor 回调：只做 accept
    // ========================================================================
    void OnAccept()
    {
        std::string ip;
        int port = 0;
        Socket clientSock = listenSock_.Accept(ip, port);

        if (clientSock.Fd() < 0)
        {
            return;
        }

        clientSock.SetNonBlocking();
        int clientFd = clientSock.Fd();

        // Round-Robin 选一个 SubReactor
        int idx = nextSubReactor_.fetch_add(1) % static_cast<int>(subLoops_.size());
        EventLoop* subLoop = subLoops_[idx].get();

        std::cout << "[+] 新连接 fd=" << clientFd << " 来自 " << ip << ":" << port
                  << " -> SubReactor #" << idx << std::endl;

        // 创建 Connection（Channel 指向 SubReactor 的 Epoll）
        Connection conn;
        conn.sock = std::move(clientSock);
        conn.channel = std::make_unique<Channel>(clientFd, subLoop->EpollPtr());
        conn.ownerLoop = subLoop;

        // 注册回调（捕获 subLoop 用于 RunInLoop）
        conn.channel->SetReadCallback([this, fd = clientFd, sub = subLoop]()
            {
                OnRead(fd, sub);
            });
        conn.channel->SetWriteCallback([this, fd = clientFd, sub = subLoop]()
            {
                OnWrite(fd, sub);
            });
        conn.channel->SetCloseCallback([this, fd = clientFd, sub = subLoop]()
            {
                OnClose(fd, sub);
            });
        conn.channel->SetErrorCallback([this, fd = clientFd, sub = subLoop]()
            {
                OnError(fd, sub);
            });

        // 插入 clients_（加锁：主线程写，SubReactor 线程可能 OnClose 删）
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clients_.emplace(clientFd, std::move(conn));
        }

        // 在 SubReactor 的 IO 线程中注册 Channel + 启用读
        subLoop->RunInLoop([this, clientFd]()
            {
                std::lock_guard<std::mutex> lock(clientsMutex_);
                auto it = clients_.find(clientFd);
                if (it != clients_.end())
                {
                    it->second.ownerLoop->AddChannel(it->second.channel.get());
                    it->second.channel->EnableRead();
                }
            });

        std::cout << "    [当前连接数: " << clients_.size() << "]" << std::endl;
    }

    // ========================================================================
    // SubReactor 回调：IO 处理（在 SubReactor 线程中执行）
    //
    // 注意：OnRead 持锁时不能直接调 OnClose，否则死锁（OnClose 里也加锁）。
    // 所以用 shouldClose 标志——先解锁，再调 OnClose。
    // ========================================================================

    void OnRead(int fd, EventLoop* subLoop)
    {
        bool shouldClose = false;
        size_t dataLen = 0;

        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
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
                    return;  // 没数据了，等待下次事件
                }
                std::cerr << "[ERROR] ReadFd fd=" << fd << ": " << std::strerror(savedErrno) << std::endl;
                shouldClose = true;
            }
            else if (n == 0)
            {
                shouldClose = true;  // 对端关闭
            }
            else
            {
                // 打印收到的消息（去掉末尾换行）
                std::string msg(conn.inputBuffer.Peek(), conn.inputBuffer.ReadableBytes());
                while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
                {
                    msg.pop_back();
                }
                std::cout << "[recv " << n << "B fd=" << fd << " via SubReactor] " << msg << std::endl;

                // Echo：把读到的数据拷贝到输出 Buffer
                conn.outputBuffer.Append(conn.inputBuffer.Peek(), conn.inputBuffer.ReadableBytes());
                dataLen = conn.inputBuffer.ReadableBytes();
                conn.inputBuffer.RetrieveAll();
            }
        } // 解锁

        if (shouldClose)
        {
            OnClose(fd, subLoop);  // OnClose 自己加锁，不会死锁
            return;
        }

        // 投递到 ThreadPool 做"计算"（这里只是 echo，实际场景可能是解析 HTTP 等）
        pool_->Run([this, fd, subLoop, dataLen]()
            {
                std::cout << "  [Worker " << std::this_thread::get_id() << "] 处理 " << dataLen << " 字节" << std::endl;

                // 计算完成后，回到 SubReactor 的 IO 线程写回数据
                subLoop->RunInLoop([this, fd]()
                    {
                        FlushWrite(fd);
                    });
            });
    }

    void OnWrite(int fd, EventLoop* subLoop)
    {
        (void)subLoop;
        FlushWrite(fd);
    }

    void OnClose(int fd, EventLoop* subLoop)
    {
        (void)subLoop;
        std::cout << "[-] 客户端断开 (fd=" << fd << ")" << std::endl;

        // 从 epoll 移除（加锁读 clients_）
        RemoveClientChannel(fd);

        // 从 clients_ 中删除
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clients_.erase(fd);
        }

        std::cout << "    [当前连接数: " << clients_.size() << "]" << std::endl;
    }

    void OnError(int fd, EventLoop* subLoop)
    {
        std::cerr << "[!] 客户端异常 (fd=" << fd << ")" << std::endl;
        OnClose(fd, subLoop);
    }

    // ========================================================================
    // 写回（SubReactor 线程中执行）
    // ========================================================================
    void FlushWrite(int fd)
    {
        bool shouldClose = false;
        EventLoop* ownerLoop = nullptr;

        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            auto it = clients_.find(fd);
            if (it == clients_.end())
            {
                return;
            }

            Connection& conn = it->second;
            ownerLoop = conn.ownerLoop;

            while (conn.outputBuffer.ReadableBytes() > 0)
            {
                ssize_t sent = conn.sock.Send(conn.outputBuffer.Peek(),
                    conn.outputBuffer.ReadableBytes());

                if (sent > 0)
                {
                    conn.outputBuffer.Retrieve(sent);
                }
                else if (sent < 0 && errno == EAGAIN)
                {
                    // 内核发送缓冲区满了，等下次可写事件
                    conn.channel->EnableWrite();
                    return;  // 不关连接，等下次 EPOLLOUT
                }
                else
                {
                    // 发送失败，标记关闭
                    shouldClose = true;
                    break;
                }
            }

            // 全部写完，关闭写事件（避免 busy-loop）
            if (!shouldClose)
            {
                conn.channel->DisableWrite();
            }
        } // 解锁

        if (shouldClose)
        {
            OnClose(fd, ownerLoop);
        }
    }

    // ========================================================================
    // 辅助：从归属 EventLoop 的 epoll 中移除 Channel
    // ========================================================================
    void RemoveClientChannel(int fd)
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        auto it = clients_.find(fd);
        if (it != clients_.end() && it->second.channel)
        {
            it->second.ownerLoop->RemoveChannel(it->second.channel.get());
        }
    }

    // ---- 主从 Reactor ----
    EventLoop*                           mainLoop_;     // 不拥有
    std::vector<std::unique_ptr<EventLoop>> subLoops_;
    std::vector<std::thread>             subThreads_;
    ThreadPool*                          pool_;         // 不拥有

    // ---- 监听 ----
    Socket                     listenSock_;
    std::unique_ptr<Channel>   listenChannel_;

    // ---- 客户端管理 ----
    std::unordered_map<int, Connection> clients_;
    std::mutex                 clientsMutex_;           // 保护 clients_ 的读写
    std::atomic<int>           nextSubReactor_{0};      // Round-Robin 计数器
};

// ============================================================================
// main
// ============================================================================
constexpr uint16_t PORT = 8888;
constexpr int      SUB_REACTOR_COUNT = 4;  // SubReactor 数量 = CPU 核心数
constexpr size_t   THREADPOOL_SIZE = 4;    // 计算线程池大小

int main()
{
    try
    {
        EventLoop mainLoop;
        ThreadPool pool(THREADPOOL_SIZE);

        MultiReactorServer server(&mainLoop, PORT, SUB_REACTOR_COUNT, &pool);

        std::cout << "[启动] MainReactor 进入 EventLoop..." << std::endl;
        mainLoop.Loop();

        std::cout << "[退出] MainReactor 正常结束" << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[FATAL] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
