#ifndef HSLL_THREADPOOL
#define HSLL_THREADPOOL

#include <vector>
#include "basic/TPTask.hpp"
#include "basic/TPRWLock.hpp"
#include "basic/TPSemaphore.hpp"
#include "basic/TPBlockQueue.hpp"

namespace HSLL
{

#define HSLL_THREADPOOL_TIMEOUT_Q 5
#define HSLL_THREADPOOL_TIMEOUT_M std::chrono::seconds(1)

	/**
	 * @brief Thread pool implementation with multiple queues for task distribution
	 * @tparam T Type of task objects to be processed, must implement execute() method
	 */
	template <class T = TaskStack<>>
	class ThreadPool
	{
	private:

		bool shutdownPolicy;			  ///< Thread pool shutdown policy: true for graceful shutdown	
		unsigned int threadNum;			  ///< Number of worker threads/queues to create
		unsigned int minThreadNum;
		unsigned int maxThreadNum;
		unsigned int batchSize;
		unsigned int queueLength;		  ///< Capacity of each internal queue
		std::atomic<bool>  exitFlag;

		Semaphore exitSem;
		Semaphore* stopSem;
		Semaphore* startSem;
		std::thread monitor;
		std::vector<std::thread> workers; ///< Worker thread collection

		ReadWriteLock rwLock;
		TPBlockQueue<T>* queues;		  ///< Per-worker task queues
		std::atomic<unsigned int> index;  ///< Atomic counter for round-robin task distribution to worker queues


		static struct SingleStealer
		{
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

		static struct BulkStealer
		{
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

	public:

		/**
		 * @brief Constructs an uninitialized thread pool
		 */
		ThreadPool() : queues(nullptr), stopSem(nullptr), startSem(nullptr), threadNum(0), index(0),
			queueLength(0), exitFlag(false), shutdownPolicy(true) {}

		/**
		* @brief Initializes thread pool resources
		* @param queueLength Capacity of each internal queue
		* @param minThreadNum Minimum number of worker threads
		* @param maxThreadNum Maximum number of worker threads
		* @param batchSize Maximum tasks to process per batch (min 1)
		* @return true if initialization succeeded, false otherwise
		*/
		bool init(unsigned int queueLength, unsigned int minThreadNum,
			unsigned int maxThreadNum, unsigned int batchSize = 1) noexcept
		{
			unsigned int succeed = 0;

			if (batchSize == 0 || minThreadNum == 0 || batchSize > queueLength || minThreadNum > maxThreadNum)
				goto clean_1;

			stopSem = new(std::nothrow) Semaphore[maxThreadNum];
			if (!stopSem)
				goto clean_1;

			startSem = new(std::nothrow) Semaphore[maxThreadNum];
			if (!startSem)
				goto clean_2;

			queues = (TPBlockQueue<T>*)ALIGNED_MALLOC(maxThreadNum * sizeof(TPBlockQueue<T>), 64);
			if (!queues)
				goto clean_3;

			for (unsigned i = 0; i < maxThreadNum; ++i)
			{
				new (&queues[i]) TPBlockQueue<T>();

				if (!queues[i].init(queueLength))
					goto clean_4;

				succeed++;
			}

			this->minThreadNum = minThreadNum;
			this->maxThreadNum = maxThreadNum;
			this->threadNum = maxThreadNum;
			this->batchSize = batchSize;
			this->queueLength = queueLength;
			workers.reserve(maxThreadNum);

			for (unsigned i = 0; i < maxThreadNum; ++i)
				workers.emplace_back(&ThreadPool::worker, this, i);

			monitor = std::thread(&ThreadPool::load_monitor, this);

			return true;

		clean_4:

			for (unsigned i = 0; i < succeed; ++i)
				queues[i].~TPBlockQueue<T>();

		clean_3:

			ALIGNED_FREE(queues);
			queues = nullptr;

		clean_2:

			delete[] stopSem;
			stopSem = nullptr;

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
			ReadLockGuard lock(rwLock);
			return select_queue_for_bulk(count / 2).template pushBulk<METHOD, POS>(tasks, count);
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
			ReadLockGuard lock(rwLock);
			return select_queue().template wait_pushBulk<METHOD, POS>(tasks, count, timeout);
		}

		/**
		 * @brief Stops all workers and releases resources
		 * @param shutdownPolicy true for graceful shutdown (waiting for tasks to complete), false for immediate shutdown
		 */
		void exit(bool shutdownPolicy = true) noexcept
		{
			if (queues)
			{
				exitFlag = true;
				exitSem.release();
				monitor.join();
				this->shutdownPolicy = shutdownPolicy;

				for (unsigned i = 0; i < workers.size(); ++i)
				{
					startSem[i].release();
					queues[i].stopWait();
				}

				for (auto& worker : workers)
					worker.join();

				workers.clear();
				workers.shrink_to_fit();

				for (unsigned i = 0; i < maxThreadNum; ++i)
					queues[i].~TPBlockQueue<T>();

				ALIGNED_FREE(queues);
				delete[] stopSem;
				delete[] startSem;
				queues = nullptr;
				stopSem = nullptr;
				startSem = nullptr;
				index = 0;
				threadNum = 0;
				queueLength = 0;
				exitFlag = false;
				shutdownPolicy = true;
			}
		}

		~ThreadPool() noexcept
		{
			exit(false);
		}

		ThreadPool(const ThreadPool&) = delete;
		ThreadPool& operator=(const ThreadPool&) = delete;

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

		void load_monitor() noexcept
		{
			while (true)
			{
				if (exitSem.try_acquire_for(HSLL_THREADPOOL_TIMEOUT_M))
					return;

				unsigned int allSize = queueLength * threadNum;
				unsigned int totalSize = 0;

				for (int i = 0; i < threadNum; ++i)
					totalSize += queues[i].get_exact_size();

				if (totalSize < allSize * 0.25 && threadNum > minThreadNum)
				{
					rwLock.lock_write();
					threadNum--;
					rwLock.unlock_write();
					queues[threadNum].stopWait();
					stopSem[threadNum].acquire();
					queues[threadNum].release();
				}
				else if (totalSize > allSize * 0.75 && threadNum < maxThreadNum)
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

			SingleStealer stealer(&rwLock, queues, queue, queueLength, &threadNum);

			while (true)
			{
				while (true)
				{
					while (queue->get_exact_size() && queue->pop(*task))
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
						if (queue->wait_pop(*task, std::chrono::milliseconds(HSLL_THREADPOOL_TIMEOUT_Q)))
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

		static inline void execute_tasks(T* tasks, unsigned int count)
		{
			for (unsigned int i = 0; i < count; ++i)
			{
				tasks[i].execute();
				tasks[i].~T();
			}
		}

		void process_bulk(TPBlockQueue<T>* queue, unsigned int index, unsigned batchSize) noexcept
		{
			T* tasks = (T*)ALIGNED_MALLOC(sizeof(T) * batchSize, alignof(T));

			if (!tasks)
			{
				process_single(queue, index);
				return;
			}

			unsigned int count;
			BulkStealer stealer(&rwLock, queues, queue, queueLength, &threadNum, batchSize);

			while (true)
			{
				while (true)
				{
					while (queue->get_exact_size() && (count = queue->popBulk(tasks, batchSize)))
						execute_tasks(tasks, count);

					count = stealer.steal(tasks);
					if (count)
					{
						execute_tasks(tasks, count);
					}
					else
					{
						count = queue->wait_popBulk(tasks, batchSize, std::chrono::milliseconds(HSLL_THREADPOOL_TIMEOUT_Q));

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
}

#endif