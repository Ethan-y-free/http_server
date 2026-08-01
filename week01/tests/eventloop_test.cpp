#include <gtest/gtest.h>
#include "eventloop.h"
#include "channel.h"

#include <atomic>
#include <thread>
#include <chrono>
#include <unistd.h>

// ============================================================
// 用例 1：构造后 IsInLoopThread 为 true
// ============================================================
TEST(EventLoopTest, Construct)
{
    EventLoop loop;

    // 构造函数里 tid_ = std::this_thread::get_id()
    EXPECT_TRUE(loop.IsInLoopThread());
}

// ============================================================
// 用例 2：RunInLoop 同线程直接执行
// ============================================================
TEST(EventLoopTest, RunInLoopSameThread)
{
    EventLoop loop;
    int x = 0;

    loop.RunInLoop([&x]()
        {
            x = 42;
        });

    EXPECT_EQ(x, 42);  // 同步执行，已经改好了
}

// ============================================================
// 用例 3：Loop + Quit — 子线程跑循环，外部 Quit
// ============================================================
TEST(EventLoopTest, LoopAndQuit)
{
    EventLoop loop;

    std::thread t([&loop]()
        {
            loop.Loop();  // 阻塞直到 Quit()
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    loop.Quit();   // 设置 quit_ + eventfd 敲门
    t.join();       // Loop 退出后 join
}

// ============================================================
// 用例 4：QueueInLoop 跨线程投递任务
// ============================================================
TEST(EventLoopTest, QueueInLoop)
{
    EventLoop loop;
    std::atomic<int> x{ 0 };

    std::thread t([&loop]()
        {
            loop.Loop();
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 从主线程投递任务到 Loop 线程
    loop.QueueInLoop([&x]()
        {
            x.store(42);
        });

    // 等 Loop 线程消费
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    loop.Quit();
    t.join();

    EXPECT_EQ(x.load(), 42);
}

// ============================================================
// 用例 5：AddChannel + RemoveChannel
// ============================================================
TEST(EventLoopTest, AddAndRemoveChannel)
{
    EventLoop loop;

    // 用 pipe 创建一个真实的 fd
    int pipefd[2];
    ASSERT_EQ(::pipe(pipefd), 0);

    {
        Channel ch(pipefd[0], loop.EpollPtr());
        loop.AddChannel(&ch);

        // AddChannel 后 epoll 里应该有这个 fd
        // （无法直接验证 epoll 内部状态，至少不崩溃）
    }
    // ch 析构 → 从 epoll 摘除 fd

    ::close(pipefd[0]);
    ::close(pipefd[1]);
}

// ============================================================
// 用例 6：多次 QueueInLoop 保证全部执行
// ============================================================
TEST(EventLoopTest, MultipleQueueInLoop)
{
    EventLoop loop;
    std::atomic<int> sum{ 0 };

    std::thread t([&loop]()
        {
            loop.Loop();
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    for (int i = 0; i < 5; ++i)
    {
        loop.QueueInLoop([&sum, i]()
            {
                sum.fetch_add(i);
            });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    loop.Quit();
    t.join();

    EXPECT_EQ(sum.load(), 0 + 1 + 2 + 3 + 4);
}
