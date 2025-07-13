#ifndef HSLL_THREADPOOL
#define HSLL_THREADPOOL

#include <vector>
#include <thread>
#include "basic/TPTask.hpp"
#include "basic/TPRWLock.hpp"
#include "basic/TPSemaphore.hpp"
#include "basic/TPBlockQueue.hpp"

namespace HSLL
{

#define HSLL_THREADPOOL_TIMEOUT 5
#define HSLL_THREADPOOL_SHRINK_FACTOR 0.25
#define HSLL_THREADPOOL_EXPAND_FACTOR 0.75

	static_assert(HSLL_THREADPOOL_TIMEOUT > 0, "Invalid timeout value.");
	static_assert(HSLL_THREADPOOL_SHRINK_FACTOR < HSLL_THREADPOOL_EXPAND_FACTOR&& HSLL_THREADPOOL_EXPAND_FACTOR < 1.0
		&& HSLL_THREADPOOL_SHRINK_FACTOR>0.0, "Invalid factors.");

	template <class T>
	class SingleStealer
	{
		template <class TYPE>
		friend class ThreadPool;
	private:

		unsigned int index;
		unsigned int queueLength;
		unsigned int* threadNum;
		unsigned int threshold;

		ReadWriteLock* rwLock;
		TPBlockQueue<T>* queues;
		TPBlockQueue<T>* ignore;

		SingleStealer(ReadWriteLock* rwLock, TPBlockQueue<T>* queues, TPBlockQueue<T>* ignore,
			unsigned int queueLength, unsigned int* threadNum)
		{
			this->index = 0;
			this->queueLength = queueLength;
			this->threadNum = threadNum;
			this->threshold = std::min((unsigned int)2, queueLength);
			this->rwLock = rwLock;
			this->queues = queues;
			this->ignore = ignore;
		}

		unsigned int steal(T& element)
		{
			ReadLockGuard lock(*rwLock);
			unsigned int num = *threadNum;
			for (int i = 0; i < num; ++i)
			{
				unsigned int now = (index + i) % num;
				TPBlockQueue<T>* queue = queues + now;
				if (queue->get_size() >= threshold && queue != ignore)
				{
					if (queue->pop(element))
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

		unsigned int index;
		unsigned int batchSize;
		unsigned int queueLength;
		unsigned int* threadNum;
		unsigned int threshold;

		ReadWriteLock* rwLock;
		TPBlockQueue<T>* queues;
		TPBlockQueue<T>* ignore;

		BulkStealer(ReadWriteLock* rwLock, TPBlockQueue<T>* queues, TPBlockQueue<T>* ignore, unsigned int queueLength,
			unsigned int* threadNum, unsigned int batchSize)
		{
			this->index = 0;
			this->batchSize = batchSize;
			this->queueLength = queueLength;
			this->threadNum = threadNum;
			this->threshold = std::min(2 * batchSize, queueLength);
			this->rwLock = rwLock;
			this->queues = queues;
			this->ignore = ignore;
		}

		unsigned int steal(T* elements)
		{
			ReadLockGuard lock(*rwLock);
			unsigned int num = *threadNum;
			for (int i = 0; i < num; ++i)
			{
				unsigned int now = (index + i) % num;
				TPBlockQueue<T>* queue = queues + now;
				if (queue->get_size() >= threshold && queue != ignore)
				{
					unsigned int count = queue->popBulk(elements, batchSize);
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
	 * @tparam T Type of task objects to be processed, must implement execute() method
	 */
	template <class T = TaskStack<>>
	class ThreadPool
	{
		static_assert(is_generic_ts<T>::value, "TYPE must be a TaskStack type");

	private:

		bool shutdownPolicy;			  ///< Thread pool shutdown policy: true for graceful shutdown	
		unsigned int threadNum;			  ///< Number of worker threads/queues
		unsigned int minThreadNum;
		unsigned int maxThreadNum;
		unsigned int batchSize;
		unsigned int queueLength;		  ///< Capacity of each internal queue
		std::chrono::milliseconds adjustInterval;

		Semaphore exitSem;
		Semaphore* stopSem;
		Semaphore* startSem;
		ReadWriteLock rwLock;
		std::atomic<bool> exitFlag;

		std::thread monitor;
		TPBlockQueue<T>* queues;		  ///< Per-worker task queues
		std::vector<std::thread> workers; ///< Worker thread collection
		std::atomic<unsigned int> index;  ///< Atomic counter for round-robin task distribution to worker queues

	public:

		/**
		 * @brief Constructs an uninitialized thread pool
		 */
		ThreadPool() : queues(nullptr) {}

		/**
		* @brief Initializes thread pool resources
		* @param capacity Capacity of each internal queue
		* @param minThreadNum Minimum number of worker threads
		* @param maxThreadNum Maximum number of worker threads
		* @param batchSize Maximum tasks to process per batch (min 1)
		* @param adjustInterval Time interval for checking the load and adjusting the number of active threads
		* @return true if initialization succeeded, false otherwise
		*/
		bool init(unsigned int capacity, unsigned int minThreadNum,
			unsigned int maxThreadNum, unsigned int batchSize = 1,
			std::chrono::milliseconds adjustInterval = std::chrono::milliseconds(3000)) noexcept
		{
			if (batchSize == 0 || minThreadNum == 0 || batchSize > capacity || minThreadNum > maxThreadNum)
				return false;

			unsigned int succeed = 0;

			if (maxThreadNum > 1)
			{
				stopSem = new(std::nothrow) Semaphore[maxThreadNum];
				if (!stopSem)
					goto clean_1;

				startSem = new(std::nothrow) Semaphore[maxThreadNum];
				if (!startSem)
					goto clean_2;
			}

			queues = (TPBlockQueue<T>*)ALIGNED_MALLOC(maxThreadNum * sizeof(TPBlockQueue<T>), 64);
			if (!queues)
				goto clean_3;

			for (unsigned i = 0; i < maxThreadNum; ++i)
			{
				new (&queues[i]) TPBlockQueue<T>();

				if (!queues[i].init(capacity))
					goto clean_4;

				succeed++;
			}

			this->index = 0;
			this->exitFlag = false;
			this->shutdownPolicy = true;
			this->minThreadNum = minThreadNum;
			this->maxThreadNum = maxThreadNum;
			this->threadNum = maxThreadNum;
			this->batchSize = batchSize;
			this->queueLength = capacity;
			this->adjustInterval = adjustInterval;
			workers.reserve(maxThreadNum);

			for (unsigned i = 0; i < maxThreadNum; ++i)
				workers.emplace_back(&ThreadPool::worker, this, i);

			if (maxThreadNum > 1)
				monitor = std::thread(&ThreadPool::load_monitor, this);

			return true;

		clean_4:

			for (unsigned i = 0; i < succeed; ++i)
				queues[i].~TPBlockQueue<T>();

		clean_3:

			ALIGNED_FREE(queues);
			queues = nullptr;

		clean_2:

			if (maxThreadNum > 1)
			{
				delete[] stopSem;
				stopSem = nullptr;
			}

		clean_1:

			return false;
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
			if (maxThreadNum == 1)
				return queues->template emplace<POS>(std::forward<Args>(args)...);

			ReadLockGuard lock(rwLock);
			return select_queue().template emplace<POS>(std::forward<Args>(args)...);
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
			if (maxThreadNum == 1)
				return queues->template wait_emplace<POS>(std::forward<Args>(args)...);

			ReadLockGuard lock(rwLock);
			return select_queue().template wait_emplace<POS>(std::forward<Args>(args)...);
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
			if (maxThreadNum == 1)
				return queues->template wait_emplace<POS>(timeout, std::forward<Args>(args)...);

			ReadLockGuard lock(rwLock);
			return select_queue().template wait_emplace<POS>(timeout, std::forward<Args>(args)...);
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
			if (maxThreadNum == 1)
				return queues->template push<POS>(std::forward<U>(task));

			ReadLockGuard lock(rwLock);
			return select_queue().template push<POS>(std::forward<U>(task));
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
			if (maxThreadNum == 1)
				return queues->template wait_push<POS>(std::forward<U>(task));

			ReadLockGuard lock(rwLock);
			return select_queue().template wait_push<POS>(std::forward<U>(task));
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
			if (maxThreadNum == 1)
				return queues->template wait_push<POS>(std::forward<U>(task), timeout);

			ReadLockGuard lock(rwLock);
			return select_queue().template wait_push<POS>(std::forward<U>(task), timeout);
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
			if (maxThreadNum == 1)
				return queues->template pushBulk<METHOD, POS>(tasks, count);

			ReadLockGuard lock(rwLock);
			return select_queue_for_bulk(std::max(1u, count / 2)).template pushBulk<METHOD, POS>(tasks, count);
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
			if (maxThreadNum == 1)
				return queues->template pushBulk<METHOD, POS>(part1, count1, part2, count2);

			ReadLockGuard lock(rwLock);
			return select_queue_for_bulk(std::max(1u, (count1 + count2) / 2)).template pushBulk<METHOD, POS>(part1, count1, part2, count2);
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
			if (maxThreadNum == 1)
				return queues->template wait_pushBulk<METHOD, POS>(tasks, count);

			ReadLockGuard lock(rwLock);
			return select_queue().template wait_pushBulk<METHOD, POS>(tasks, count);
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
			if (maxThreadNum == 1)
				return queues->template wait_pushBulk<METHOD, POS>(tasks, count, timeout);

			ReadLockGuard lock(rwLock);
			return select_queue().template wait_pushBulk<METHOD, POS>(tasks, count, timeout);
		}

		//Get the maximum occupied space of the thread pool.
		unsigned long long get_max_usage()
		{
			return  maxThreadNum * queues->get_bsize();
		}

		/**
		 * @brief Waits until all task queues are empty (all tasks have been taken from queues)
		 * @note This does NOT guarantee all tasks have completed execution - it only ensures:
		 *       1. All tasks have been dequeued by worker threads
		 *       2. May return while some tasks are still being processed by workers
		 * @details Continuously checks all active queues until they're empty.
		 *          Uses yield() between checks to avoid busy waiting.
		 */
		void join()
		{
			while (true)
			{
				bool flag = true;

				for (int i = 0; i < maxThreadNum; ++i)
				{
					if (queues[i].get_exact_size())
					{
						flag = false;
						break;
					}
				}

				if (flag)
					return;
				else
					std::this_thread::yield();
			}
		}

		/**
		 * @brief Waits until all task queues are empty (all tasks dequeued) or sleeps for specified intervals between checks.
		 * @note This does NOT guarantee all tasks have completed execution - it only ensures:
		 *       1. All tasks have been dequeued by worker threads
		 *       2. May return while some tasks are still being processed by workers
		 * @tparam Rep Arithmetic type representing tick count
		 * @tparam Period Type representing tick period
		 * @param interval Sleep duration between queue checks. Smaller values increase responsiveness
		 *                 but may use more CPU, larger values reduce CPU load but delay detection.
		 */
		template <class Rep, class Period>
		void join(const std::chrono::duration<Rep, Period>& interval)
		{
			while (true)
			{
				bool flag = true;
				for (int i = 0; i < maxThreadNum; ++i)
				{
					if (queues[i].get_exact_size())
					{
						flag = false;
						break;
					}
				}

				if (flag)
					return;
				else
					std::this_thread::sleep_for(interval);
			}
		}

		/**
		 * @brief Stops all workers and releases resources
		 * @param shutdownPolicy true for graceful shutdown (waiting for tasks to complete), false for immediate shutdown
		 */
		void exit(bool shutdownPolicy = true) noexcept
		{
			if (queues)
			{
				if (maxThreadNum > 1)
				{
					exitFlag = true;
					exitSem.release();
					monitor.join();

					for (unsigned i = 0; i < workers.size(); ++i)
						startSem[i].release();
				}

				this->shutdownPolicy = shutdownPolicy;

				for (unsigned i = 0; i < workers.size(); ++i)
					queues[i].stopWait();

				for (auto& worker : workers)
					worker.join();

				workers.clear();
				workers.shrink_to_fit();

				if (maxThreadNum > 1)
				{
					delete[] stopSem;
					delete[] startSem;
				}

				for (unsigned i = 0; i < maxThreadNum; ++i)
					queues[i].~TPBlockQueue<T>();

				ALIGNED_FREE(queues);
				queues = nullptr;
			}
		}

		~ThreadPool() noexcept
		{
			exit(false);
		}

		ThreadPool(const ThreadPool&) = delete;
		ThreadPool& operator=(const ThreadPool&) = delete;
		ThreadPool(ThreadPool&&) = delete;
		ThreadPool& operator=(ThreadPool&&) = delete;

	private:

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

		static inline void execute_tasks(T* tasks, unsigned int count)
		{
			for (unsigned int i = 0; i < count; ++i)
			{
				tasks[i].execute();
				tasks[i].~T();
			}
		}

		void load_monitor() noexcept
		{
			while (true)
			{
				if (exitSem.try_acquire_for(adjustInterval))
					return;

				unsigned int allSize = queueLength * threadNum;
				unsigned int totalSize = 0;

				for (int i = 0; i < threadNum; ++i)
					totalSize += queues[i].get_exact_size();

				if (totalSize < allSize * HSLL_THREADPOOL_SHRINK_FACTOR && threadNum > minThreadNum)
				{
					rwLock.lock_write();
					threadNum--;
					rwLock.unlock_write();
					queues[threadNum].stopWait();
					stopSem[threadNum].acquire();
					queues[threadNum].release();
				}
				else if (totalSize > allSize * HSLL_THREADPOOL_EXPAND_FACTOR && threadNum < maxThreadNum)
				{
					unsigned int newThreads = std::max(1u, (maxThreadNum - threadNum) / 2);
					unsigned int succeed = 0;
					for (int i = threadNum; i < threadNum + newThreads; ++i)
					{
						if (!queues[i].init(queueLength))
							break;

						startSem[i].release();
						succeed++;
					}

					if (succeed > 0)
					{
						rwLock.lock_write();
						threadNum += succeed;
						rwLock.unlock_write();
					}
				}
			}
		}

		void worker(unsigned int index) noexcept
		{
			if (batchSize == 1)
				process_single(queues + index, index);
			else
				process_bulk(queues + index, index, batchSize);
		}

		void process_single(TPBlockQueue<T>* queue, unsigned int index) noexcept
		{
			alignas(alignof(T)) char storage[sizeof(T)];
			T* task = (T*)(&storage);

			if (maxThreadNum == 1)
			{
				while (true)
				{
					if (queue->wait_pop(*task))
					{
						task->execute();
						task->~T();
					}
					else
					{
						break;
					}
				}

				while (shutdownPolicy && queue->pop(*task))
				{
					task->execute();
					task->~T();
				}
				return;
			}

			SingleStealer<T> stealer(&rwLock, queues, queue, queueLength, &threadNum);

			while (true)
			{
				while (true)
				{
					while (queue->pop(*task))
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
						if (queue->wait_pop(*task, std::chrono::milliseconds(HSLL_THREADPOOL_TIMEOUT)))
						{
							task->execute();
							task->~T();
						}
					}

					if (queue->is_Stopped())
						break;
				}

				while (shutdownPolicy && queue->pop(*task))
				{
					task->execute();
					task->~T();
				}

				stopSem[index].release();
				startSem[index].acquire();

				if (exitFlag)
					break;
			}
		}

		void process_bulk(TPBlockQueue<T>* queue, unsigned int index, unsigned batchSize) noexcept
		{
			T* tasks;
			unsigned int count;

			if (!(tasks= (T*)ALIGNED_MALLOC(sizeof(T) * batchSize, alignof(T))))
				std::abort();

			if (maxThreadNum == 1)
			{
				while (true)
				{
					while (true)
					{
						unsigned int round = 1;
						unsigned int size = queue->get_exact_size();
						while (size < batchSize && round < batchSize / 2)
						{
							std::this_thread::yield();
							size = queue->get_exact_size();
							round++;
							std::this_thread::yield();
						}

						if (size && (count = queue->popBulk(tasks, batchSize)))
							execute_tasks(tasks, count);
						else
							break;
					}

					count = queue->wait_popBulk(tasks, batchSize);

					if (count)
						execute_tasks(tasks, count);
					else
						break;
				}

				while (shutdownPolicy && (count = queue->popBulk(tasks, batchSize)))
					execute_tasks(tasks, count);

				ALIGNED_FREE(tasks);
				return;
			}

			BulkStealer<T> stealer(&rwLock, queues, queue, queueLength, &threadNum, batchSize);

			while (true)
			{
				while (true)
				{
					while (true)
					{
						unsigned int round = 1;
						unsigned int size = queue->get_exact_size();
						while (size < batchSize && round < batchSize / 2)
						{
							std::this_thread::yield();
							size = queue->get_exact_size();
							round++;
							std::this_thread::yield();
						}

						if (size && (count = queue->popBulk(tasks, batchSize)))
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
						count = queue->wait_popBulk(tasks, batchSize, std::chrono::milliseconds(HSLL_THREADPOOL_TIMEOUT));

						if (count)
							execute_tasks(tasks, count);
					}

					if (queue->is_Stopped())
						break;
				}

				while (shutdownPolicy && (count = queue->popBulk(tasks, batchSize)))
					execute_tasks(tasks, count);

				stopSem[index].release();
				startSem[index].acquire();

				if (exitFlag)
					break;
			}

			ALIGNED_FREE(tasks);
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
			if (size == 0)
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