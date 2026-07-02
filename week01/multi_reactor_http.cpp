#include "socket_raii.h"
#include "epoll.h"
#include "channel.h"
#include "buffer.h"
#include "eventloop.h"
#include "threadpool.h"
#include "http_parser.h"
#include "http_static_handler.h"

#include <iostream>
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <cstring>

struct Connection
{
	Socket sock;
	std::unique_ptr<Channel> channel;
	Buffer inputBuffer;
	Buffer outputBuffer;
	EventLoop* ownerLoop;
	HttpRequestParser parser;
	int subIndex;  // 所属 SubReactor
};

static HttpStaticHandler g_handler("/home/ethany/.vs/http_server/8043fcde-127c-492a-a0b0-72c8fb565f69/src/week01/www");
static void GenerateResponse(const HttpRequest& req, Buffer* output)
{
	g_handler.HandleRequest(req, output);
}

class MultiReactorServer
{
public:
	MultiReactorServer(EventLoop* mainLoop, uint16_t port, int subReactorCount, ThreadPool* pool)
		: mainLoop_(mainLoop), pool_(pool)
	{
		subLoops_.resize(subReactorCount);
		subClients_.resize(subReactorCount);

		for (int i = 0; i < subReactorCount; ++i)
		{
			subLoops_[i] = std::make_unique<EventLoop>();

			subThreads_.emplace_back([this, i]()
			{
				subLoops_[i]->Loop();
			});
		}

		listen_sock_.SetReuseAddr();
		listen_sock_.Bind(port);
		listen_sock_.Listen(128);
		listen_sock_.SetNonBlocking();

		std::cout << "\n========================================" << std::endl;
		std::cout << "  主从 Reactor HTTP Server" << std::endl;
		std::cout << "  MainReactor x1 + SubReactor x" << subReactorCount << std::endl;
		std::cout << "  ThreadPool 线程 " << subReactorCount << " 个" << std::endl;
		std::cout << "  http://localhost:" << port << "/" << std::endl;
		std::cout << "========================================\n" << std::endl;

		listen_channel_ = std::make_unique<Channel>(listen_sock_.Fd(), mainLoop_->EpollPtr());
		listen_channel_->SetReadCallback([this]() { OnAccept(); });
		mainLoop_->AddChannel(listen_channel_.get());
		listen_channel_->EnableRead();
	}

	~MultiReactorServer()
	{
		if (listen_channel_)
		{
			mainLoop_->RemoveChannel(listen_channel_.get());
		}

		for (auto& subLoop : subLoops_)
		{
			subLoop->Quit();
		}
		for (auto& t : subThreads_)
		{
			if (t.joinable())
			{
				t.join();
			}
		}

		subClients_.clear();
	}

	MultiReactorServer(const MultiReactorServer&) = delete;
	MultiReactorServer& operator=(const MultiReactorServer&) = delete;

private:
	void OnAccept()
	{
		std::string ip;
		int port = 0;
		Socket client_sock = listen_sock_.Accept(ip, port);
		if (client_sock.Fd() < 0)
		{
			return;
		}

		client_sock.SetNonBlocking();
		int client_fd = client_sock.Fd();

		int idx = nextSubReactor_.fetch_add(1) % static_cast<int>(subLoops_.size());
		EventLoop* subLoop = subLoops_[idx].get();

		// 将 client_sock 转移至 shared_ptr 保证可被跨线程移动与持有
		auto sock_ptr = std::make_shared<Socket>(std::move(client_sock));

		subLoop->RunInLoop([this, client_fd, sock_ptr, subLoop, idx]()
		{
			Connection conn;
			conn.sock = std::move(*sock_ptr);
			conn.channel = std::make_unique<Channel>(client_fd, subLoop->EpollPtr());
			conn.ownerLoop = subLoop;
			conn.subIndex = idx;

			conn.channel->SetReadCallback([this, fd = client_fd, sub = subLoop, idx]()
			{
				OnRead(fd, sub, idx);
			});
			conn.channel->SetWriteCallback([this, fd = client_fd, sub = subLoop, idx]()
			{
				OnWrite(fd, sub, idx);
			});
			conn.channel->SetCloseCallback([this, fd = client_fd, sub = subLoop, idx]()
			{
				OnClose(fd, sub, idx);
			});
			conn.channel->SetErrorCallback([this, fd = client_fd, sub = subLoop, idx]()
			{
				OnError(fd, sub, idx);
			});

			// 直接写入对应 SubReactor 的表（完全由该子线程操作，绝对线程安全）
			auto [it, inserted] = subClients_[idx].emplace(client_fd, std::move(conn));
			if (inserted)
			{
				it->second.ownerLoop->AddChannel(it->second.channel.get());
				it->second.channel->EnableRead();
			}
		});
	}

	void OnRead(int fd, EventLoop* sub, int idx)
	{
		bool shouldClose = false;
		auto& clients = subClients_[idx];

		auto it = clients.find(fd);
		if (it == clients.end())
		{
			return;
		}
		Connection& conn = it->second;

		int savedErrno = 0;
		ssize_t n = conn.inputBuffer.ReadFd(fd, &savedErrno);

		if (n < 0)
		{
			if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK)
			{
				return;
			}
			std::cerr << "[ERROR] ReadFd fd=" << fd << ": " << std::strerror(savedErrno) << std::endl;
			shouldClose = true;
		}
		else if (n == 0)
		{
			shouldClose = true;
		}
		else
		{
			auto result = conn.parser.Parse(&conn.inputBuffer);

			if (result == HttpRequestParser::PARSE_ERROR)
			{
				std::cerr << "[ERROR] HTTP 解析失败 fd=" << fd << std::endl;
				shouldClose = true;
			}
			else if (result == HttpRequestParser::PARSE_OK && conn.parser.IsDone())
			{
				const HttpRequest& req = conn.parser.GetRequest();
				GenerateResponse(req, &conn.outputBuffer);

				if (!req.IsKeepAlive())
				{
					shouldClose = true;
				}
				else
				{
					conn.parser.Reset();
				}
			}
		}

		if (shouldClose)
		{
			OnClose(fd, sub, idx);
			return;
		}

		// 移除多余的 ThreadPool 任务投递开销，改为当前从 SubReactor 直接写回
		// 此处因为业务逻辑极为轻量，直接在本线程完成发送能够极大提高性能
		FlushWrite(fd, idx);
	}

	void OnWrite(int fd, EventLoop* sub, int idx)
	{
		(void)sub;
		FlushWrite(fd, idx);
	}

	void OnClose(int fd, EventLoop* sub, int idx)
	{
		(void)sub;
		RemoveClientChannel(fd, idx);
		subClients_[idx].erase(fd);
	}

	void OnError(int fd, EventLoop* sub, int idx)
	{
		OnClose(fd, sub, idx);
	}

	void FlushWrite(int fd, int idx)
	{
		auto it = subClients_[idx].find(fd);
		if (it == subClients_[idx].end())
		{
			return;
		}

		Connection& conn = it->second;
		bool shouldClose = false;
		EventLoop* ownerLoop = conn.ownerLoop;

		while (conn.outputBuffer.ReadableBytes() > 0)
		{
			ssize_t sent = conn.sock.Send(conn.outputBuffer.Peek(),
				conn.outputBuffer.ReadableBytes());

			if (sent > 0)
			{
				conn.outputBuffer.Retrieve(sent);
			}
			else if (sent < 0 && errno == EAGAIN)
			{
				conn.channel->EnableWrite();
				return;
			}
			else
			{
				shouldClose = true;
				break;
			}
		}

		if (!shouldClose)
		{
			conn.channel->DisableWrite();
		}
		else
		{
			OnClose(fd, ownerLoop, idx);
		}
	}

	void RemoveClientChannel(int fd, int idx)
	{
		auto it = subClients_[idx].find(fd);
		if (it != subClients_[idx].end() && it->second.channel)
		{
			it->second.ownerLoop->RemoveChannel(it->second.channel.get());
		}
	}

	EventLoop* mainLoop_;
	std::vector<std::unique_ptr<EventLoop>> subLoops_;
	std::vector<std::thread> subThreads_;
	ThreadPool* pool_;

	Socket listen_sock_;
	std::unique_ptr<Channel> listen_channel_;

	std::vector<std::unordered_map<int, Connection>> subClients_;
	std::atomic<int> nextSubReactor_{ 0 };
};


constexpr uint16_t PORT = 8888;
constexpr int      SUB_REACTOR_COUNT = 4;
constexpr size_t   THREADPOOL_SIZE = 4;

int main()
{
	try
	{
		EventLoop mainLoop;
		ThreadPool pool(THREADPOOL_SIZE);

		MultiReactorServer server(&mainLoop, PORT, SUB_REACTOR_COUNT, &pool);

		std::cout << "[启动] MainReactor 进入 EventLoop..." << std::endl;
		mainLoop.Loop();

		std::cout << "[退出] MainReactor 正常结束" << std::endl;
	}
	catch (const std::exception& e)
	{
		std::cerr << "[FATAL] " << e.what() << std::endl;
		return 1;
	}

	return 0;
}