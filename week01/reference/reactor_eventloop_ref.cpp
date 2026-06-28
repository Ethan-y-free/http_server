// ============================================================
// reactor_eventloop_ref.cpp — EventLoop 关键实现参考（Day 9 用）
// ============================================================
// 本文件只展示几个关键方法的实现思路，不提供完整代码。
// 目的：让你理解每个方法"做什么、为什么"，而不是直接抄。
// ============================================================

#include "reactor_eventloop_ref.h"
#include <sys/eventfd.h>
#include <unistd.h>
#include <cassert>
#include <iostream>

// ============================================================
// 构造函数
// ============================================================
EventLoop::EventLoop()
    : tid_(std::this_thread::get_id())
    , epoll_(std::make_unique<Epoll>())
    , looping_(false)
    , quit_(false)
    , wakeup_fd_(CreateWakeupFd())
{
    // wakeup_channel_ 监听 wakeup_fd_ 的可读事件
    // 当其他线程 write(wakeup_fd_) 时，epoll_wait 被唤醒
    wakeup_channel_ = std::make_unique<Channel>(wakeup_fd_, epoll_.get());
    wakeup_channel_->SetReadCallback([this]() { HandleWakeup(); });
    wakeup_channel_->EnableRead();  // 注册 EPOLLIN
}

// ============================================================
// 创建 eventfd
// ============================================================
// eventfd 是一个轻量级的"事件通知"fd。
// write(eventfd, &one, 8) → epoll_wait 立即返回（wakeup_fd_ 变为可读）
// read(eventfd, &one, 8)  → 消费掉这个事件，恢复不可读状态
int EventLoop::CreateWakeupFd()
{
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0)
    {
        throw std::runtime_error("eventfd() failed");
    }
    return fd;
}

// ============================================================
// 核心事件循环
// ============================================================
void EventLoop::Loop()
{
    AssertInLoopThread();    // ① 必须在自己的线程
    looping_ = true;
    quit_ = false;

    constexpr int MAX_EVENTS = 64;
    epoll_event buf[MAX_EVENTS];

    while (!quit_)
    {
        active_channels_.clear();

        // ② 等待事件（默认一直等，直到有事或被唤醒）
        int nfds = epoll_->Wait(buf, MAX_EVENTS, -1);

        // ③ 遍历就绪 fd，分发给 Channel
        for (int i = 0; i < nfds; i++)
        {
            int fd = buf[i].data.fd;
            Channel* ch = /* 从 Channel 指针获取 */ nullptr;  // 提示：需要在 EventLoop 里维护 fd→Channel* 的映射

            if (ch)
            {
                ch->HandleEvent(buf[i].events);
            }
        }

        // ④ 执行其他线程塞进来的回调
        DoPendingFunctors();
    }

    looping_ = false;
}

// ============================================================
// 退出事件循环
// ============================================================
// 线程安全：其他线程可以调 Quit() 来优雅停止 EventLoop
void EventLoop::Quit()
{
    quit_ = true;

    // 如果调用者不在 EventLoop 线程，需要唤醒 epoll_wait
    // 否则 EventLoop 可能永远阻塞在 Wait() 上
    if (!IsInLoopThread())
    {
        // 写入 eventfd 唤醒
        uint64_t one = 1;
        ::write(wakeup_fd_, &one, sizeof(one));
    }
}

// ============================================================
// 跨线程调用：在 EventLoop 线程执行一个回调
// ============================================================
void EventLoop::RunInLoop(Functor cb)
{
    if (IsInLoopThread())
    {
        // 就在 EventLoop 线程 → 直接执行
        cb();
    }
    else
    {
        // 在其他线程 → 入队，唤醒
        QueueInLoop(std::move(cb));
    }
}

void EventLoop::QueueInLoop(Functor cb)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_functors_.push_back(std::move(cb));
    }

    // 如果在其他线程，需要唤醒 EventLoop
    // 如果在 EventLoop 线程但在 QueueInLoop 中，也需要唤醒
    // （因为 DoPendingFunctors 已执行完，新的 cb 在下一次 Wait 前入队）
    if (!IsInLoopThread())
    {
        uint64_t one = 1;
        ::write(wakeup_fd_, &one, sizeof(one));
    }
}

// ============================================================
// 执行跨线程回调
// ============================================================
void EventLoop::DoPendingFunctors()
{
    std::vector<Functor> functors;
    {
        // 加锁交换：把 pending_functors_ 移到本地，尽快释放锁
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pending_functors_);
    }

    for (auto& f : functors)
    {
        f();  // 在 EventLoop 线程执行
    }
}

// ============================================================
// 处理 eventfd 唤醒
// ============================================================
void EventLoop::HandleWakeup()
{
    // 消费 eventfd 里的数据，让它恢复"不可读"状态
    uint64_t one = 0;
    ::read(wakeup_fd_, &one, sizeof(one));
    // DoPendingFunctors 已在 Loop() 末尾调用，这里不需要再做
}

// ============================================================
// 断言/判断所在线程
// ============================================================
bool EventLoop::IsInLoopThread() const
{
    return tid_ == std::this_thread::get_id();
}

void EventLoop::AssertInLoopThread()
{
    assert(IsInLoopThread());
}

// ============================================================
// Channel 管理
// ============================================================
// Channel 调用 EnableRead/EnableWrite 时通知 EventLoop 更新 epoll
void EventLoop::UpdateChannel(Channel* ch)
{
    AssertInLoopThread();
    // 在 EventLoop 中维护 fd→Channel* 映射
    // 调用 epoll_->Mod(ch->Fd(), ch->Events())
}

void EventLoop::RemoveChannel(Channel* ch)
{
    AssertInLoopThread();
    // 从 fd→Channel* 映射中移除
    // 调用 epoll_->Del(ch->Fd())
}

// ============================================================
// 析构函数
// ============================================================
EventLoop::~EventLoop()
{
    if (wakeup_fd_ >= 0)
    {
        ::close(wakeup_fd_);
    }
}

// ============================================================
// ⚠️ 实现时需要注意的细节（明天 Day 9 对照检查）：
// ============================================================
//
// 1. fd → Channel* 映射：
//    EventLoop 需要 std::unordered_map<int, Channel*> channels_
//    这样在 Loop() 里拿到就绪 fd 后，能快速找到对应的 Channel
//
// 2. EventLoop 的生命周期：
//    通常由所属线程创建，Loop() 阻塞直到 Quit()
//    不要在 Loop() 还在运行时析构 EventLoop
//
// 3. 线程安全：
//    - mutex_ 只保护 pending_functors_
//    - quit_ 用 std::atomic<bool>（多线程读写）
//    - looping_ 只用在本线程，不需要原子
//
// 4. eventfd 的值：
//    write 写入 1（8字节），read 读出并清零
//    内核维护一个 uint64_t 计数器
//    每次 write 累加，每次 read 清零
//
// 5. 调用约定：
//    UpdateChannel/RemoveChannel 必须在 EventLoop 线程调用
//    这就是为什么 Channel 的回调里能安全调它们——
//    因为 HandleEvent 是在 EventLoop::Loop() 中被调用的
// ============================================================
