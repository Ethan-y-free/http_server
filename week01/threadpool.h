#pragma once

#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <stdexcept>
#include <vector>
#include <condition_variable>

class ThreadPool
{
public:
	using Task = std::function<void()>;

	explicit ThreadPool(size_t numThreads)
	{
		for (size_t i = 0; i < numThreads; ++i)
		{
			workers_.emplace_back([this]() {WorkerLoop(); });
		}
	}

	~ThreadPool()
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stop_ = true;
		}
		cv_.notify_all(); //唤醒所有线程防止程序关闭后线程还在epoll_wait

		for (auto& w : workers_)
		{
			w.join();
		}
	}

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	void Run(Task task)
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			tasks_.push(std::move(task));
		}
		cv_.notify_one();
	}

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
				cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });

				if (stop_ && tasks_.empty()) return;
				task = std::move(tasks_.front());
				tasks_.pop();
			}

			task();
		}
	}

	std::vector<std::thread> workers_;

	std::queue<Task> tasks_;
	std::mutex mutex_;
	std::condition_variable cv_;

	bool stop_ = false;
};