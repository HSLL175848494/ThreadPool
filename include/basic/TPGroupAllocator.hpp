#ifndef HSLL_TPGROUPALLOCATOR
#define HSLL_TPGROUPALLOCATOR

#include <map>
#include <thread>
#include <vector>
#include "TPBlockQueue.hpp"

namespace HSLL 
{
	namespace INNER
	{
		constexpr float HSLL_QUEUE_FULL_FACTOR_MAIN = 0.999f;
		constexpr float HSLL_QUEUE_FULL_FACTOR_OTHER = 0.995f;

		static_assert(HSLL_QUEUE_FULL_FACTOR_MAIN > 0 && HSLL_QUEUE_FULL_FACTOR_MAIN <= 1, "Invalid factors.");
		static_assert(HSLL_QUEUE_FULL_FACTOR_OTHER > 0 && HSLL_QUEUE_FULL_FACTOR_OTHER <= 1, "Invalid factors.");

		template<typename T>
		class RoundRobinGroup
		{
			template<typename TYPE>
			friend class TPGroupAllocator;

			unsigned int m_nowCount;
			unsigned int m_nowIndex;
			unsigned int m_taskThreshold;
			unsigned int m_mainFullThreshold;
			unsigned int m_otherFullThreshold;
			std::vector<TPBlockQueue<T>*>* m_assignedQueues;

			void advance_index() noexcept
			{
				if (m_nowCount >= m_taskThreshold)
				{
					m_nowIndex = (m_nowIndex + 1) % (unsigned int)(m_assignedQueues->size());
					m_nowCount = 0;
				}
			}

		public:

			void resetAndInit(std::vector<TPBlockQueue<T>*>* queues, unsigned int capacity, unsigned int threshold) noexcept
			{
				m_nowCount = 0;
				m_nowIndex = 0;
				m_assignedQueues = queues;
				m_taskThreshold = threshold;
				m_mainFullThreshold = std::max(2.0f, capacity * HSLL_QUEUE_FULL_FACTOR_MAIN);
				m_otherFullThreshold = std::max(2.0f, capacity * HSLL_QUEUE_FULL_FACTOR_OTHER);
			}

			TPBlockQueue<T>* current_queue() noexcept
			{
				TPBlockQueue<T>* queue = (*m_assignedQueues)[m_nowIndex];

				if (queue->get_size_weak() <= m_mainFullThreshold)
				{
					return queue;
				}
				else
				{
					return nullptr;
				}
			}

			TPBlockQueue<T>* available_queue() noexcept
			{
				TPBlockQueue<T>* candidateQueue;

				for (size_t i = 0; i < m_assignedQueues->size() - 1; ++i)
				{
					m_nowIndex = (m_nowIndex + 1) % m_assignedQueues->size();
					candidateQueue = (*m_assignedQueues)[m_nowIndex];

					if (candidateQueue->get_size_weak() <= m_otherFullThreshold)
					{
						m_nowCount = 0;
						return candidateQueue;
					}
				}

				return nullptr;
			}

			void record(unsigned int count) noexcept
			{
				if (m_assignedQueues->size() == 1)
				{
					return;
				}

				if (count)
				{
					m_nowCount += count;
					advance_index();
				}
				else
				{
					m_nowIndex = (m_nowIndex + 1) % m_assignedQueues->size();
					m_nowCount = 0;
					return;
				}
			}
		};

		template<typename T>
		class TPGroupAllocator
		{
			unsigned int m_capacity;
			unsigned int m_queueCount;
			unsigned int m_fullThreshold;
			unsigned int m_moveThreshold;

			TPBlockQueue<T>* m_queues;
			std::vector<std::vector<TPBlockQueue<T>*>> m_threadSlots;
			std::map<std::thread::id, RoundRobinGroup<T>> m_threadGroups;

			void manage_thread_entry(bool addThread, std::thread::id threadId) noexcept
			{
				if (addThread)
				{
					if (m_threadGroups.find(threadId) == m_threadGroups.end())
					{
						m_threadGroups.insert({ threadId, RoundRobinGroup<T>() });
					}
					else
					{
						return;
					}
				}
				else
				{
					auto group = m_threadGroups.find(threadId);

					if (group != m_threadGroups.end())
					{
						m_threadGroups.erase(group);
					}
					else
					{
						return;
					}
				}

				if (addThread)
				{
					m_threadSlots.emplace_back(std::vector<TPBlockQueue<T>*>());
				}
				else
				{
					m_threadSlots.pop_back();
				}

				rebuild_slot_assignments();
			}

			void rebuild_slot_assignments() noexcept
			{
				if (m_threadSlots.size())
				{
					for (size_t i = 0; i < m_threadSlots.size(); ++i)
					{
						m_threadSlots[i].clear();
					}

					distribute_queues_to_threads((unsigned int)m_threadSlots.size());
					reinitialize_groups();
				}
			}

			void reinitialize_groups() noexcept
			{
				if (m_threadSlots.size())
				{
					unsigned int slotIndex = 0;

					for (auto& group : m_threadGroups)
					{
						group.second.resetAndInit(&m_threadSlots[slotIndex], m_capacity, m_moveThreshold);
						slotIndex++;
					}
				}
			}

			static unsigned int calculate_balanced_thread_count(unsigned int queueCount, unsigned int threadCount) noexcept
			{
				if (threadCount > queueCount)
				{
					while (threadCount > queueCount)
					{
						if (!(threadCount % queueCount))
						{
							break;
						}

						threadCount--;
					}
				}
				else
				{
					while (threadCount)
					{
						if (!(queueCount % threadCount))
						{
							break;
						}

						threadCount--;
					}
				}

				return threadCount;
			}

			void populate_slot(bool forwardOrder, std::vector<TPBlockQueue<T>*>& slot) noexcept
			{
				if (forwardOrder)
				{
					for (unsigned int k = 0; k < m_queueCount; ++k)
					{
						slot.emplace_back(m_queues + k);
					}
				}
				else
				{
					for (unsigned int k = m_queueCount; k > 0; --k)
					{
						slot.emplace_back(m_queues + k - 1);
					}
				}
			}

			void handle_remainder_case(unsigned int threadCount) noexcept
			{
				bool fillDirection = false;
				unsigned int balancedCount = calculate_balanced_thread_count(m_queueCount, threadCount - 1);
				distribute_queues_to_threads(balancedCount);

				for (unsigned int i = 0; i < threadCount - balancedCount; ++i)
				{
					populate_slot(fillDirection, m_threadSlots[balancedCount + i]);
					fillDirection = !fillDirection;
				}
			}

			void distribute_queues_to_threads(unsigned int threadCount) noexcept
			{
				if (!threadCount)
				{
					return;
				}

				if (threadCount <= m_queueCount)
				{
					unsigned int queuesPerThread = m_queueCount / threadCount;
					unsigned int remainder = m_queueCount % threadCount;

					if (remainder)
					{
						handle_remainder_case(threadCount);
					}
					else
					{
						for (unsigned int i = 0; i < threadCount; ++i)
						{
							for (unsigned int k = 0; k < queuesPerThread; ++k)
							{
								m_threadSlots[i].emplace_back(m_queues + i * queuesPerThread + k);
							}
						}
					}
				}
				else
				{
					unsigned int threadsPerQueue = threadCount / m_queueCount;
					unsigned int remainder = threadCount % m_queueCount;

					if (remainder)
					{
						handle_remainder_case(threadCount);
					}
					else
					{
						for (unsigned int i = 0; i < threadCount; ++i)
						{
							m_threadSlots[i].emplace_back(m_queues + i / threadsPerQueue);
						}
					}
				}

				return;
			}

		public:

			void reset() noexcept
			{
				std::vector<std::vector<TPBlockQueue<T>*>>().swap(m_threadSlots);
				std::map<std::thread::id, RoundRobinGroup<T>>().swap(m_threadGroups);
			}

			void initialize(TPBlockQueue<T>* queues, unsigned int queueCount, unsigned int capacity, unsigned int threshold) noexcept
			{
				m_queues = queues;
				m_capacity = capacity;
				m_queueCount = queueCount;
				m_moveThreshold = threshold;
				m_fullThreshold = std::max(2u, (unsigned int)(capacity * HSLL_QUEUE_FULL_FACTOR_MAIN));
			}

			RoundRobinGroup<T>* find(std::thread::id threadId) noexcept
			{
				auto it = m_threadGroups.find(threadId);

				if (it != m_threadGroups.end())
				{
					return &(it->second);
				}

				return nullptr;
			}

			TPBlockQueue<T>* available_queue(RoundRobinGroup<T>* group) noexcept
			{
				unsigned int size = (unsigned int)group->m_assignedQueues->size();

				if (size == m_queueCount)
				{
					return nullptr;
				}

				std::vector <TPBlockQueue<T>*>& assignedQueues = *group->m_assignedQueues;
				long long start = (assignedQueues[0] - m_queues + size) % m_queueCount;

				for (unsigned int i = 0; i < m_queueCount - size; ++i)
				{
					TPBlockQueue<T>* queue = m_queues + (start + i) % m_queueCount;

					if (queue->get_size_weak() <= m_fullThreshold)
					{
						return queue;
					}
				}

				return nullptr;
			}

			void register_thread(std::thread::id threadId) noexcept
			{
				manage_thread_entry(true, threadId);
			}

			void unregister_thread(std::thread::id threadId) noexcept
			{
				manage_thread_entry(false, threadId);
			}

			void update(unsigned int newQueueCount) noexcept
			{
				if (m_queueCount != newQueueCount)
				{
					m_queueCount = newQueueCount;
					rebuild_slot_assignments();
				}
			}
		};
	}
}

#endif // !HSLL_TPGROUPALLOCATOR