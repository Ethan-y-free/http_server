/**
 * channel.h —— Channel 事件分发器（完整参考版）
 *
 * Channel 负责：一个 fd + 它关心的事件 + 事件到了做什么
 * 把"fd → epoll 事件 → 回调"绑在一起
 *
 * 风格：Allman 大括号，4 空格缩进
 */

#pragma once

#include <functional>
#include <cassert>

#include "epoll.h"


class Channel
{
public:
    // 回调类型：void()
    using EventCallback = std::function<void()>;

    // ==========================================================
    // ① 构造 / 析构
    // ==========================================================

    Channel(int fd, Epoll* epoll)
        : fd_(fd), epoll_(epoll), events_(0)
    {
    }

    ~Channel()
    {
        // 确保 fd 从 epoll 移除
        if (fd_ >= 0 && epoll_)
        {
            try { epoll_->Del(fd_); }
            catch (...) {}  // 析构不抛异常
        }
    }

    // 禁止拷贝
    Channel (const Channel&) = delete;
    Channel& operator= (const Channel&) = delete;

    // 禁止移动（可加，这里先简化）
    // 实践中可移动主要是为了容器，可加入

    // ==========================================================
    // ② 属性
    // ==========================================================

    int  Fd ()      const noexcept { return fd_; }
    int  Events ()  const noexcept { return events_; }

    // ==========================================================
    // ③ 设置回调
    // ==========================================================

    void SetReadCallback  (EventCallback cb) { readCallback_  = std::move(cb); }
    void SetWriteCallback (EventCallback cb) { writeCallback_ = std::move(cb); }
    void SetCloseCallback (EventCallback cb) { closeCallback_ = std::move(cb); }
    void SetErrorCallback (EventCallback cb) { errorCallback_ = std::move(cb); }

    // ==========================================================
    // ④ 启用/禁用事件
    // ==========================================================

    void EnableRead()
    {
        events_ |= EPOLLIN;
        Update();
    }

    void EnableWrite()
    {
        events_ |= EPOLLOUT;
        Update();
    }

    void DisableWrite()
    {
        events_ &= ~EPOLLOUT;
        Update();
    }

    void DisableAll()
    {
        events_ = 0;
        Update();
    }

    // ==========================================================
    // ⑤ 事件分发
    // ==========================================================

    void HandleEvent(uint32_t revents)
    {
        // 优先级：错误 → 关闭 → 读 → 写
        // EPOLLERR 和 EPOLLHUP 是 epoll 自动加的，不用手动注册

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

        if (revents & (EPOLLRDHUP | EPOLLHUP))
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

    void Update()
    {
        // events_ == 0 表示不监听了，从 epoll 移除
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

    int      fd_      = -1;
    Epoll*   epoll_   = nullptr;
    uint32_t events_  = 0;

    EventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};
