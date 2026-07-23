#pragma once

#include "socket_raii.h"
#include "epoll.h"
#include "channel.h"
#include "buffer.h"
#include "eventloop.h"

#include <functional>   
#include <memory>

class TcpConnection
{
public:
    using MessageCallback = std::function<void(TcpConnection*, Buffer*)>;
    using WriteCompleteCallback = std::function<void(TcpConnection*)>;
    using CloseCallback = std::function<void(TcpConnection*)>;
    using ErrorCallback = std::function<void(TcpConnection*, int err)>;

    TcpConnection(int fd, EventLoop* loop) : sock_(fd), channel_(std::make_unique<Channel>(fd, loop->EpollPtr())), ownerLoop_(loop), state_(Connecting)
    {
        channel_->SetReadCallback([this]() { OnRead(); });
        channel_->SetWriteCallback([this]() { OnWrite(); });
        channel_->SetCloseCallback([this]() { OnClose(); });
        channel_->SetErrorCallback([this]() { OnClose(); });
    }

    // 禁止拷贝，允许移动
    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    TcpConnection(TcpConnection&&) = default;
    TcpConnection& operator=(TcpConnection&&) = default;

    ~TcpConnection() = default;

    void ConnectEstablished()
    {
        ownerLoop_->AssertInLoopThread();
        state_ = Connected;
        ownerLoop_->AddChannel(channel_.get());
        channel_->EnableRead();
    }

    void SetMessageCallback(MessageCallback cb) { messageCallback_ = std::move(cb); };
    void SetWriteCompleteCallback(WriteCompleteCallback cb) { writeCompleteCallback_ = std::move(cb); };
    void SetCloseCallback(CloseCallback cb) { closeCallback_ = std::move(cb); };
    void SetErrorCallback(ErrorCallback cb) { errorCallback_ = std::move(cb); };

    void Send(const void* data, size_t len)
    {
        outputBuffer_.Append(static_cast<const char*>(data), len);
        OnWrite();
    }

    void Send(Buffer* data)
    {
        outputBuffer_.Append(data->Peek(), data->ReadableBytes());
        OnWrite();
    }

    void Shutdown()
    {
        state_ = Disconnecting;
        if (outputBuffer_.ReadableBytes())
        {
            OnWrite();
        }
        else ForceClose();
    }

    void ForceClose()
    {
        ownerLoop_->AssertInLoopThread();
        OnClose();
    }

    int Fd() const
    {
        return sock_.Fd();
    }

    EventLoop* ownerLoop() const
    {
        return ownerLoop_;
    }

    void OnRead()
    {
        ownerLoop_->AssertInLoopThread();
        if (sock_.Fd() < 0) return;

        int saved_errno = 0;
        ssize_t n = inputBuffer_.ReadFd(sock_.Fd(), &saved_errno);
        if (n > 0)
        {
            if (messageCallback_)
            {
                messageCallback_(this, &inputBuffer_);
            }
        }
        else if (n == 0)
        {
            OnClose();
        }
        else
        {
            if (saved_errno != EAGAIN && saved_errno != EWOULDBLOCK) 
            {
                if (errorCallback_)
                {
                    errorCallback_(this, saved_errno);
                }
                OnClose();
            }
        }
    }

    void OnWrite()
    {
        while (outputBuffer_.ReadableBytes())
        {
            ssize_t sent = sock_.Send(outputBuffer_.Peek(), outputBuffer_.ReadableBytes());
            if (sent > 0) outputBuffer_.Retrieve(sent);
            else if (sent < 0 && errno == EAGAIN)
            {
                channel_->EnableWrite();
                return;
            }
            else
            {
                OnClose();
                return;
            }
        }
        channel_->DisableWrite();
        if (state_ == Disconnecting) ForceClose();
        else
        {
            if (writeCompleteCallback_)
            {
                writeCompleteCallback_(this);
            }
        }
    }

    void OnClose()
    {
        if (state_ == Disconnected) return;
        state_ = Disconnected;

        if (channel_)
        {
            ownerLoop_->RemoveChannel(channel_.get());
        }
        if (closeCallback_)
        {
            closeCallback_(this);
        }
    }

private:
    // ---- 数据成员 ----
    Socket sock_;
    std::unique_ptr<Channel> channel_;
    Buffer inputBuffer_;
    Buffer outputBuffer_;
    EventLoop* ownerLoop_;

    // 状态
    enum State { Connecting, Connected, Disconnecting, Disconnected };
    State state_ = Connecting;

    // 4 个回调
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    CloseCallback closeCallback_;
    ErrorCallback errorCallback_;
};