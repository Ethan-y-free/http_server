#pragma once    

#include "tcp_connection.h"
#include "timer_wheel.h"
#include "async_logger/logger.h"

#include <functional>
#include <sys/timerfd.h>       
#include <unistd.h>            
#include <memory>              
#include <thread>              
#include <unordered_map>       
#include <cstring>

class SubReactor
{
public:
    SubReactor(int idleTimeoutMs, AsyncLogWriter* logWriter)
        : loop_(std::make_unique<EventLoop>())
        , timerWheel_(std::make_unique<TimerWheel>(60, 1000))
        , logBuffer_(std::make_unique<LogBuffer>(logWriter))
        , idleTimeoutMs_(idleTimeoutMs)
        , logWriter_(logWriter) {};

    ~SubReactor()
    {
        Stop();
        if (timerFd_ >= 0)
        {
            ::close(timerFd_);
            timerFd_ = -1;
        }
    }

    void Start()
    {
        thread_ = std::thread([this]()
            {
                Logger::SetCurrentLogBuffer(logBuffer_.get());
                loop_->Loop();
            });
        loop_->RunInLoop([this]() { SetupTimer(); });
    }

    void Stop()
    {
        loop_->Quit();
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    void AddConnection(int fd, TcpConnection::MessageCallback onMessage)
    {
        loop_->RunInLoop([this, fd, onMessage = std::move(onMessage)]
            {
                auto conn = std::make_unique<TcpConnection>(fd, loop_.get());

                conn->SetMessageCallback(onMessage);
                conn->SetCloseCallback([this](TcpConnection* c)
                    {
                        timerWheel_->Remove(c->Fd());
                        int fd = c->Fd();
                        // 延迟删除：避免在 HandleEvent 调用栈内析构 Channel（use-after-free）
                        loop_->QueueInLoop([this, fd]() 
                        {
                            connections_.erase(fd);
                        });
                    });

                timerWheel_->AddOrRefresh(fd, idleTimeoutMs_);
                connections_[fd] = std::move(conn);
                connections_[fd]->ConnectEstablished();
            });
    }

    void RunInLoop(std::function<void()> task)
    {
        loop_->RunInLoop(std::move(task));
    }

    void SetupTimer()
    {
        loop_->AssertInLoopThread();

      // ① 创建 timerfd：非阻塞，fork 时自动关闭
      timerFd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
      if (timerFd_ < 0)
      {
          LOG_ERROR << "timerfd_create() failed: " << std::strerror(errno);
          return;
      }

      // ② 设定时：1 秒后首次触发，之后每 1 秒一次
      struct itimerspec its;
      its.it_value.tv_sec     = 1;   // 首次触发：1 秒后
      its.it_value.tv_nsec    = 0;
      its.it_interval.tv_sec  = 1;   // 间隔：1 秒
      its.it_interval.tv_nsec = 0;

      if (::timerfd_settime(timerFd_, 0, &its, nullptr) < 0)
      {
          LOG_ERROR << "timerfd_settime() failed: " << std::strerror(errno);
          ::close(timerFd_);
          timerFd_ = -1;
          return;
      }

      // ③ 注册到 epoll：读事件触发时调 OnTimerTick
      timerChannel_ = std::make_unique<Channel>(timerFd_, loop_->EpollPtr());
      timerChannel_->SetReadCallback([this]() { OnTimerTick(); });
      loop_->AddChannel(timerChannel_.get());
      timerChannel_->EnableRead();
    }

    EventLoop* Loop() const
    {
        return loop_.get();
    }

private:
    void OnTimerTick()
    {
        loop_->AssertInLoopThread();

        uint64_t expirations = 0;
        ::read(timerFd_, &expirations, sizeof(expirations));

        std::vector<int> expired = timerWheel_->Tick();
        for (int fd : expired)
        {
            auto it = connections_.find(fd);
            if (it != connections_.end())
            {
                it->second->ForceClose();
            }
        }
        logBuffer_->Flush();
    }

    std::unique_ptr<EventLoop> loop_;
    std::unique_ptr<TimerWheel> timerWheel_;
    std::unique_ptr<LogBuffer>  logBuffer_;
    std::unique_ptr<Channel>    timerChannel_;
    int timerFd_ = -1;

    std::unordered_map<int, std::unique_ptr<TcpConnection>> connections_;
    std::thread thread_;

    int idleTimeoutMs_;
    AsyncLogWriter* logWriter_;
};