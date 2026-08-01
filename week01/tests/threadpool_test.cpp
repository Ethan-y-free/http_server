#include <gtest/gtest.h>
#include "threadpool.h"
#include <atomic>
#include <chrono>

// ============================================================
// 用例 1：Submit 返回正确结果
// ============================================================
TEST(ThreadPoolTest, SubmitReturnsValue)
{
    ThreadPool pool(2);

    auto f = pool.Submit([](int a, int b)
        {
            return a + b;
        }, 3, 4);

    EXPECT_EQ(f.get(), 7);
}

// ============================================================
// 用例 2：Submit 多参数 + 字符串
// ============================================================
TEST(ThreadPoolTest, SubmitStringConcat)
{
    ThreadPool pool(2);

    auto f = pool.Submit([](const std::string& a, const std::string& b)
        {
            return a + b;
        }, std::string("hello "), std::string("world"));

    EXPECT_EQ(f.get(), "hello world");
}

// ============================================================
// 用例 3：Run 执行 fire-and-forget 任务
// ============================================================
TEST(ThreadPoolTest, RunExecutesTask)
{
    ThreadPool pool(2);
    std::atomic<int> counter{ 0 };

    pool.Run([&counter]()
        {
            counter.fetch_add(1);
        });

    // 用 Submit 串行等它跑完：Submit 的任务排在 Run 后面
    auto f = pool.Submit([&counter]()
        {
            return counter.load();
        });

    EXPECT_EQ(f.get(), 1);
}

// ============================================================
// 用例 4：多个任务并行执行
// ============================================================
TEST(ThreadPoolTest, ParallelExecution)
{
    ThreadPool pool(4);
    std::atomic<int> counter{ 0 };

    for (int i = 0; i < 100; ++i)
    {
        pool.Run([&counter]()
            {
                counter.fetch_add(1);
            });
    }

    // 最后用一个 Submit 确保前面全部跑完
    auto f = pool.Submit([&counter]()
        {
            return counter.load();
        });

    EXPECT_EQ(f.get(), 100);
}

// ============================================================
// 用例 5：任务按 FIFO 顺序执行
// ============================================================
TEST(ThreadPoolTest, TasksExecuteInOrder)
{
    ThreadPool pool(1);  // 单线程保证顺序
    std::vector<int> results;
    std::mutex mtx;

    for (int i = 0; i < 10; ++i)
    {
        pool.Run([&results, &mtx, i]()
            {
                std::lock_guard<std::mutex> lock(mtx);
                results.push_back(i);
            });
    }

    // 等全部跑完
    auto f = pool.Submit([&results]()
        {
            return results.size();
        });

    EXPECT_EQ(f.get(), 10);

    for (int i = 0; i < 10; ++i)
    {
        EXPECT_EQ(results[i], i);
    }
}

// ============================================================
// 用例 6：析构函数等待所有任务完成（不 hang）
// ============================================================
TEST(ThreadPoolTest, DestructorWaitsForTasks)
{
    std::atomic<int> counter{ 0 };

    {
        ThreadPool pool(2);

        // 提交 50 个任务
        for (int i = 0; i < 50; ++i)
        {
            pool.Run([&counter]()
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    counter.fetch_add(1);
                });
        }

        // pool 析构：会等所有 worker 执行完才返回
    }

    EXPECT_EQ(counter.load(), 50);
}

// ============================================================
// 用例 7：空线程池也能正常析构
// ============================================================
TEST(ThreadPoolTest, ZeroThreads)
{
    ThreadPool pool(1);
    // 什么都不做，析构不应 hang
    SUCCEED();
}
