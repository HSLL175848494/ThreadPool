#ifndef HSLL_TPGROUPALLOCATOR
#define HSLL_TPGROUPALLOCATOR

#include "TPBlockQueue.hpp"

namespace HSLL
{
	namespace INNER
	{
		constexpr float HSLL_QUEUE_FULL_FACTOR_MAIN = 0.999;
		constexpr float HSLL_QUEUE_FULL_FACTOR_OTHER = 0.995;

		static_assert(HSLL_QUEUE_FULL_FACTOR_MAIN > 0 && HSLL_QUEUE_FULL_FACTOR_MAIN <= 1, "Invalid factors.");
		static_assert(HSLL_QUEUE_FULL_FACTOR_OTHER > 0 && HSLL_QUEUE_FULL_FACTOR_OTHER <= 1, "Invalid factors.");

		template<class T>
		class RoundRobinGroup
		{
			template<class TYPE>
			friend class TPGroupAllocator;

			unsigned int nowCount;
			unsigned int nowIndex;
			unsigned int taskThreshold;
			unsigned int mainFullThreshold;
			unsigned int otherFullThreshold;
			std::vector<TPBlockQueue<T>*>* assignedQueues;

			void advance_index()
			{
				if (nowCount >= taskThreshold)
				{
					nowIndex = (nowIndex + 1) % assignedQueues->size();
					nowCount = 0;
				}
			}

		public:

			void resetAndInit(std::vector<TPBlockQueue<T>*>* queues, unsigned int capacity, unsigned int threshold)
			{
				nowCount = 0;
				nowIndex = 0;
				this->assignedQueues = queues;
				this->taskThreshold = threshold;
				this->mainFullThreshold = capacity * HSLL_QUEUE_FULL_FACTOR_MAIN;
				this->otherFullThreshold = capacity * HSLL_QUEUE_FULL_FACTOR_OTHER;
			}

			TPBlockQueue<T>* current_queue()
			{
				TPBlockQueue<T>* queue = (*assignedQueues)[nowIndex];

				if (queue->get_size() <= mainFullThreshold)
					return queue;
				else
					return nullptr;
			}

			TPBlockQueue<T>* available_queue()
			{
				TPBlockQueue<T>* candidateQueue;

				for (int i = 0; i < assignedQueues->size() - 1; ++i)
				{
					nowIndex = (nowIndex + 1) % assignedQueues->size();
					candidateQueue = (*assignedQueues)[nowIndex];

					if (candidateQueue->get_size() <= otherFullThreshold)
					{
						nowCount = 0;
						return candidateQueue;
					}
				}

				return nullptr;
			}

			void record(unsigned int count)
			{
				if (assignedQueues->size() == 1)
					return;

				if (count)
				{
					nowCount += count;
					advance_index();
				}
				else
				{
					nowIndex = (nowIndex + 1) % assignedQueues->size();
					nowCount = 0;
					return;
				}
			}
		};

		template<class T>
		class TPGroupAllocator
		{
			unsigned int capacity;
			unsigned int queueCount;
			unsigned int fullThreshold;
			unsigned int moveThreshold;

			TPBlockQueue<T>* queues;
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
			}

			void rebuild_slot_assignments()
			{
				if (threadSlots.size())
				{
					for (int i = 0; i < threadSlots.size(); ++i)
						threadSlots[i].clear();

					distribute_queues_to_threads(threadSlots.size());
					reinitialize_groups();
				}
			}

			void reinitialize_groups()
			{
				if (threadSlots.size())
				{
					unsigned int slotIndex = 0;
					for (auto& group : threadGroups)
					{
						group.second.resetAndInit(&threadSlots[slotIndex], capacity, moveThreshold);
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
					for (unsigned int k = 0; k < queueCount; ++k)
						slot.emplace_back(queues + k);
				}
				else
				{
					for (unsigned int k = queueCount; k > 0; --k)
						slot.emplace_back(queues + k - 1);
				}
			}

			void handle_remainder_case(unsigned int threadCount)
			{
				bool fillDirection = false;
				unsigned int balancedCount = calculate_balanced_thread_count(queueCount, threadCount - 1);
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

				if (threadCount <= queueCount)
				{
					unsigned int queuesPerThread = queueCount / threadCount;
					unsigned int remainder = queueCount % threadCount;

					if (remainder)
					{
						handle_remainder_case(threadCount);
					}
					else
					{
						for (unsigned int i = 0; i < threadCount; ++i)
							for (unsigned int k = 0; k < queuesPerThread; ++k)
								threadSlots[i].emplace_back(queues + i * queuesPerThread + k);
					}
				}
				else
				{
					unsigned int threadsPerQueue = threadCount / queueCount;
					unsigned int remainder = threadCount % queueCount;
					if (remainder)
					{
						handle_remainder_case(threadCount);
					}
					else
					{
						for (unsigned int i = 0; i < threadCount; ++i)
							threadSlots[i].emplace_back(queues + i / threadsPerQueue);
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
				this->queues = queues;
				this->capacity = capacity;
				this->queueCount = queueCount;
				this->moveThreshold = threshold;
				this->fullThreshold = capacity * HSLL_QUEUE_FULL_FACTOR_OTHER;
			}

			RoundRobinGroup<T>* find(std::thread::id threadId)
			{
				auto it = threadGroups.find(threadId);

				if (it != threadGroups.end())
					return &(it->second);

				return nullptr;
			}

			TPBlockQueue<T>* available_queue(RoundRobinGroup<T>* group)
			{
				unsigned int size = group->assignedQueues->size();

				if (size == queueCount)
					return nullptr;

				unsigned int start = std::rand() % queueCount;
				std::vector <TPBlockQueue<T>*>& assignedQueues = *group->assignedQueues;
				TPBlockQueue<T>* lowBounds = assignedQueues[0];

				for (unsigned int i = 0; i < queueCount; ++i)
				{
					TPBlockQueue<T>* queue = queues + (start + i) % queueCount;

					if (queues == lowBounds)
					{
						i += size - 1;
						continue;
					}

					if (queue->get_size() <= fullThreshold)
						return queue;
				}

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
				if (this->queueCount != newQueueCount)
				{
					this->queueCount = newQueueCount;
					rebuild_slot_assignments();
				}
			}
		};
	}
}

#endif // !HSLL_TPGROUPALLOCATOR