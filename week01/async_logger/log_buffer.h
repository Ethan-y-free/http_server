#pragma once

#include <string>
#include <vector>
#include <algorithm>

#include "async_log_writer.h"

class LogBuffer
{
public:
	explicit LogBuffer(AsyncLogWriter* writer) : current_(kBufferSize), next_(kBufferSize), writer_(writer)
	{
		current_.clear(), next_.clear();
	}

	void Append(const char* data, size_t len)
	{
		if (current_.size() + len <= kBufferSize)
		{
			current_.insert(current_.end(), data, data + len);
		}
		else
		{
			std::swap(current_, next_);
			std::vector<char> fullBuf = std::move(next_);
			next_.reserve(kBufferSize);
			writer_->Submit(std::move(fullBuf));
			current_.clear();
			current_.insert(current_.end(), data, data + len);
		}
	}

	// 定期刷当前 buffer（SubReactor timerfd 回调调用，同线程无锁）
	void Flush()
	{
		if (!current_.empty())
		{
			std::vector<char> buf = std::move(current_);
			current_.clear();
			current_.reserve(kBufferSize);
			writer_->Submit(std::move(buf));
		}
	}

private:
	static constexpr size_t kBufferSize = 4 * 1024 * 1024;  // 4MB

	std::vector<char> current_;
	std::vector<char> next_;
	AsyncLogWriter* writer_;
};
