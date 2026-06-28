#pragma once

#include <vector>
#include <string>
#include <cstring>
#include <algorithm>

#include <stdexcept>
#include <sys/uio.h>

class Buffer
{
public:
	static constexpr size_t kCheapPrepend = 8;
	static constexpr size_t kInitialSize = 1024;

	Buffer()
	{
		buffer_.resize(kCheapPrepend + kInitialSize);
		readIndex_ = kCheapPrepend;
		writeIndex_ = kCheapPrepend;
	}
	
	// 属性
	const char* Peek() const
	{
		return buffer_.data() + readIndex_;
	}

	size_t ReadableBytes() const
	{
		return writeIndex_ - readIndex_;
	}

	size_t WritableBytes() const
	{
		return buffer_.size() - writeIndex_;
	}

	size_t PrependableBytes() const
	{
		return readIndex_;
	}

	// 写入
	void Append(const char* data, size_t len)
	{
		EnsureWritableBytes(len);
		std::copy(data, data + len, beginWrite());
		HasWritten(len);
	}

	void Append(const std::string& str)
	{
		Append(str.data(), str.size());
	}

	void Prepend(const void* data, size_t len)
	{
		if (len > readIndex_)
		{
			throw std::out_of_range("Prepend(" + std::to_string(len) + ") exceeds PrependableBytes(" + std::to_string(readIndex_) + ")");
		}
		readIndex_ -= len;
		std::memcpy(buffer_.data() + readIndex_, data, len);
	}

	char* beginWrite()
	{
		return buffer_.data() + writeIndex_;
	}

	const char* beginWrite() const
	{
		return buffer_.data() + writeIndex_;
	}

	void HasWritten(size_t len)
	{
		writeIndex_ += len;
	}

	// 消费（读出）
	void Retrieve(size_t len)
	{
		if (len < ReadableBytes())
		{
			readIndex_ += len;
		}
		else
		{
			RetrieveAll();
		}
	}

	void RetrieveUntil(const char* end)
	{
		Retrieve(end - Peek());
	}

	void RetrieveAll()
	{
		readIndex_ = kCheapPrepend;
		writeIndex_ = kCheapPrepend;
	}

	std::string RetrieveAsString(size_t len)
	{
		std::string result(Peek(), len);
		Retrieve(len);
		return result;
	}

	std::string RetrieveAllAsString()
	{
		return RetrieveAsString(ReadableBytes());
	}

	// 从socket读数据（核心）
	ssize_t ReadFd(int fd, int* savedErrno)
	{
		char extrabuf[65536];
		const size_t writable = WritableBytes();

		iovec iov[2];
		iov[0].iov_base = begin() + writeIndex_;
		iov[0].iov_len = writable;
		iov[1].iov_base = extrabuf;
		iov[1].iov_len = sizeof(extrabuf);

		const ssize_t n = readv(fd, iov, 2);
		if (n < 0)
		{
			*savedErrno = errno;
		}
		else if (static_cast<size_t>(n) <= writable)
		{
			writeIndex_ += n;
		}
		else
		{
			writeIndex_ = buffer_.size();
			Append(extrabuf, n - writable);
		}

		return n;
	}

	// 扩容
	void EnsureWritableBytes(size_t len)
	{
		if (WritableBytes() >= len)
		{
			return;
		}

		// 如果重复使用可以装下
		if (readIndex_ + WritableBytes() >= len + kCheapPrepend)
		{
			size_t readable = ReadableBytes();
			std::copy(buffer_.data() + readIndex_, buffer_.data() + writeIndex_, buffer_.data() + kCheapPrepend);

			readIndex_ = kCheapPrepend;
			writeIndex_ = readIndex_ + readable;
		}
		else
		{
			buffer_.resize(writeIndex_ + len);
		}
	}

	// 工具
	void Swap(Buffer& rhs)
	{
		buffer_.swap(rhs.buffer_);
		std::swap(readIndex_, rhs.readIndex_);
		std::swap(writeIndex_, rhs.writeIndex_);
	}

	const char* FindCRLF() const
	{
		const char kCRLF[] = "\r\n";
		const char* start = Peek();
		const char* end = beginWrite();
		const char* pos = std::search(start, end, kCRLF, kCRLF + 2);
		return (pos == end) ? nullptr : pos;
	}

	void Shrink()
	{
		buffer_.resize(writeIndex_);
		buffer_.shrink_to_fit();
	}

	size_t InternalCapacity() const
	{
		return buffer_.capacity();
	}

private:
	char* begin()
	{
		return buffer_.data();
	}

	const char* begin() const
	{
		return buffer_.data();
	}

	std::vector<char> buffer_;
	size_t readIndex_ = 0;
	size_t writeIndex_ = 0;
};