#ifndef HSLL_THREADPOOL
#define HSLL_THREADPOOL

#include <vector>
#include <thread>
#include "basic/TPTask.hpp"
#include "basic/TPRWLock.hpp"
#include "basic/TPSemaphore.hpp"
#include "basic/TPBlockQueue.hpp"

#define HSLL_THREADPOOL_TIMEOUT_MILLISECONDS 5
#define HSLL_THREADPOOL_SHRINK_FACTOR 0.25
#define HSLL_THREADPOOL_EXPAND_FACTOR 0.75

static_assert(HSLL_THREADPOOL_TIMEOUT_MILLISECONDS > 0, "Invalid timeout value.");
static_assert(HSLL_THREADPOOL_SHRINK_FACTOR < HSLL_THREADPOOL_EXPAND_FACTOR&& HSLL_THREADPOOL_EXPAND_FACTOR < 1.0
	&& HSLL_THREADPOOL_SHRINK_FACTOR>0.0, "Invalid factors.");

namespace HSLL
{
	template <class T>
	class SingleStealer
	{
		template <class TYPE>
		friend class ThreadPool;
	private:

		bool monitor;
		unsigned int index;
		unsigned int queueLength;
		unsigned int* threadNum;
		unsigned int threshold;

		SpinReadWriteLock* rwLock;
		TPBlockQueue<T>* queues;
		TPBlockQueue<T>* ignore;

		SingleStealer(SpinReadWriteLock* rwLock, TPBlockQueue<T>* queues, TPBlockQueue<T>* ignore,
			unsigned int queueLength, unsigned int* threadNum, bool monitor)
		{
			this->index = 0;
			this->queueLength = queueLength;
			this->threadNum = threadNum;
			this->threshold = std::min((unsigned int)2, queueLength);
			this->rwLock = rwLock;
			this->queues = queues;
			this->ignore = ignore;
			this->monitor = monitor;
		}

		unsigned int steal(T& element)
		{
			if (monitor)
			{
				ReadLockGuard lock(*rwLock);
				return steal_inner(element);
			}
			else
			{
				return steal_inner(element);
			}
		}

		unsigned int steal_inner(T& element)
		{
			unsigned int num = *threadNum;
			for (int i = 0; i < num; ++i)
			{
				unsigned int now = (index + i) % num;
				TPBlockQueue<T>* queue = queues + now;
				if (queue->get_size() >= threshold && queue != ignore)
				{
					if (queue->dequeue(element))
					{
						index = now;
						return 1;
					}
				}
			}
			return 0;
		}
	};

	template <class T>
	class BulkStealer
	{
		template <class TYPE>
		friend class ThreadPool;

	private:

		bool monitor;
		unsigned int index;
		unsigned int batchSize;
		unsigned int queueLength;
		unsigned int* threadNum;
		unsigned int threshold;

		SpinReadWriteLock* rwLock;
		TPBlockQueue<T>* queues;
		TPBlockQueue<T>* ignore;

		BulkStealer(SpinReadWriteLock* rwLock, TPBlockQueue<T>* queues, TPBlockQueue<T>* ignore, unsigned int queueLength,
			unsigned int* threadNum, unsigned int batchSize, bool monitor)
		{
			this->index = 0;
			this->batchSize = batchSize;
			this->queueLength = queueLength;
			this->threadNum = threadNum;
			this->threshold = std::min(2 * batchSize, queueLength / 2);
			this->rwLock = rwLock;
			this->queues = queues;
			this->ignore = ignore;
			this->monitor = monitor;
		}

		unsigned int steal(T* elements)
		{
			if (monitor)
			{
				ReadLockGuard lock(*rwLock);
				return steal_inner(elements);
			}
			else
			{
				return steal_inner(elements);
			}
		}

		unsigned int steal_inner(T* elements)
		{
			unsigned int num = *threadNum;
			for (int i = 0; i < num; ++i)
			{
				unsigned int now = (index + i) % num;
				TPBlockQueue<T>* queue = queues + now;
				if (queue->get_size() >= threshold && queue != ignore)
				{
					unsigned int count = queue->dequeue_bulk(elements, batchSize);
					if (count)
					{
						index = now;
						return count;
					}
				}
			}
			return 0;
		}
	};

	/**
	 * @brief Thread pool implementation with multiple queues for task distribution
	 */
	template <class T = TaskStack<>>
	class ThreadPool
	{
		static_assert(is_generic_ts<T>::value, "TYPE must be a TaskStack type");

	private:

		unsigned int threadNum;
		unsigned int minThreadNum;
		unsigned int maxThreadNum;
		unsigned int batchSize;
		unsigned int queueLength;

		bool enableMonitor;
		Semaphore monitorSem;
		std::atomic<bool> adjustFlag;
		std::chrono::milliseconds adjustMillis;

		T* containers;
		Semaphore* stoppedSem;
		Semaphore* restartSem;
		SpinReadWriteLock rwLock;
		std::atomic<bool> exitFlag;
		std::atomic<bool> shutdownPolicy;

		std::thread monitor;
		TPBlockQueue<T>* queues;
		std::vector<std::thread> workers;
		std::atomic<unsigned int> index;

	public:

		ThreadPool() : queues(nullptr) {}

		/**
		 * @brief Initializes thread pool with fixed number of threads (no dynamic scaling)
		 * @param capacity Capacity of each internal task queue (must be >= 2)
		 * @param threadNum Fixed number of worker threads (must be != 0)
		 * @param batchSize Maximum number of tasks processed per batch (must be != 0)
		 * @return true  Initialization successful
		 * @return false Initialization failed (invalid parameters or resource allocation failure)
		 */
		bool init(unsigned int capacity, unsigned int threadNum, unsigned int batchSize) noexcept
		{
			assert(!queues);

			if (!batchSize || !threadNum || capacity < 2)
				return false;

			if (!initResourse(capacity, threadNum, batchSize))
				return false;

			this->index = 0;
			this->exitFlag = false;
			this->adjustFlag = false;
			this->enableMonitor = false;
			this->shutdownPolicy = true;
			this->minThreadNum = threadNum;
			this->maxThreadNum = threadNum;
			this->threadNum = maxThreadNum;
			this->batchSize = std::min(batchSize, capacity / 2);
			this->queueLength = capacity;
			this->adjustMillis = adjustMillis;
			workers.reserve(maxThreadNum);

			for (unsigned i = 0; i < maxThreadNum; ++i)
				workers.emplace_back(&ThreadPool::worker, this, i);

			return true;
		}

		/**
		* @brief Initializes thread pool resources (Dynamic scaling)
		* @param capacity Capacity of each internal queue (must be >= 2)
		* @param minThreadNum Minimum number of worker threads (must be != 0)
		* @param maxThreadNum Maximum number of worker threads (must be >=minThreadNum)
		* @param batchSize Maximum tasks to process per batch (must be != 0)
		* @param adjustMillis Time interval for checking the load and adjusting the number of active threads(must be != 0)
		* @return true  Initialization successful
		* @return false Initialization failed (invalid parameters or resource allocation failure)
		*/
		bool init(unsigned int capacity, unsigned int minThreadNum, unsigned int maxThreadNum,
			unsigned int batchSize, unsigned int adjustMillis = 2500
		) noexcept
		{
			assert(!queues);

			if (!batchSize || !minThreadNum || capacity< 2 || minThreadNum > maxThreadNum || !adjustMillis)
				return false;

			if (!initResourse(capacity, maxThreadNum, batchSize))
				return false;

			this->index = 0;
			this->exitFlag = false;
			this->adjustFlag = false;
			this->enableMonitor = (minThreadNum != maxThreadNum) ? true : false;
			this->shutdownPolicy = true;
			this->minThreadNum = minThreadNum;
			this->maxThreadNum = maxThreadNum;
			this->threadNum = maxThreadNum;
			this->batchSize = std::min(batchSize, capacity / 2);
			this->queueLength = capacity;
			this->adjustMillis = std::chrono::milliseconds(adjustMillis);
			workers.reserve(maxThreadNum);

			for (unsigned i = 0; i < maxThreadNum; ++i)
				workers.emplace_back(&ThreadPool::worker, this, i);

			if (enableMonitor)
				monitor = std::thread(&ThreadPool::load_monitor, this);

			return true;
		}

#define HSLL_ENQUEUE_HELPER(exp1,exp2)  \
		assert(queues);					\
		if(maxThreadNum == 1)			\
		{								\
			return exp1;				\
		}								\
		else if (enableMonitor)			\
		{								\
			ReadLockGuard lock(rwLock); \
			return exp2;				\
		}								\
		else							\
		{								\
			return exp2;				\
		}

		/**
		 * @brief Non-blocking task emplacement with perfect forwarding
		 * @tparam POS Insertion position (HEAD or TAIL, default TAIL)
		 * @tparam Args Types of arguments for task constructor
		 * @param args Arguments forwarded to task constructor
		 * @return true if task was enqueued, false if queue was full
		 * @details Constructs task in-place at selected position without blocking
		 */
		template <INSERT_POS POS = TAIL, typename... Args>
		bool emplace(Args &&...args) noexcept
		{
			HSLL_ENQUEUE_HELPER(queues->template emplace<POS>(std::forward<Args>(args)...),
				select_queue().template emplace<POS>(std::forward<Args>(args)...)
			);
		}

		/**
		 * @brief Blocking task emplacement with indefinite wait
		 * @tparam POS Insertion position (HEAD or TAIL, default TAIL)
		 * @tparam Args Types of arguments for task constructor
		 * @param args Arguments forwarded to task constructor
		 * @return true if task was added, false if thread pool was stopped
		 * @details Waits indefinitely for queue space, constructs task at selected position
		 */
		template <INSERT_POS POS = TAIL, typename... Args>
		bool wait_emplace(Args &&...args) noexcept
		{
			HSLL_ENQUEUE_HELPER(queues->template wait_emplace<POS>(std::forward<Args>(args)...),
				select_queue().template wait_emplace<POS>(std::forward<Args>(args)...)
			);
		}

		/**
		 * @brief Blocking task emplacement with timeout
		 * @tparam POS Insertion position (HEAD or TAIL, default TAIL)
		 * @tparam Rep Chrono duration representation type
		 * @tparam Period Chrono duration period type
		 * @tparam Args Types of arguments for task constructor
		 * @param timeout Maximum duration to wait for space
		 * @param args Arguments forwarded to task constructor
		 * @return true if task was added, false on timeout or thread pool stop
		 */
		template <INSERT_POS POS = TAIL, class Rep, class Period, typename... Args>
		bool wait_emplace(const std::chrono::duration<Rep, Period>& timeout, Args &&...args) noexcept
		{
			HSLL_ENQUEUE_HELPER(queues->template wait_emplace<POS>(timeout, std::forward<Args>(args)...),
				select_queue().template wait_emplace<POS>(timeout, std::forward<Args>(args)...)
			);
		}

		/**
		 * @brief Non-blocking push for preconstructed task
		 * @tparam POS Insertion position (HEAD or TAIL, default TAIL)
		 * @tparam U Deduced task type (supports perfect forwarding)
		 * @param task Task object to enqueue
		 * @return true if task was enqueued, false if queue was full
		 */
		template <INSERT_POS POS = TAIL, class U>
		bool enqueue(U&& task) noexcept
		{
			HSLL_ENQUEUE_HELPER(queues->template enqueue<POS>(std::forward<U>(task)),
				select_queue().template enqueue<POS>(std::forward<U>(task))
			);
		}

		/**
		 * @brief Blocking push for preconstructed task with indefinite wait
		 * @tparam POS Insertion position (HEAD or TAIL, default TAIL)
		 * @tparam U Deduced task type
		 * @param task Task object to add
		 * @return true if task was added, false if thread pool was stopped
		 */
		template <INSERT_POS POS = TAIL, class U>
		bool wait_enqueue(U&& task) noexcept
		{
			HSLL_ENQUEUE_HELPER(queues->template wait_push<POS>(std::forward<U>(task)),
				select_queue().template wait_push<POS>(std::forward<U>(task))
			);
		}

		/**
		 * @brief Blocking push for preconstructed task with timeout
		 * @tparam POS Insertion position (HEAD or TAIL, default TAIL)
		 * @tparam U Deduced task type
		 * @tparam Rep Chrono duration representation type
		 * @tparam Period Chrono duration period type
		 * @param task Task object to add
		 * @param timeout Maximum duration to wait for space
		 * @return true if task was added, false on timeout or thread pool stop
		 */
		template <INSERT_POS POS = TAIL, class U, class Rep, class Period>
		bool wait_enqueue(U&& task, const std::chrono::duration<Rep, Period>& timeout) noexcept
		{
			HSLL_ENQUEUE_HELPER(queues->template wait_push<POS>(std::forward<U>(task), timeout),
				select_queue().template wait_push<POS>(std::forward<U>(task), timeout)
			);
		}

		/**
		 * @brief Non-blocking bulk push for multiple tasks
		 * @tparam METHOD Bulk construction method (COPY or MOVE, default COPY)
		 * @tparam POS Insertion position (HEAD or TAIL, default TAIL)
		 * @param tasks Array of tasks to enqueue
		 * @param count Number of tasks in array
		 * @return Actual number of tasks enqueued
		 */
		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL>
		unsigned int enqueue_bulk(T* tasks, unsigned int count) noexcept
		{
			HSLL_ENQUEUE_HELPER((queues->template enqueue_bulk<METHOD, POS>(tasks, count)),
				(select_queue_for_bulk(std::max(1u, count / 2)).template enqueue_bulk<METHOD, POS>(tasks, count))
			);
		}

		/**
		 * @brief Non-blocking bulk push for multiple tasks (dual-part version)
		 * @tparam METHOD Bulk construction method (COPY or MOVE, default COPY)
		 * @tparam POS Insertion position (HEAD or TAIL, default TAIL)
		 * @param part1 First array segment of tasks to enqueue
		 * @param count1 Number of tasks in first segment
		 * @param part2 Second array segment of tasks to enqueue
		 * @param count2 Number of tasks in second segment
		 * @return Actual number of tasks successfully enqueued (sum of both segments minus failures)
		 * @note Designed for ring buffers that benefit from batched two-part insertion.
		 */
		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL>
		unsigned int enqueue_bulk(T* part1, unsigned int count1, T* part2, unsigned int count2) noexcept
		{
			HSLL_ENQUEUE_HELPER((queues->template enqueue_bulk<METHOD, POS>(part1, count1, part2, count2)),
				(select_queue_for_bulk(std::max(1u, (count1 + count2) / 2)).template enqueue_bulk<METHOD, POS>(part1, count1, part2, count2))
			);
		}

		/**
		 * @brief Blocking bulk push with indefinite wait
		 * @tparam METHOD Bulk construction method (COPY or MOVE, default COPY)
		 * @tparam POS Insertion position (HEAD or TAIL, default TAIL)
		 * @param tasks Array of tasks to add
		 * @param count Number of tasks to add
		 * @return Actual number of tasks added before stop
		 */
		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL>
		unsigned int wait_enqueue_bulk(T* tasks, unsigned int count) noexcept
		{
			HSLL_ENQUEUE_HELPER((queues->template wait_pushBulk<METHOD, POS>(tasks, count)),
				(select_queue().template wait_pushBulk<METHOD, POS>(tasks, count))
			);
		}

		/**
		 * @brief Blocking bulk push with timeout
		 * @tparam METHOD Bulk construction method (COPY or MOVE, default COPY)
		 * @tparam POS Insertion position (HEAD or TAIL, default TAIL)
		 * @tparam Rep Chrono duration representation type
		 * @tparam Period Chrono duration period type
		 * @param tasks Array of tasks to add
		 * @param count Number of tasks to add
		 * @param timeout Maximum duration to wait for space
		 * @return Actual number of tasks added (may be less than count)
		 */
		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL, class Rep, class Period>
		unsigned int wait_enqueue_bulk(T* tasks, unsigned int count, const std::chrono::duration<Rep, Period>& timeout) noexcept
		{
			HSLL_ENQUEUE_HELPER((queues->template wait_pushBulk<METHOD, POS>(tasks, count, timeout)),
				(select_queue().template wait_pushBulk<METHOD, POS>(tasks, count, timeout))
			);
		}

		/**
		* @brief Get the maximum occupied space of the thread pool.
		*/
		unsigned long long get_max_usage()
		{
			assert(queues);
			return  maxThreadNum * queues->get_bsize();
		}

		/**
		 * @brief Waits for all tasks to complete.
		 * @note
		 *  1. During the join operation, adding any new tasks is prohibited.
		 *  2. This function is not thread-safe.
		 *	3. This function does not clean up resources. After the call, the queue can be used normally.
		 */
		void drain()
		{
			assert(queues);

			if (enableMonitor)
			{
				adjustFlag = true;
				monitorSem.release();

				while (adjustFlag)
					std::this_thread::yield();
			}

			for (int i = 0; i < threadNum; ++i)
			{
				restartSem[i].release();
				queues[i].stopWait();
			}

			for (int i = 0; i < threadNum; ++i)
			{
				stoppedSem[i].acquire();
				queues[i].enableWait();
			}

			if (enableMonitor)
				monitorSem.release();
		}

		/**
		 * @brief Stops all workers and releases resources.
		 * @param shutdownPolicy If true, performs a graceful shutdown (waits for tasks to complete);
		 *                       if false, forces an immediate shutdown.
		 * @note This function is not thread-safe.
		 * @note After calling this function, the thread pool can be reused by calling init again.
		 */
		void exit(bool shutdownPolicy = true) noexcept
		{
			assert(queues);

			if (enableMonitor)
			{
				monitorSem.release();
				monitor.join();
			}

			exitFlag = true;
			this->shutdownPolicy = shutdownPolicy;

			{
				for (unsigned i = 0; i < workers.size(); ++i)
					restartSem[i].release();

				for (unsigned i = 0; i < workers.size(); ++i)
					queues[i].stopWait();

				for (auto& worker : workers)
					worker.join();
			}

			rleaseResourse();
		}

		~ThreadPool() noexcept
		{
			if (queues)
				exit(false);
		}

		ThreadPool(const ThreadPool&) = delete;
		ThreadPool& operator=(const ThreadPool&) = delete;
		ThreadPool(ThreadPool&&) = delete;
		ThreadPool& operator=(ThreadPool&&) = delete;

	private:

		unsigned int caculate_threshold()
		{
			if (queueLength <= 40)
				return 1;
			else
				return queueLength * 0.05;
		}

		unsigned int next_index() noexcept
		{
			return index.fetch_add(1, std::memory_order_relaxed) % threadNum;
		}

		TPBlockQueue<T>& select_queue() noexcept
		{
			unsigned int index = next_index();

			if (queues[index].get_exact_size() < queueLength)
				return queues[index];

			unsigned int half = threadNum / 2;
			return queues[(index + half) % threadNum];
		}

		TPBlockQueue<T>& select_queue_for_bulk(unsigned required) noexcept
		{
			unsigned int index = next_index();

			if (queues[index].get_exact_size() + required <= queueLength)
				return queues[index];

			unsigned int half = threadNum / 2;
			return queues[(index + half) % threadNum];
		}

		void load_monitor() noexcept
		{
			unsigned int count = 0;

			while (true)
			{
				if (monitorSem.try_acquire_for(adjustMillis))
				{
					if (adjustFlag)
					{
						adjustFlag = false;
						monitorSem.acquire();
					}
					else
					{
						return;
					}
				}

				unsigned int allSize = queueLength * threadNum;
				unsigned int totalSize = 0;

				for (int i = 0; i < threadNum; ++i)
					totalSize += queues[i].get_exact_size();

				if (totalSize < allSize * HSLL_THREADPOOL_SHRINK_FACTOR && threadNum > minThreadNum)
				{
					count++;

					if (count >= 3)
					{
						rwLock.lock_write();
						threadNum--;
						rwLock.unlock_write();
						queues[threadNum].stopWait();
						stoppedSem[threadNum].acquire();
						queues[threadNum].release();
						count = 0;
					}
				}
				else
				{
					if (totalSize > allSize * HSLL_THREADPOOL_EXPAND_FACTOR && threadNum < maxThreadNum)
					{
						unsigned int newThreads = std::max(1u, (maxThreadNum - threadNum) / 2);
						unsigned int succeed = 0;
						for (int i = threadNum; i < threadNum + newThreads; ++i)
						{
							if (!queues[i].init(queueLength))
								break;

							restartSem[i].release();
							succeed++;
						}

						if (succeed > 0)
						{
							rwLock.lock_write();
							threadNum += succeed;
							rwLock.unlock_write();
						}
					}

					count = 0;
				}
			}
		}

		void worker(unsigned int index) noexcept
		{
			if (batchSize == 1)
			{
				if (maxThreadNum == 1)
					process_single1(queues + index, index);
				else
					process_single2(queues + index, index);
			}
			else
			{
				if (maxThreadNum == 1)
					process_bulk1(queues + index, index);
				else
					process_bulk2(queues + index, index);
			}
		}

		static inline void execute_tasks(T* tasks, unsigned int count)
		{
			for (unsigned int i = 0; i < count; ++i)
			{
				tasks[i].execute();
				tasks[i].~T();
			}
		}

		void process_single1(TPBlockQueue<T>* queue, unsigned int index) noexcept
		{
			T* task = containers + index * batchSize;

			while (true)
			{
				while (true)
				{
					while (queue->dequeue(*task))
					{
						task->execute();
						task->~T();
					}

					if (queue->wait_dequeue(*task, std::chrono::milliseconds(HSLL_THREADPOOL_TIMEOUT_MILLISECONDS)))
					{
						task->execute();
						task->~T();
					}

					if (queue->is_Stopped())
						break;
				}

				if (shutdownPolicy)
				{
					while (queue->dequeue(*task))
					{
						task->execute();
						task->~T();
					}
				}

				stoppedSem[index].release();
				restartSem[index].acquire();

				if (exitFlag)
					break;
			}
		}

		void process_single2(TPBlockQueue<T>* queue, unsigned int index) noexcept
		{
			T* task = containers + index * batchSize;
			SingleStealer<T> stealer(&rwLock, queues, queue, queueLength, &threadNum, enableMonitor);

			while (true)
			{
				while (true)
				{
					while (queue->dequeue(*task))
					{
						task->execute();
						task->~T();
					}

					if (stealer.steal(*task))
					{
						task->execute();
						task->~T();
					}
					else
					{
						if (queue->wait_dequeue(*task, std::chrono::milliseconds(HSLL_THREADPOOL_TIMEOUT_MILLISECONDS)))
						{
							task->execute();
							task->~T();
						}
					}

					if (queue->is_Stopped())
						break;
				}

				if (shutdownPolicy)
				{
					while (queue->dequeue(*task))
					{
						task->execute();
						task->~T();
					}
				}

				stoppedSem[index].release();
				restartSem[index].acquire();

				if (exitFlag)
					break;
			}
		}

		void process_bulk1(TPBlockQueue<T>* queue, unsigned int index) noexcept
		{
			T* tasks = containers + index * batchSize;
			unsigned int size_threshold = batchSize;
			unsigned int round_threshold = batchSize / 2;

			while (true)
			{
				unsigned int count;

				while (true)
				{
					while (true)
					{
						unsigned int round = 1;
						unsigned int size = queue->get_exact_size();

						while (size < size_threshold && round < round_threshold)
						{
							std::this_thread::yield();
							size = queue->get_exact_size();
							round++;
							std::this_thread::yield();
						}

						if (size && (count = queue->dequeue_bulk(tasks, size_threshold)))
							execute_tasks(tasks, count);
						else
							break;
					}

					count = queue->wait_dequeue_bulk(tasks, size_threshold, std::chrono::milliseconds(HSLL_THREADPOOL_TIMEOUT_MILLISECONDS));

					if (count)
						execute_tasks(tasks, count);

					if (queue->is_Stopped())
						break;
				}

				if (shutdownPolicy)
				{
					while (count = queue->dequeue_bulk(tasks, size_threshold))
						execute_tasks(tasks, count);
				}

				stoppedSem[index].release();
				restartSem[index].acquire();

				if (exitFlag)
					break;
			}
		}

		void process_bulk2(TPBlockQueue<T>* queue, unsigned int index) noexcept
		{
			T* tasks = containers + index * batchSize;
			unsigned int size_threshold = batchSize;
			unsigned int round_threshold = batchSize / 2;
			BulkStealer<T> stealer(&rwLock, queues, queue, queueLength, &threadNum, batchSize, enableMonitor);

			while (true)
			{
				unsigned int count;

				while (true)
				{
					while (true)
					{
						unsigned int round = 1;
						unsigned int size = queue->get_exact_size();

						while (size < size_threshold && round < round_threshold)
						{
							std::this_thread::yield();
							size = queue->get_exact_size();
							round++;
							std::this_thread::yield();
						}

						if (size && (count = queue->dequeue_bulk(tasks, size_threshold)))
							execute_tasks(tasks, count);
						else
							break;
					}

					count = stealer.steal(tasks);
					if (count)
					{
						execute_tasks(tasks, count);
					}
					else
					{
						count = queue->wait_dequeue_bulk(tasks, size_threshold, std::chrono::milliseconds(HSLL_THREADPOOL_TIMEOUT_MILLISECONDS));

						if (count)
							execute_tasks(tasks, count);
					}

					if (queue->is_Stopped())
						break;
				}

				if (shutdownPolicy)
				{
					while (count = queue->dequeue_bulk(tasks, size_threshold))
						execute_tasks(tasks, count);
				}

				stoppedSem[index].release();
				restartSem[index].acquire();

				if (exitFlag)
					break;
			}
		}

		bool initResourse(unsigned int capacity, unsigned int maxThreadNum, unsigned int batchSize)
		{
			unsigned int succeed = 0;

			if (!(restartSem = new(std::nothrow) Semaphore[2 * maxThreadNum]))
				goto clean_1;

			stoppedSem = restartSem + maxThreadNum;

			if (!(containers = (T*)ALIGNED_MALLOC(sizeof(T) * batchSize * maxThreadNum, alignof(T))))
				goto clean_2;

			if (!(queues = (TPBlockQueue<T>*)ALIGNED_MALLOC(maxThreadNum * sizeof(TPBlockQueue<T>), 64)))
				goto clean_3;

			for (unsigned i = 0; i < maxThreadNum; ++i)
			{
				new (&queues[i]) TPBlockQueue<T>();

				if (!queues[i].init(capacity))
					goto clean_4;

				succeed++;
			}

			return true;

		clean_4:

			for (unsigned i = 0; i < succeed; ++i)
				queues[i].~TPBlockQueue<T>();

		clean_3:

			ALIGNED_FREE(queues);
			queues = nullptr;

		clean_2:

			ALIGNED_FREE(containers);

		clean_1:

			delete[] restartSem;

			return false;
		}

		void rleaseResourse()
		{
			for (unsigned i = 0; i < maxThreadNum; ++i)
				queues[i].~TPBlockQueue<T>();

			ALIGNED_FREE(queues);
			ALIGNED_FREE(containers);
			delete[] restartSem;
			queues = nullptr;
			workers.clear();
			workers.shrink_to_fit();
		}
	};

	template <class T, unsigned int BATCH, INSERT_POS POS = TAIL>
	class BatchSubmitter
	{
		static_assert(is_generic_ts<T>::value, "TYPE must be a TaskStack type");
		static_assert(BATCH > 0, "BATCH > 0");
		alignas(alignof(T)) unsigned char buf[BATCH * sizeof(T)];

		T* elements;
		unsigned int size;
		unsigned int index;
		ThreadPool<T>* pool;

		bool check_and_submit()
		{
			if (size == BATCH)
				return submit() == BATCH;

			return true;
		}

	public:
		/**
		* @brief Constructs a batch submitter associated with a thread pool
		* @param pool Pointer to the thread pool for batch task submission
		*/
		BatchSubmitter(ThreadPool<T>* pool) : size(0), index(0), elements((T*)buf), pool(pool) {
			assert(pool);
		}

		/**
		 * @brief Gets current number of buffered tasks
		 * @return Number of tasks currently held in the batch buffer
		 */
		unsigned int get_size() const noexcept
		{
			return size;
		}

		/**
		 * @brief Checks if batch buffer is empty
		 * @return true if no tasks are buffered, false otherwise
		 */
		bool empty() const noexcept
		{
			return size == 0;
		}

		/**
		 * @brief Checks if batch buffer is full
		 * @return true if batch buffer has reached maximum capacity (BATCH), false otherwise
		 */
		bool full() const noexcept
		{
			return size == BATCH;
		}

		/**
		 * @brief Constructs task in-place in batch buffer
		 * @tparam Args Types of arguments for task constructor
		 * @param args Arguments forwarded to task constructor
		 * @return true if task was added to buffer or submitted successfully,
		 *         false if submission failed due to full thread pool queues
		 * @details Automatically submits batch if buffer becomes full during emplace
		 */
		template <typename... Args>
		bool emplace(Args &&...args) noexcept
		{
			if (!check_and_submit())
				return false;

			new (elements + index) T(std::forward<Args>(args)...);
			index = (index + 1) % BATCH;
			size++;
			return true;
		}

		/**
		 * @brief Adds preconstructed task to batch buffer
		 * @tparam U Deduced task type
		 * @param task Task object to buffer
		 * @return true if task was added to buffer or submitted successfully,
		 *         false if submission failed due to full thread pool queues
		 * @details Automatically submits batch if buffer becomes full during add
		 */
		template <class U>
		bool add(U&& task) noexcept
		{
			if (!check_and_submit())
				return false;

			new (elements + index) T(std::forward<U>(task));
			index = (index + 1) % BATCH;
			size++;
			return true;
		}

		/**
		 * @brief Submits all buffered tasks to thread pool
		 * @return Number of tasks successfully submitted
		 * @details Moves buffered tasks to thread pool in bulk.
		 */
		unsigned int submit() noexcept
		{
			if (!size)
				return 0;

			unsigned int start = (index - size + BATCH) % BATCH;
			unsigned int len1 = (start + size <= BATCH) ? size : (BATCH - start);
			unsigned int len2 = size - len1;
			unsigned int submitted;

			if (!len2)
			{
				if (len1 == 1)
					submitted = pool->template enqueue<POS>(std::move(*(elements + start))) ? 1 : 0;
				else
					submitted = pool->template enqueue_bulk<MOVE, POS>(elements + start, len1);
			}
			else
			{
				submitted = pool->template enqueue_bulk<MOVE, POS>(
					elements + start, len1,
					elements, len2
				);
			}

			if (submitted > 0)
			{
				if (submitted <= len1)
				{
					for (unsigned i = 0; i < submitted; ++i)
						elements[(start + i) % BATCH].~T();
				}
				else
				{
					for (unsigned i = 0; i < len1; ++i)
						elements[(start + i) % BATCH].~T();

					for (unsigned i = 0; i < submitted - len1; ++i)
						elements[i].~T();
				}

				size -= submitted;
			}

			return submitted;
		}

		~BatchSubmitter() noexcept
		{
			if (size > 0)
			{
				unsigned int start = (index - size + BATCH) % BATCH;
				unsigned int len1 = (start + size <= BATCH) ? size : (BATCH - start);
				unsigned int len2 = size - len1;

				for (unsigned int i = 0; i < len1; i++)
					elements[(start + i) % BATCH].~T();

				for (unsigned int i = 0; i < len2; i++)
					elements[i].~T();
			}
		}

		BatchSubmitter(const BatchSubmitter&) = delete;
		BatchSubmitter& operator=(const BatchSubmitter&) = delete;
		BatchSubmitter(BatchSubmitter&&) = delete;
		BatchSubmitter& operator=(BatchSubmitter&&) = delete;
	};
}

#endif