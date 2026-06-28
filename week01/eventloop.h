#include <sys/eventfd.h>
#include <unistd.h>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cassert>
#include <mutex>
#include <iostream>

#include "epoll.h"
#include "channel.h"

class EventLoop
{
public:
	using Functor = std::function<void()>;

	EventLoop() : tid_(std::this_thread::get_id()), epoll_(std::make_unique<Epoll>()), wakeup_fd_(CreateWakeupFd())
	{
		wakeup_channel_ = std::make_unique<Channel>(wakeup_fd_, epoll_.get());
		wakeup_channel_->SetReadCallback([this]() { HandleWakeup(); });
		wakeup_channel_->EnableRead();
	}

	~EventLoop()
	{
		if (wakeup_fd_ >= 0)
		{
			::close(wakeup_fd_);
		}
	}

	EventLoop(const EventLoop&) = delete;
	EventLoop& operator=(const EventLoop&) = delete;

	void Loop()
	{
		AssertInLoopThread();
		looping_ = true;
		quit_ = false;

		constexpr int MAX_EVENTS = 64;
		epoll_event buf[MAX_EVENTS];

		while (!quit_)
		{
			int nfds = epoll_->Wait(buf, MAX_EVENTS);
			for (int i = 0; i < nfds; ++i)
			{
				int fd = buf[i].data.fd;
				auto it = channels_.find(fd);
				if (it != channels_.end())
				{
					it->second->HandleEvent(buf[i].events);
				}
			}

			DoPendingFunctors();
		}

		looping_ = false;
	}

	void Quit()
	{
		quit_ = true;

		if (!IsInLoopThread())
		{
			uint64_t one = 1;
			::write(wakeup_fd_, &one, sizeof(one));
		}
	}

	bool IsInLoopThread() const
	{
		return tid_ == std::this_thread::get_id();
	}

	void AssertInLoopThread()
	{
		assert(IsInLoopThread());
	}

	void RunInLoop(Functor cb)
	{
		if (IsInLoopThread())
		{
			cb();
		}
		else QueueInLoop(std::move(cb));
	}

	void QueueInLoop(Functor cb)
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			pending_functors_.push_back(std::move(cb));
		}

		if (!IsInLoopThread())
		{
			uint64_t one = 1;
			::write(wakeup_fd_, &one, sizeof(one));   // 敲门
		}
	}

	void AddChannel(Channel* ch)
	{
		AssertInLoopThread();
		int fd = ch->Fd();
		auto it = channels_.find(fd);
		if (it != channels_.end())
		{
			std::cout << "fd已存在";
			return;
		}
		else channels_[fd] = ch;
		epoll_->Add(fd, ch->Events());
	}

	void UpdateChannel(Channel* ch)
	{
		AssertInLoopThread();
		epoll_->Mod(ch->Fd(), ch->Events());
	}

	void RemoveChannel(Channel* ch)
	{
		AssertInLoopThread();
		channels_.erase(ch->Fd());
		epoll_->Del(ch->Fd());
	}

	Epoll* EpollPtr() const
	{
		return epoll_.get();
	}

private:
	void HandleWakeup()
	{
		uint64_t one = 0;
		::read(wakeup_fd_, &one, sizeof(one));
	}

	int CreateWakeupFd()
	{
		int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (fd < 0)
		{
			throw std::runtime_error("eventfd() failed");
		}
		return fd;
	}

	void DoPendingFunctors()
	{
		std::vector<Functor> functors;

		{
			std::lock_guard<std::mutex> lock(mutex_);
			functors.swap(pending_functors_);
		}

		for (auto& f : functors)
		{
			f();
		}
	}

	const std::thread::id tid_;
	std::unique_ptr<Epoll> epoll_;
	std::mutex mutex_;
	std::vector<Functor> pending_functors_;


	bool looping_ = false;
	std::atomic<bool> quit_ = false;

	int wakeup_fd_ = -1;
	std::unique_ptr<Channel> wakeup_channel_;

	std::unordered_map<int, Channel*> channels_;
};