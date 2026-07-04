#pragma once

#include <algorithm>
#include <vector>
#include <unordered_set>
#include <unordered_map>

class TimerWheel
{
public:
	TimerWheel(int slotCount, int tickMs) : slotCount_(slotCount), tickMs_(tickMs)
	{
		currentSlot_ = 0;
		slots_.resize(slotCount);
	}

	void AddOrRefresh(int fd, int timeoutMs)
	{
		auto it = fdToSlot_.find(fd);
		if (it != fdToSlot_.end())
		{
			slots_[it->second].erase(fd);
			fdToSlot_.erase(it);
		}

		int targetSlot = (currentSlot_ + timeoutMs / tickMs_) % slotCount_;
		slots_[targetSlot].insert(fd);
		fdToSlot_[fd] = targetSlot;
	}

	void Remove(int fd)
	{
		auto it = fdToSlot_.find(fd);
		if (it != fdToSlot_.end())
		{
			slots_[it->second].erase(fd);
			fdToSlot_.erase(it);
		}
	}

	std::vector<int> Tick()
	{
		std::vector<int> expired;
		auto& bucket = slots_[currentSlot_];
		if (!slots_[currentSlot_].empty())
		{
			expired.assign(bucket.begin(), bucket.end());
			for (int fd : expired) fdToSlot_.erase(fd);

			bucket.clear();
		}
		currentSlot_ = (currentSlot_ + 1) % slotCount_;
		return expired;
	}

	int Size() const
	{
		return (int)fdToSlot_.size();
	}

private:
	int slotCount_;
	int tickMs_;
	int currentSlot_;
	std::vector<std::unordered_set<int>> slots_;
	std::unordered_map<int, int> fdToSlot_;
};