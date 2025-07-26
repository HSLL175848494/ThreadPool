#ifndef HSLL_TPGROUPALLOCATOR
#define HSLL_TPGROUPALLOCATOR

#include<map>
#include<vector>
#include"TPBlockQueue.hpp"

namespace HSLL
{
	template<class T>
	class RoundRobinGroup
	{
		unsigned int currentIndex;
		unsigned int taskCount;
		unsigned int capacityThreshold;
		unsigned int taskThreshold;
		std::vector<TPBlockQueue<T>*>* assignedQueues;

		void advance_index()
		{
			if (taskCount >= taskThreshold)
			{
				currentIndex = (currentIndex + 1) % assignedQueues->size();
				taskCount = 0;
			}
		}

	public:

		void resetAndInit(std::vector<TPBlockQueue<T>*>* queues, unsigned int capacity, unsigned int threshold)
		{
			currentIndex = 0;
			taskCount = 0;
			this->assignedQueues = queues;
			this->capacityThreshold = capacity * 0.995;
			this->taskThreshold = threshold;
		}

		TPBlockQueue<T>* current_queue()
		{
			return (*assignedQueues)[currentIndex];
		}

		TPBlockQueue<T>* available_queue()
		{
			TPBlockQueue<T>* candidateQueue;

			for (int i = 0; i < assignedQueues->size() - 1; ++i)
			{
				currentIndex = (currentIndex + 1) % assignedQueues->size();
				candidateQueue = (*assignedQueues)[currentIndex];

				if (candidateQueue->get_size() <= capacityThreshold)
				{
					taskCount = 0;
					return candidateQueue;
				}
			}

			return nullptr;
		}

		void record(unsigned int taskSize)
		{
			if (taskSize)
			{
				taskCount += taskSize;
				advance_index();
			}
			else
			{
				currentIndex = (currentIndex + 1) % assignedQueues->size();
				taskCount = 0;
				return;
			}
		}
	};

	template<class T>
	class TPGroupAllocator
	{
		unsigned int totalQueues;
		unsigned int taskThreshold;
		unsigned int queueCapacity;
		TPBlockQueue<T>* queueArray;
		std::vector<std::vector<TPBlockQueue<T>*>> threadSlots;
		std::map<std::thread::id, RoundRobinGroup<T>> threadGroups;

		void manage_thread_entry(bool addThread, std::thread::id threadId)
		{
			if (addThread)
			{
				if (threadGroups.find(threadId) == threadGroups.end())
					threadGroups.insert({ threadId, RoundRobinGroup<T>() });
				else
					return;
			}
			else
			{
				auto group = threadGroups.find(threadId);

				if (group != threadGroups.end())
					threadGroups.erase(group);
				else
					return;
			}

			if (addThread)
				threadSlots.emplace_back(std::vector<TPBlockQueue<T>*>());
			else
				threadSlots.pop_back();

			rebuild_slot_assignments();
			reinitialize_groups();
		}

		void rebuild_slot_assignments()
		{
			if (threadSlots.size())
			{
				for (int i = 0; i < threadSlots.size(); ++i)
					threadSlots[i].clear();

				distribute_queues_to_threads(threadSlots.size());
			}
		}

		void reinitialize_groups()
		{
			if (threadSlots.size())
			{
				unsigned int slotIndex = 0;
				for (auto& group : threadGroups)
				{
					group.second.resetAndInit(&threadSlots[slotIndex], queueCapacity, taskThreshold);
					slotIndex++;
				}
			}
		}

		static unsigned int calculate_balanced_thread_count(unsigned int queueCount, unsigned int threadCount)
		{
			if (threadCount > queueCount)
			{
				while (threadCount > queueCount)
				{
					if (!(threadCount % queueCount))
						break;

					threadCount--;
				}
			}
			else
			{
				while (threadCount)
				{
					if (!(queueCount % threadCount))
						break;

					threadCount--;
				}
			}

			return threadCount;
		}

		void populate_slot(bool forwardOrder, std::vector<TPBlockQueue<T>*>& slot)
		{
			if (forwardOrder)
			{
				for (unsigned int k = 0; k < totalQueues; k++)
					slot.emplace_back(queueArray + k);
			}
			else
			{
				for (unsigned int k = totalQueues; k > 0; k--)
					slot.emplace_back(queueArray + k - 1);
			}
		}

		void handle_remainder_case(unsigned int threadCount)
		{
			bool fillDirection = false;
			unsigned int balancedCount = calculate_balanced_thread_count(totalQueues, threadCount - 1);
			distribute_queues_to_threads(balancedCount);

			for (unsigned int i = 0; i < threadCount - balancedCount; ++i)
			{
				populate_slot(fillDirection, threadSlots[balancedCount + i]);
				fillDirection = !fillDirection;
			}
		}

		void distribute_queues_to_threads(unsigned int threadCount)
		{
			if (!threadCount)
				return;

			if (threadCount <= totalQueues)
			{
				unsigned int queuesPerThread = totalQueues / threadCount;
				unsigned int remainder = totalQueues % threadCount;

				if (remainder)
				{
					handle_remainder_case(threadCount);
				}
				else
				{
					for (unsigned int i = 0; i < threadCount; ++i)
						for (unsigned int k = 0; k < queuesPerThread; k++)
							threadSlots[i].emplace_back(queueArray + i * queuesPerThread + k);
				}
			}
			else
			{
				unsigned int threadsPerQueue = threadCount / totalQueues;
				unsigned int remainder = threadCount % totalQueues;
				if (remainder)
				{
					handle_remainder_case(threadCount);
				}
				else
				{
					for (unsigned int i = 0; i < threadCount; i++)
						threadSlots[i].emplace_back(queueArray + i / threadsPerQueue);
				}
			}

			return;
		}

	public:

		void reset()
		{
			std::vector<std::vector<TPBlockQueue<T>*>>().swap(threadSlots);
			std::map<std::thread::id, RoundRobinGroup<T>>().swap(threadGroups);
		}

		void initialize(TPBlockQueue<T>* queues, unsigned int queueCount, unsigned int capacity, unsigned int threshold)
		{
			this->queueArray = queues;
			this->totalQueues = queueCount;
			this->queueCapacity = capacity;
			this->taskThreshold = threshold;
		}

		RoundRobinGroup<T>* find(std::thread::id threadId)
		{
			auto it = threadGroups.find(threadId);

			if (it != threadGroups.end())
				return &(it->second);

			return nullptr;
		}

		void register_thread(std::thread::id threadId)
		{
			manage_thread_entry(true, threadId);
		}

		void unregister_thread(std::thread::id threadId)
		{
			manage_thread_entry(false, threadId);
		}

		void update(unsigned int newQueueCount)
		{
			if (this->totalQueues != newQueueCount)
			{
				this->totalQueues = newQueueCount;
				rebuild_slot_assignments();
			}
		}
	};
}

#endif // !HSLL_TPGROUPALLOCATOR