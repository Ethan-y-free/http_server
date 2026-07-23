#pragma once

#include "eventloop.h"
#include "socket_raii.h"
#include "tcp_connection.h"
#include "async_logger/logger.h"
#include "sub_reactor.h"
#include "channel.h"

#include <atomic>

struct TcpServerConfig
{
    uint16_t port = 8888;
    int subReactorCount = 4;
    int idleTimeoutMs = 60000;           
    TcpConnection::MessageCallback onMessage;  
};

class TcpServer
{
public:
    TcpServer(const TcpServerConfig& config, AsyncLogWriter* logWriter) : config_(config), logWriter_(logWriter) {}

    ~TcpServer()
    {
        mainLoop_.RemoveChannel(listenChannel_.get());
        listenSock_.Close();  

        for (int i = 0; i < config_.subReactorCount; ++i)
        {
            subReactors_[i]->Stop();
        }

        mainLoop_.Quit();
    }

    void Start()
    {
        for (int i = 0; i < config_.subReactorCount; ++i)
        {
            subReactors_.push_back(std::make_unique<SubReactor>(config_.idleTimeoutMs, logWriter_));
        }
        listenSock_.SetReuseAddr();
        listenSock_.Bind(config_.port);
        listenSock_.Listen(128);

        listenChannel_ = std::make_unique<Channel>(listenSock_.Fd(), mainLoop_.EpollPtr());
        listenChannel_->SetReadCallback([this]() { OnAccept(); });
        mainLoop_.AddChannel(listenChannel_.get());
        listenChannel_->EnableRead();

        for (auto& sr : subReactors_) sr->Start();
        mainLoop_.Loop();
    }

private:
    void OnAccept()
    {
        int idx = roundRobin_.fetch_add(1) % config_.subReactorCount;

        Socket clientSock = listenSock_.Accept();
        clientSock.SetNonBlocking();
        int clientFd = clientSock.ReleaseFd();         
        subReactors_[idx]->AddConnection(clientFd, config_.onMessage);
    } 

    Socket listenSock_;
    std::unique_ptr<Channel> listenChannel_;
    std::vector<std::unique_ptr<SubReactor>> subReactors_;

    EventLoop mainLoop_;       
    TcpServerConfig config_;
    AsyncLogWriter* logWriter_;

    std::atomic<int> roundRobin_{ 0 };
};