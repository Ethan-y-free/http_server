/**
 * epoll.h —— Epoll RAII 封装类（完整参考版）
 *
 * 把三大 API 收进一个类：
 *   epoll_create1  → 构造函数
 *   epoll_ctl      → Add / Mod / Del
 *   epoll_wait     → Wait()
 *   close(epfd)    → 析构函数
 *
 * 设计：可移动不可拷贝（epoll fd 只能有一个 owner）
 *
 * 风格：Allman 大括号，4 空格缩进
 */

#pragma once

#include <vector>
#include <stdexcept>
#include <cstring>
#include <cerrno>

#include <sys/epoll.h>
#include <unistd.h>


class Epoll
{
public:
    // ==========================================================
    // ① 构造 / 析构
    // ==========================================================

    // 创建 epoll 实例，失败抛异常
    Epoll()
    {
        epfd_ = epoll_create1(0);
        if (epfd_ < 0)
        {
            throw std::runtime_error(
                std::string("epoll_create1() failed: ") + std::strerror(errno));
        }
    }

    ~Epoll() noexcept
    {
        if (epfd_ >= 0)
        {
            close(epfd_);
        }
    }

    // ==========================================================
    // ② 移动 / 拷贝
    // ==========================================================

    Epoll(Epoll&& other) noexcept : epfd_(other.epfd_)
    {
        other.epfd_ = -1;
    }

    Epoll& operator= (Epoll&& other) noexcept
    {
        if (this != &other)
        {
            if (epfd_ >= 0)
            {
                close(epfd_);
            }
            epfd_ = other.epfd_;
            other.epfd_ = -1;
        }
        return *this;
    }

    Epoll (const Epoll&) = delete;
    Epoll& operator= (const Epoll&) = delete;

    // ==========================================================
    // ③ 属性
    // ==========================================================

    int Fd () const noexcept { return epfd_; }

    // ==========================================================
    // ④ 三个核心操作
    // ==========================================================

    // 注册 fd 到 epoll，监听指定事件
    void Add (int fd, uint32_t events)
    {
        struct epoll_event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.events = events;
        ev.data.fd = fd;

        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0)
        {
            throw std::runtime_error(
                std::string("epoll_ctl(ADD) fd=") + std::to_string(fd)
                + " failed: " + std::strerror(errno));
        }
    }

    // 修改已注册 fd 的监听事件
    void Mod (int fd, uint32_t events)
    {
        struct epoll_event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.events = events;
        ev.data.fd = fd;

        if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) < 0)
        {
            throw std::runtime_error(
                std::string("epoll_ctl(MOD) fd=") + std::to_string(fd)
                + " failed: " + std::strerror(errno));
        }
    }

    // 从 epoll 删除 fd
    void Del (int fd)
    {
        // EPOLL_CTL_DEL 的 event 参数在 Linux 2.6.9+ 可为 nullptr
        if (epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) < 0)
        {
            throw std::runtime_error(
                std::string("epoll_ctl(DEL) fd=") + std::to_string(fd)
                + " failed: " + std::strerror(errno));
        }
    }

    // ==========================================================
    // ⑤ 等待事件
    // ==========================================================
    //
    // events: 调用者开的数组，内核往里填就绪事件
    // max_events: 数组大小
    // timeout_ms: -1=永远等, 0=立刻返回, >0=等N毫秒
    // 返回: 就绪数量 / 0=超时或信号打断
    //
    // ==========================================================
    int Wait (epoll_event* events, int max_events, int timeout_ms = -1)
    {
        int nfds = epoll_wait(epfd_, events, max_events, timeout_ms);

        if (nfds < 0)
        {
            if (errno == EINTR)
            {
                return 0;
            }
            throw std::runtime_error(
                std::string("epoll_wait() failed: ") + std::strerror(errno));
        }

        return nfds;
    }

private:
    int epfd_ = -1;
};
