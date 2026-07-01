#include "socket_raii.h"
#include "epoll.h"
#include "channel.h"
#include "buffer.h"
#include "eventloop.h"
#include "threadpool.h"
#include "http_parser.h"

#include <iostream>
#include <string>
#include <unordered_map>
#include <memory>
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
	int subIndex;
};

static void GenerateResponse(const HttpRequest& req, Buffer* output)
{
	std::string body;
	std::string status;

	if (req.GetMethod() == "GET" || req.GetMethod() == "HEAD")
	{
		status = "200 OK";
		body = "<html><body><h1>Hello from http_server!</h1>"
			"<p>Path: " + req.GetPath() + "</p>"
			"<p>Method: " + req.GetMethod() + "</p>"
			"</body></html>";
	}
	else if (req.GetMethod() == "POST")
	{
		status = "200 OK";
		body = "<html><body><h1>POST received</h1>"
			"<p>Body: " + req.GetBody() + "</p>"
			"</body></html>";
	}
	else
	{
		status = "405 Method Not Allowed";
		body = "<html><body><h1>405 Method Not Allowed</h1></body></html>";
	}

	char buf[4096];
	int len;

	if (req.GetMethod() == "HEAD")
	{
		len = snprintf(buf, sizeof(buf),
			"%s %s\r\n"
			"Server: tiny-http/1.0\r\n"
			"Content-Type: text/html; charset=utf-8\r\n"
			"Content-Length: %zu\r\n"
			"Connection: %s\r\n"
			"\r\n",
			req.GetVersion().c_str(), status.c_str(),
			body.size(),
			req.IsKeepAlive() ? "keep-alive" : "close");
	}
	else
	{
		len = snprintf(buf, sizeof(buf),
			"%s %s\r\n"
			"Server: tiny-http/1.0\r\n"
			"Content-Type: text/html; charset=utf-8\r\n"
			"Content-Length: %zu\r\n"
			"Connection: %s\r\n"
			"\r\n"
			"%s",
			req.GetVersion().c_str(), status.c_str(),
			body.size(),
			req.IsKeepAlive() ? "keep-alive" : "close",
			body.c_str());
	}

	output->Append(buf, static_cast<size_t>(len));
}

class MultiReactorServer
{
public:
	MultiReactorServer(EventLoop* mainLoop, uint16_t port, int subReactorCount)
		: mainLoop_(mainLoop)
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
		std::cout << "  主从 Reactor HTTP Server (优化版)" << std::endl;
		std::cout << "  MainReactor x1 + SubReactor x" << subReactorCount << std::endl;
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
	// ===== 架构设计原则 =====
	// 1. 每个 SubReactor 拥有独立的连接表 subClients_[idx]
	//    → 该表只由 SubReactor 线程自身读写，无需锁
	// 2. OnAccept 中连接对象在 RunInLoop 内构造并写入 SubReactor 线程
	//    → MainReactor 不直接操作 subClients_
	// 3. OnRead 中 HTTP 解析 + 响应生成 + 发送全在 SubReactor 线程内完成
	//    → 不投 ThreadPool（业务太轻量，线程切换开销 > 任务本身）

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

		// Socket 包装为 shared_ptr 跨越线程边界
		auto sock_ptr = std::make_shared<Socket>(std::move(client_sock));

		// 全部连接构建工作抛给 SubReactor 线程
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

			auto [it, ok] = subClients_[idx].emplace(client_fd, std::move(conn));
			if (ok)
			{
				it->second.ownerLoop->AddChannel(it->second.channel.get());
				it->second.channel->EnableRead();
			}
		});
	}

	void OnRead(int fd, EventLoop* sub, int idx)
	{
		auto it = subClients_[idx].find(fd);
		if (it == subClients_[idx].end())
		{
			return;
		}
		Connection& conn = it->second;
		bool shouldClose = false;

		int savedErrno = 0;
		ssize_t n = conn.inputBuffer.ReadFd(fd, &savedErrno);

		if (n < 0)
		{
			if (savedErrno != EAGAIN && savedErrno != EWOULDBLOCK)
			{
				shouldClose = true;
			}
			else
			{
				return;
			}
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
				shouldClose = true;
			}
			else if (result == HttpRequestParser::PARSE_OK && conn.parser.IsDone())
			{
				GenerateResponse(conn.parser.GetRequest(), &conn.outputBuffer);

				if (!conn.parser.GetRequest().IsKeepAlive())
				{
					shouldClose = true;
				}
				else
				{
					conn.parser.Reset();
				}
			}
		}

		// 先发送响应，再决定是否关闭
		RespondDirect(fd, idx);

		if (shouldClose)
		{
			auto it = subClients_[idx].find(fd);
			if (it != subClients_[idx].end())
			{
				OnClose(fd, sub, idx);
			}
		}
	}

	void RespondDirect(int fd, int idx)
	{
		auto it = subClients_[idx].find(fd);
		if (it == subClients_[idx].end())
		{
			return;
		}
		Connection& conn = it->second;

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
				OnClose(fd, conn.ownerLoop, idx);
				return;
			}
		}

		conn.channel->DisableWrite();
	}

	void OnWrite(int fd, EventLoop* sub, int idx)
	{
		(void)sub;
		RespondDirect(fd, idx);
	}

	void OnClose(int fd, EventLoop* sub, int idx)
	{
		(void)sub;
		auto it = subClients_[idx].find(fd);
		if (it != subClients_[idx].end() && it->second.channel)
		{
			it->second.ownerLoop->RemoveChannel(it->second.channel.get());
			subClients_[idx].erase(fd);

		}
	}

	void OnError(int fd, EventLoop* sub, int idx)
	{
		OnClose(fd, sub, idx);
	}

	EventLoop* mainLoop_;
	std::vector<std::unique_ptr<EventLoop>> subLoops_;
	std::vector<std::thread> subThreads_;

	Socket listen_sock_;
	std::unique_ptr<Channel> listen_channel_;

	std::vector<std::unordered_map<int, Connection>> subClients_;
	std::atomic<int> nextSubReactor_{ 0 };
};


constexpr uint16_t PORT = 8888;
constexpr int      SUB_REACTOR_COUNT = 4;

int main()
{
	try
	{
		EventLoop mainLoop;
		MultiReactorServer server(&mainLoop, PORT, SUB_REACTOR_COUNT);

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
