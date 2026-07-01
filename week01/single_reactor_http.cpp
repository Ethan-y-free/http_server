#include "socket_raii.h"
#include "epoll.h"
#include "channel.h"
#include "buffer.h"
#include "eventloop.h"
#include "http_parser.h"

#include <iostream>
#include <string>
#include <unordered_map>
#include <memory>
#include <cstring>

struct Connection
{
	Socket sock;
	std::unique_ptr<Channel> channel;
	Buffer inputBuffer;
	Buffer outputBuffer;
	HttpRequestParser parser;
};

static void GenerateResponse(const HttpRequest& req, Buffer* output)
{
	std::string body;
	std::string status;

	if (req.GetMethod() == "GET" || req.GetMethod() == "HEAD")
	{
		status = "200 OK";
		body = "<html><body><h1>Hello from single_reactor_http!</h1>"
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

class SingleReactorServer
{
public:
	SingleReactorServer(EventLoop* loop, uint16_t port)
		: loop_(loop)
	{
		listen_sock_.SetReuseAddr();
		listen_sock_.Bind(port);
		listen_sock_.Listen(128);
		listen_sock_.SetNonBlocking();

		std::cout << "\n========================================" << std::endl;
		std::cout << "  单 Reactor HTTP Server" << std::endl;
		std::cout << "  http://localhost:" << port << "/" << std::endl;
		std::cout << "========================================\n" << std::endl;

		listen_channel_ = std::make_unique<Channel>(listen_sock_.Fd(), loop_->EpollPtr());
		listen_channel_->SetReadCallback([this]() { OnAccept(); });
		loop_->AddChannel(listen_channel_.get());
		listen_channel_->EnableRead();
	}

	~SingleReactorServer()
	{
		for (auto& [fd, conn] : clients_)
		{
			loop_->RemoveChannel(conn.channel.get());
		}
		clients_.clear();

		if (listen_channel_)
		{
			loop_->RemoveChannel(listen_channel_.get());
		}
	}

	SingleReactorServer(const SingleReactorServer&) = delete;
	SingleReactorServer& operator=(const SingleReactorServer&) = delete;

private:
	void OnAccept()
	{
		std::string ip;
		int port = 0;
		Socket client_sock = listen_sock_.Accept(ip, port);
		if (client_sock.Fd() < 0) return;

		client_sock.SetNonBlocking();
		int client_fd = client_sock.Fd();

		Connection conn;
		conn.sock = std::move(client_sock);
		conn.channel = std::make_unique<Channel>(client_fd, loop_->EpollPtr());

		conn.channel->SetReadCallback([this, fd = client_fd]() { OnRead(fd); });
		conn.channel->SetWriteCallback([this, fd = client_fd]() { OnWrite(fd); });
		conn.channel->SetCloseCallback([this, fd = client_fd]() { OnClose(fd); });
		conn.channel->SetErrorCallback([this, fd = client_fd]() { OnError(fd); });

		loop_->AddChannel(conn.channel.get());
		conn.channel->EnableRead();
		clients_.emplace(client_fd, std::move(conn));
	}

	void OnRead(int fd)
	{
		auto it = clients_.find(fd);
		if (it == clients_.end()) return;

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
		FlushWrite(fd);

		if (shouldClose)
		{
			auto it = clients_.find(fd);
			if (it != clients_.end())
			{
				OnClose(fd);
			}
		}
	}

	void OnWrite(int fd)
	{
		FlushWrite(fd);
	}

	void OnClose(int fd)
	{
		auto it = clients_.find(fd);
		if (it != clients_.end() && it->second.channel)
		{
			loop_->RemoveChannel(it->second.channel.get());  // 从 epoll 摘除 + 清 events_
			clients_.erase(it);
		}
	}

	void OnError(int fd)
	{
		OnClose(fd);
	}

	void FlushWrite(int fd)
	{
		auto it = clients_.find(fd);
		if (it == clients_.end()) return;

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
				OnClose(fd);
				return;
			}
		}

		conn.channel->DisableWrite();
	}

	EventLoop* loop_;
	Socket listen_sock_;
	std::unique_ptr<Channel> listen_channel_;
	std::unordered_map<int, Connection> clients_;
};

constexpr uint16_t PORT = 8888;

int main()
{
	try
	{
		EventLoop loop;
		SingleReactorServer server(&loop, PORT);

		std::cout << "[启动] 进入 EventLoop..." << std::endl;
		loop.Loop();

		std::cout << "[退出] EventLoop 正常结束" << std::endl;
	}
	catch (const std::exception& e)
	{
		std::cerr << "[FATAL] " << e.what() << std::endl;
		return 1;
	}

	return 0;
}
