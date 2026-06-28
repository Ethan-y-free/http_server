/**
 * buffer.h —— 应用层缓冲区
 *
 * TCP 是字节流，没有消息边界。Buffer 在应用层做缓冲，
 * 把"从 socket 读"和"解析业务数据"解耦。
 *
 * 内存布局（muduo 经典设计）：
 *   | prependable |  readable  |  writable  |
 *   |   (8字节)   | (未消费数据) |  (可写空间) |
 *   ^             ^            ^             ^
 *   data()        readIndex_   writeIndex_   data()+size()
 *
 * prependable 的用途：事后在前面插入数据（如 HTTP 响应先写 body 再补 Content-Length）
 *
 * 核心方法 ReadFd() 使用 readv + 栈上临时空间，一次系统调用完成读取，
 * 避免"先读到栈再拷进 buffer"的两次拷贝。
 */

#pragma once

#include <vector>
#include <string>
#include <cstring>
#include <algorithm>

#include <sys/uio.h>   // readv


class Buffer
{
public:
    static constexpr size_t kCheapPrepend = 8;
    static constexpr size_t kInitialSize  = 1024;

    // ---- 构造 --------------------------------------------------
    Buffer()
    {
        buffer_.resize(kCheapPrepend + kInitialSize);
        readIndex_  = kCheapPrepend;
        writeIndex_ = kCheapPrepend;
    }

    // ---- 属性 --------------------------------------------------
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

    const char* Peek() const
    {
        return begin() + readIndex_;
    }

    // ---- 消费数据 -----------------------------------------------
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
        readIndex_  = kCheapPrepend;
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

    // ---- 写入数据 -----------------------------------------------
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
        readIndex_ -= len;
        std::memcpy(begin() + readIndex_, data, len);
    }

    // ---- 扩容 --------------------------------------------------
    void EnsureWritableBytes(size_t len)
    {
        if (WritableBytes() >= len)
        {
            return;
        }

        // 可读数据前移 + 尾部空闲合并后够用吗？
        if (readIndex_ + WritableBytes() >= len + kCheapPrepend)
        {
            size_t readable = ReadableBytes();
            std::copy(begin() + readIndex_,
                      begin() + writeIndex_,
                      begin() + kCheapPrepend);
            readIndex_  = kCheapPrepend;
            writeIndex_ = readIndex_ + readable;
        }
        else
        {
            buffer_.resize(writeIndex_ + len);
        }
    }

    // ---- 写指针（供直接写入 buffer）-------------------------------
    char* beginWrite()
    {
        return begin() + writeIndex_;
    }

    const char* beginWrite() const
    {
        return begin() + writeIndex_;
    }

    void HasWritten(size_t len)
    {
        writeIndex_ += len;
    }

    // ---- 从 fd 读数据（核心方法）----------------------------------
    //
    // readv(fd, iov, 2)：一次系统调用读到两个内存块
    //   iov[0] = buffer 可写区     ← 优先填这里
    //   iov[1] = 栈上临时 buf       ← 数据太多时兜底，再 Append 进 buffer
    //
    // 优势：数据少时不经过栈，直接进 buffer；数据多时栈上兜底不丢数据
    //
    ssize_t ReadFd(int fd, int* savedErrno)
    {
        char extrabuf[65536];
        const size_t writable = WritableBytes();

        iovec iov[2];
        iov[0].iov_base = begin() + writeIndex_;
        iov[0].iov_len  = writable;
        iov[1].iov_base = extrabuf;
        iov[1].iov_len  = sizeof(extrabuf);

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

    // ---- 工具方法 -----------------------------------------------
    void Swap(Buffer& rhs)
    {
        buffer_.swap(rhs.buffer_);
        std::swap(readIndex_,  rhs.readIndex_);
        std::swap(writeIndex_, rhs.writeIndex_);
    }

    // 查找 \r\n（HTTP 协议解析用）
    const char* FindCRLF() const
    {
        const char kCRLF[] = "\r\n";
        const char* start  = Peek();
        const char* end    = beginWrite();
        const char* pos    = std::search(start, end, kCRLF, kCRLF + 2);
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
    size_t readIndex_  = 0;
    size_t writeIndex_ = 0;
};
