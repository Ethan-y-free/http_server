#include "socket_raii.h"
#include "epoll.h"
#include "channel.h"
#include "buffer.h"
#include "eventloop.h"
#include "threadpool.h"

#include <iostream>
#include <string>
#include <unordered_map>
#include <memory>
#include <thread>
#include <mutex>
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
};

class MultiReactorServer
{
public:
	MultiReactorServer(EventLoop* mainLoop, uint16_t port, int subReactorCount, ThreadPool* pool) : mainLoop_(mainLoop), subLoops_(subReactorCount), pool_(pool)
	{
		for (int i = 0; i < subReactorCount; ++i)
		{
			subLoops_[i] = std::make_unique<EventLoop>();

			subThreads_.emplace_back([this, i]()
				{
					std::cout << "  [SubReactor #" << i << "] 线程启动" << std::endl;
					subLoops_[i]->Loop();
					std::cout << "  [SubReactor #" << i << "] 线程退出" << std::endl;
				});
		}

		listen_sock_.SetReuseAddr();
		listen_sock_.Bind(port);
		listen_sock_.Listen(128);
		listen_sock_.SetNonBlocking();

		std::cout << "\n========================================" << std::endl;
		std::cout << "  主从 Reactor Echo Server" << std::endl;
		std::cout << "  MainReactor x1 + SubReactor x" << subReactorCount << std::endl;
		std::cout << "  ThreadPool 线程 " << subReactorCount << " 个" << std::endl;
		std::cout << "  测试: nc localhost " << port << std::endl;
		std::cout << "========================================\n" << std::endl;

		listen_channel_ = std::make_unique<Channel>(listen_sock_.Fd(), mainLoop_->EpollPtr());
		listen_channel_->SetReadCallback([this]() { OnAccept(); });
		mainLoop_->AddChannel(listen_channel_.get());
		listen_channel_->EnableRead();
	}

	~MultiReactorServer()
	{
		// ① 先停 MainReactor 监听，不再 accept 新连接
		if (listen_channel_)
		{
			mainLoop_->RemoveChannel(listen_channel_.get());
		}

		// ② 停所有 SubReactor 线程
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

		// ③ 线程全部退出后安全清理
		clients_.clear();
	}

	// 禁止拷贝/移动
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

		std::cout << "[+] 新连接 fd=" << client_fd << " 来自 " << ip << ":" << port
		          << " -> SubReactor #" << idx << std::endl;

		Connection conn;
		conn.sock = std::move(client_sock);
		conn.channel = std::make_unique<Channel>(client_fd, subLoop->EpollPtr());
		conn.ownerLoop = subLoop;

		// 注册回调（捕获 subLoop 用于 RunInLoop）
		conn.channel->SetReadCallback([this, fd = client_fd, sub = subLoop]()
			{
				OnRead(fd, sub);
			});
		conn.channel->SetWriteCallback([this, fd = client_fd, sub = subLoop]()
			{
				OnWrite(fd, sub);
			});
		conn.channel->SetCloseCallback([this, fd = client_fd, sub = subLoop]()
			{
				OnClose(fd, sub);
			});
		conn.channel->SetErrorCallback([this, fd = client_fd, sub = subLoop]()
			{
				OnError(fd, sub);
			});

		{
			std::lock_guard<std::mutex> lock(clients_mutex_);
			clients_.emplace(client_fd, std::move(conn));
		}

		subLoop->RunInLoop([this, client_fd]()
			{
				std::lock_guard<std::mutex> lock(clients_mutex_);
				auto it = clients_.find(client_fd);
				if (it != clients_.end())
				{
					it->second.ownerLoop->AddChannel(it->second.channel.get());
					it->second.channel->EnableRead();
				}
			});

		std::cout << "    [当前连接数: " << clients_.size() << "]" << std::endl;
	}

	void OnRead(int fd, EventLoop* sub)
	{
		bool shouldClose = false;
		size_t dataLen = 0;

		{
			std::lock_guard<std::mutex> lock(clients_mutex_);
			auto it = clients_.find(fd);
			if (it == clients_.end())
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
					return;  // 没数据了，等待下次事件
				}
				std::cerr << "[ERROR] ReadFd fd=" << fd << ": " << std::strerror(savedErrno) << std::endl;
				shouldClose = true;
			}
			else if (n == 0)
			{
				shouldClose = true;  // 对端关闭
			}
			else
			{
				std::string msg(conn.inputBuffer.Peek(), conn.inputBuffer.ReadableBytes());
				while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
				{
					msg.pop_back();
				}
				std::cout << "[recv " << n << "B fd=" << fd << " via SubReactor] " << msg << std::endl;

				// Echo：把读到的数据拷贝到输出 Buffer
				conn.outputBuffer.Append(conn.inputBuffer.Peek(), conn.inputBuffer.ReadableBytes());
				dataLen = conn.inputBuffer.ReadableBytes();
				conn.inputBuffer.RetrieveAll();
			}
		} // 解锁后再调 OnClose，避免死锁

		if (shouldClose)
		{
			OnClose(fd, sub);
			return;
		}

		// 投递到 ThreadPool 做计算（echo 场景只是模拟，HTTP 解析才是正经用途）
		pool_->Run([this, fd, sub, dataLen]()
			{
				std::cout << "  [Worker " << std::this_thread::get_id() << "] 处理 " << dataLen << " 字节" << std::endl;

				sub->RunInLoop([this, fd]()
					{
						FlushWrite(fd);
					});
			});
	}

	void OnWrite(int fd, EventLoop* sub)
	{
		(void)sub;
		FlushWrite(fd);
	}

	void OnClose(int fd, EventLoop* sub)
	{
		(void)sub;
		std::cout << "[-] 客户端断开 (fd=" << fd << ")" << std::endl;

		RemoveClientChannel(fd);

		{
			std::lock_guard<std::mutex> lock(clients_mutex_);
			clients_.erase(fd);
		}

		std::cout << "    [当前连接数: " << clients_.size() << "]" << std::endl;
	}

	void OnError(int fd, EventLoop* sub)
	{
		std::cerr << "[!] 客户端异常 (fd=" << fd << ")" << std::endl;
		OnClose(fd, sub);
	}

	void FlushWrite(int fd)
	{
		bool shouldClose = false;
		EventLoop* ownerLoop = nullptr;

		{
			std::lock_guard<std::mutex> lock(clients_mutex_);
			auto it = clients_.find(fd);
			if (it == clients_.end())
			{
				return;
			}

			Connection& conn = it->second;
			ownerLoop = conn.ownerLoop;

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
		} // 解锁后再调 OnClose

		if (shouldClose)
		{
			OnClose(fd, ownerLoop);
		}
	}

	void RemoveClientChannel(int fd)
	{
		std::lock_guard<std::mutex> lock(clients_mutex_);
		auto it = clients_.find(fd);
		if (it != clients_.end() && it->second.channel)
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

	std::unordered_map<int, Connection> clients_;
	std::mutex clients_mutex_;
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
