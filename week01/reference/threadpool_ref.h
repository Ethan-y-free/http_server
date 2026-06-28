// ============================================================
// threadpool_ref.h — ThreadPool 完整参考（Day 10 用）
// ============================================================
// 使用方式：先根据 DESIGN.md §十一 自己实现，遇到困惑再对照本文件。
// ============================================================

#pragma once

#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <condition_variable>
#include <stdexcept>

class ThreadPool
{
public:
    using Task = std::function<void()>;

    explicit ThreadPool(size_t numThreads) : stop_(false)
    {
        for (size_t i = 0; i < numThreads; ++i)
        {
            workers_.emplace_back([this]() { WorkerLoop(); });
        }
    }

    ~ThreadPool()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();

        for (auto& worker : workers_)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // ===== 提交任务 =====

    // 提交无返回值的任务
    void Run(Task task)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_)
            {
                throw std::runtime_error("ThreadPool is stopped");
            }
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    // 提交有返回值的任务
    template <typename F, typename... Args>
    auto Submit(F&& f, Args&&... args) -> std::future<decltype(f(args...))>
    {
        using ReturnType = decltype(f(args...));

        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<ReturnType> result = task->get_future();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_)
            {
                throw std::runtime_error("ThreadPool is stopped");
            }
            tasks_.push([task]() { (*task)(); });
        }
        cv_.notify_one();

        return result;
    }

private:
    void WorkerLoop()
    {
        while (true)
        {
            Task task;

            {
                std::unique_lock<std::mutex> lock(mutex_);

                // 等两件事之一：stop_=true 或者 队列非空
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });

                // 该停了，且没活干了 → 退出
                if (stop_ && tasks_.empty())
                {
                    return;
                }

                // 有活 → 取出来
                task = std::move(tasks_.front());
                tasks_.pop();
            }

            // 不持锁执行任务
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<Task> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_;
};
