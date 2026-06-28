// ============================================================
// reactor_eventloop_ref.h — EventLoop 参考骨架（Day 9 用）
// ============================================================
// 使用方式：
//   1. 先看 DESIGN.md §九 理解 Reactor 模式
//   2. 根据 §9.6 API 清单，自己写 eventloop.h
//   3. 遇到困惑时对照本文件，但先自己写
//
// 核心概念回顾：
//   - EventLoop = 一个线程 + 一个 Epoll + 一组 Channel
//   - one loop per thread：每个 EventLoop 绑定一个线程
//   - eventfd：跨线程唤醒正在 epoll_wait 的 EventLoop
//   - pending functors：其他线程通过 RunInLoop 塞进来的回调
// ============================================================

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>

#include "../epoll.h"
#include "../channel.h"

class EventLoop
{
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    // 禁止拷贝/移动（每个对象绑定固定线程）
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // ===== 核心 API =====

    // 启动事件循环。阻塞当前线程，直到 Quit() 被调用。
    // 必须在所属线程调用。
    void Loop();

    // 退出事件循环。线程安全，可在任意线程调用。
    void Quit();

    // 判断当前线程是否就是 EventLoop 所属线程
    bool IsInLoopThread() const;

    // 断言当前在 EventLoop 所属线程（DEBUG 用）
    void AssertInLoopThread();

    // ===== 跨线程调用 =====

    // 在 EventLoop 线程执行 cb。
    // - 如果调用者就在 EventLoop 线程 → 直接执行
    // - 如果调用者在其他线程 → 放入队列，唤醒 EventLoop
    void RunInLoop(Functor cb);

    // 放入队列，唤醒 EventLoop（即便已经在 EventLoop 线程也入队）
    void QueueInLoop(Functor cb);

    // ===== Channel 管理（由 Channel 调用） =====

    // Channel 调用 EnableRead/EnableWrite 时通知 EventLoop
    void UpdateChannel(Channel* ch);

    // Channel 析构时通知 EventLoop 移除自己
    void RemoveChannel(Channel* ch);

private:
    void DoPendingFunctors();          // 执行 pending_functors_ 中的回调
    void HandleWakeup();               // eventfd 可读时的回调
    int  CreateWakeupFd();             // 创建 eventfd

    using ChannelList = std::vector<Channel*>;

    // 绑定的线程 ID
    const std::thread::id tid_;

    // EventLoop 拥有 Epoll（一对一关系）
    std::unique_ptr<Epoll> epoll_;

    // 状态标志
    bool        looping_;       // 是否正在 Loop() 中
    std::atomic<bool> quit_;    // 是否已请求退出

    // epoll_wait 返回的就绪 Channel（每轮清空）
    ChannelList active_channels_;

    // 跨线程回调队列
    std::mutex           mutex_;
    std::vector<Functor> pending_functors_;  // guarded by mutex_

    // 唤醒机制
    int                            wakeup_fd_;
    std::unique_ptr<Channel>       wakeup_channel_;
};
