/**
 * channel.h —— Channel 事件分发器
 *
 * Channel 负责：一个 fd + 它关心的事件 + 事件到了做什么
 * 把"fd → epoll 事件 → 回调"绑在一起
 *
 * 你需要实现的部分用中文提示标出。
 * 完整参考版：reference/channel_full.h（写不出来时再看）
 *
 * 风格：Allman 大括号，4 空格缩进
 */

#pragma once

#include <functional>
#include <sys/epoll.h>
#include "epoll.h"

// 前置声明，避免循环依赖
class Epoll;


class Channel
{
public:
    // 回调类型：void()
    using EventCallback = std::function<void()>;

    // ==========================================================
    // ① 构造 / 析构
    // ==========================================================

    // 构造函数：绑定 fd 和 Epoll 实例
    // TODO: Channel(int fd, Epoll* epoll)
    //   fd_ = fd, epoll_ = epoll, events_ = 0
    Channel(int fd, Epoll* epoll) : fd_(fd), epoll_(epoll)
    {
        events_ = 0;
    }

    // 析构：从 epoll 移除自己
    // TODO: ~Channel()
    //   如果 fd_ >= 0，手动从 epoll 摘掉
    ~Channel()
    {
        if (fd_ >= 0 && epoll_ && events_ != 0)
        {
            DisableAll();  // events_ != 0 说明还没摘除，先摘再析构
        }
    }

    // 禁止拷贝，允许移动
    // TODO: 禁止拷贝，允许移动
    Channel (const Channel&) = delete;
    Channel& operator= (const Channel&) = delete;

    // ==========================================================
    // ② 属性
    // ==========================================================

    int  Fd() const noexcept
    {
        return fd_;
    }

    int  Events() const noexcept
    {
        return events_;
    }

    // ==========================================================
    // ③ 设置回调（触发某个条件，你就帮我调用对应的函数）
    // ==========================================================
    // 提示：四个 setter，各存一个 std::function

    // TODO: void SetReadCallback(EventCallback cb)
    void SetReadCallback(EventCallback cb)
    {
        readCallback_ = std::move(cb);
    }

    // TODO: void SetWriteCallback(EventCallback cb)
    void SetWriteCallback(EventCallback cb)
    {
        writeCallback_ = std::move(cb);
    }

    // TODO: void SetCloseCallback(EventCallback cb)
    void SetCloseCallback(EventCallback cb)
    {
        closeCallback_ = std::move(cb);
    }

    // TODO: void SetErrorCallback(EventCallback cb)
    void SetErrorCallback(EventCallback cb)
    {
        errorCallback_ = std::move(cb);
    }

    // ==========================================================
    // ④ 启用/禁用事件
    // ==========================================================
    // 关键：每次改 events_ 后调用 Update() 通知 epoll

    // TODO: EnableRead()  —— events_ 加上 EPOLLIN，调 Update()
    void EnableRead()
    {
        events_ |= EPOLLIN | EPOLLRDHUP;
        Update();
    }

    // TODO: EnableWrite() —— events_ 加上 EPOLLOUT，调 Update()
    void EnableWrite()
    {
        events_ |= EPOLLOUT;
        Update();
    }

    // TODO: DisableWrite()—— events_ 去掉 EPOLLOUT，调 Update()
    void DisableWrite()
    {
        events_ &= ~EPOLLOUT;
        Update();
    }

    // TODO: DisableAll()  —— events_ = 0，调 Update()
    void DisableAll()
    {
        events_ = 0;
        Update();
    }

    // ==========================================================
    // ⑤ 事件分发
    // ==========================================================
    // 由 epoll 事件循环调用，根据 revents 执行对应回调

    // TODO: HandleEvent(uint32_t revents)
    //   if (revents & (EPOLLERR|EPOLLHUP)) → errorCallback_
    //   if (revents & (EPOLLRDHUP))        → closeCallback_
    //   if (revents & EPOLLIN)             → readCallback_
    //   if (revents & EPOLLOUT)            → writeCallback_
    //   提示：先处理错误和关闭，再处理读写
    void HandleEvent(uint32_t revents)
    {
        if ((revents & EPOLLHUP) && !(revents & EPOLLIN))
        {
            // 对端关闭：如果还有数据可读，先读完再 close
            if (closeCallback_)
            {
                closeCallback_();
            }
            return;
        }

        if (revents & (EPOLLERR | EPOLLHUP))
        {
            if (errorCallback_)
            {
                errorCallback_();
            }
            return;
        }

        if (revents & EPOLLRDHUP)
        {
            if (closeCallback_)
            {
                closeCallback_();
            }
            return;
        }

        if (revents & EPOLLIN)
        {
            if (readCallback_)
            {
                readCallback_();
            }
        }

        if (revents & EPOLLOUT)
        {
            if (writeCallback_)
            {
                writeCallback_();
            }
        }
    }

private:
    // ==========================================================
    // ⑥ 内部方法
    // ==========================================================

    // TODO: Update()
    //   把 fd_ 当前的 events_ 同步到 epoll_
    //   如果 events_ == 0 → epoll_->Del(fd_)
    //   否则 → epoll_->Mod(fd_)
    void Update()
    {
        if (fd_ == -1) return;

        if (events_ == 0)
        {
            epoll_->Del(fd_);     
        }
        else
        {
            epoll_->Mod(fd_, events_); 
        }
    }

    // ==========================================================
    // 成员变量
    // ==========================================================

    int      fd_      = -1;   // 文件描述符
    Epoll*   epoll_   = nullptr;  // 归属的 Epoll（不拥有）
    uint32_t events_  = 0;    // 当前监听的事件（位掩码）

    EventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};
