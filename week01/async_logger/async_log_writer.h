#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <chrono>
#include <thread>
#include <condition_variable>

class AsyncLogWriter
{
public:
	explicit AsyncLogWriter(const std::string& filepath) : filepath_(filepath), running_(false) {}

		~AsyncLogWriter()
		{
			Stop();
		}

	void Start()
	{
		running_.store(true, std::memory_order_release);
		thread_ = std::thread([this]() { ThreadFunc(); });
	}

	void Stop()
	{
		if (running_.load(std::memory_order_acquire))
		{
			running_.store(false, std::memory_order_release);
			cv_.notify_one();
			if (thread_.joinable())
			{
				thread_.join();
			}
		}
	}

	void Submit(std::vector<char> buf)
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			tasks_.push_back(std::move(buf));
		}
		cv_.notify_one();
	}

private:
	void ThreadFunc()
	{
		FILE* fp = fopen(filepath_.c_str(), "a");
		if (!fp) return;

		while (running_.load(std::memory_order_acquire))
		{
			std::vector<std::vector<char>> batch;

			{
				std::unique_lock<std::mutex> lock(mutex_);
				cv_.wait_for(lock, std::chrono::seconds(3),
					[this]() { return !tasks_.empty() || !running_.load(); });

				batch.swap(tasks_);
			}

			for (auto& buf : batch)
			{
				fwrite(buf.data(), 1, buf.size(), fp);
				buf.clear();
			}

			fflush(fp);
		}
		for (auto& buf : tasks_)
		{
			fwrite(buf.data(), 1, buf.size(), fp);
		}
		fflush(fp);
		fclose(fp);
	}

	std::string filepath_;
	std::atomic<bool> running_;

	std::mutex mutex_;
	std::condition_variable cv_;
	std::vector<std::vector<char>> tasks_;
	std::thread thread_;
};