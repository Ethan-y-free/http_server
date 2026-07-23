/**
 * epoll.h —— Epoll RAII 封装类（骨架版）
 *
 * 把 epoll 三大 API 收进一个类：
 *   epoll_create1  → 构造函数
 *   epoll_ctl      → Add / Mod / Del
 *   epoll_wait     → Wait()
 *   close(epfd)    → 析构函数
 *
 * 你需要实现的部分用中文提示标出。
 * 完整参考版：reference/epoll_full.h（写不出来时再看）
 *
 * 风格：Allman 大括号，4 空格缩进
 */

#pragma once

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

    // TODO: Epoll()
    //   调用 epoll_create1(0)，失败抛异常
    Epoll()
    {
        epfd_ = epoll_create1(0);
        if (epfd_ < 0)
        {
            throw std::runtime_error(std::string("epoll_create1() failed: ") + std::strerror(errno));
        }
    }

    // TODO: ~Epoll() noexcept
    //   如果 epfd_ >= 0，close(epfd_)
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

    // TODO: 移动构造 —— 接管 other.epfd_，other.epfd_ 置 -1
    Epoll(Epoll&& other) noexcept
    {
        epfd_ = other.epfd_;
        other.epfd_ = -1;
    }

    // TODO: 移动赋值 —— 先释放自身 fd，再接管 other，other 置 -1
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

    // TODO: 禁止拷贝
    Epoll (const Epoll&) = delete;
    Epoll& operator= (const Epoll&) = delete;

    // ==========================================================
    // ③ 属性
    // ==========================================================

    // TODO: int Fd() const noexcept → 返回 epfd_
    int Fd() const noexcept
    {
        return epfd_;
    }

    // ==========================================================
    // ④ 三个核心操作
    // ==========================================================

    // TODO: Add(fd, events)
    //   struct epoll_event ev → 填 events 和 data.fd → epoll_ctl(epfd_, EPOLL_CTL_ADD, ...)
    //   失败抛异常
    void Add(int fd, uint32_t events)
    {
        epoll_event ev;
        ev.events = events;
        ev.data.fd = fd;

        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0)
        {
            throw std::runtime_error(std::string("epoll_ctl(ADD) fd=") + std::to_string(fd) + " failed: " + std::strerror(errno));
        }
    }

    // TODO: Mod(fd, events)
    //   同上，但用 EPOLL_CTL_MOD
    void Mod(int fd, uint32_t events)
    {
        epoll_event ev;
        ev.events = events;
        ev.data.fd = fd;

        if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) < 0)
        {
            // ENOENT: fd 已被内核移出 epoll（如 fd 复用后被 close 了）
            // 仅打印警告，不抛异常
            if (errno != ENOENT)
            {
                throw std::runtime_error(std::string("epoll_ctl(MOD) fd=") + std::to_string(fd) + " failed: " + std::strerror(errno));
            }
        }
    }

    // TODO: Del(fd)
    //   epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr)
    void Del(int fd)
    {
        if (epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) < 0)
        {
            // ENOENT: fd 已不在 epoll 中（重复 DEL 或已被内核移除）
            // EBADF:  fd 已被关闭（Deferred deletion 期间 Socket 析构先于 Channel）
            if (errno != ENOENT && errno != EBADF)
            {
                throw std::runtime_error(std::string("epoll_ctl(DEL) fd=") + std::to_string(fd) + " failed: " + std::strerror(errno));
            }
        }
    }

    // ==========================================================
    // ⑤ 等待事件
    // ==========================================================

    int Wait(epoll_event* events, int max_events, int timeout_ms = -1)
    {
        int nfds = epoll_wait(epfd_, events, max_events, timeout_ms);

        if (nfds < 0)
        {
            if (errno == EINTR)
            {
                return 0;
            }
            throw std::runtime_error(std::string("epoll_wait() failed: ") + std::strerror(errno));
        }

        return nfds;
    }

private:
    int epfd_ = -1;
};
