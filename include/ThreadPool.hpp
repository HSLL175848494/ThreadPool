#ifndef HSLL_THREADPOOL
#define HSLL_THREADPOOL

#include "basic/TPSRWLock.hpp"
#include "basic/TPTaskStack.hpp"
#include "basic/TPSemaphore.hpp"
#include "basic/TPGroupAllocator.hpp"

namespace HSLL
{
	namespace INNER
	{
		constexpr unsigned int HSLL_THREADPOOL_LOCK_TIMEOUT_MILLISECONDS = 50u;
		constexpr unsigned int HSLL_THREADPOOL_DEQUEUE_TIMEOUT_MILLISECONDS = 1u;

		static_assert(HSLL_THREADPOOL_LOCK_TIMEOUT_MILLISECONDS > 0, "Invalid lock timeout value.");
		static_assert(HSLL_THREADPOOL_DEQUEUE_TIMEOUT_MILLISECONDS > 0, "Invalid dequeue timeout value.");

		template <typename T>
		class SingleStealer
		{
			template <typename TYPE>
			friend class ThreadPool;
		private:

			unsigned int index;
			unsigned int capacity;
			unsigned int threshold;
			unsigned int threadNum;

			TPBlockQueue<T>* queues;
			TPBlockQueue<T>* ignore;

			SingleStealer(TPBlockQueue<T>* queues, TPBlockQueue<T>* ignore,
				unsigned int capacity, unsigned int threadNum)
			{
				this->index = 0;
				this->capacity = capacity;
				this->threadNum = threadNum;
				this->threshold = std::min(capacity / 2, threadNum / 2);
				this->queues = queues;
				this->ignore = ignore;
			}

			unsigned int steal(T& element)
			{
				return steal_inner(element);
			}

			bool steal_inner(T& element)
			{
				unsigned int num = threadNum;

				for (unsigned int i = 0; i < num; ++i)
				{
					unsigned int now = (index + i) % num;
					TPBlockQueue<T>* queue = queues + now;

					if (queue != ignore && queue->get_size_weak() >= threshold)
					{
						if (queue->dequeue(element))
						{
							index = now;
							return true;
						}
					}
				}
				return false;
			}
		};

		template <typename T>
		class BulkStealer
		{
			template <typename TYPE>
			friend class ThreadPool;

		private:

			unsigned int index;
			unsigned int capacity;
			unsigned int batchSize;
			unsigned int threshold;
			unsigned int threadNum;

			TPBlockQueue<T>* queues;
			TPBlockQueue<T>* ignore;

			BulkStealer(TPBlockQueue<T>* queues, TPBlockQueue<T>* ignore, unsigned int capacity,
				unsigned int threadNum, unsigned int batchSize)
			{
				this->index = 0;
				this->batchSize = batchSize;
				this->capacity = capacity;
				this->threadNum = threadNum;
				this->threshold = std::min(capacity / 2, batchSize * threadNum / 2);
				this->queues = queues;
				this->ignore = ignore;
			}

			unsigned int steal(T* elements)
			{
				return steal_inner(elements);
			}

			unsigned int steal_inner(T* elements)
			{
				unsigned int count;
				unsigned int num = threadNum;

				for (unsigned int i = 0; i < num; ++i)
				{
					unsigned int now = (index + i) % num;
					TPBlockQueue<T>* queue = queues + now;

					if (queue != ignore && queue->get_size_weak() >= threshold)
					{
						if (count = queue->dequeue_bulk(elements, batchSize))
						{
							if (count == batchSize)
								index = now;
							else
								index = (now + 1) % num;

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
		template <typename T = TaskStack<>>
		class ThreadPool
		{
			static_assert(is_TaskStack<T>::value, "TYPE must be a TaskStack type");

			template <typename TYPE, unsigned int, INSERT_POS POS>
			friend class BatchSubmitter;

		private:

			unsigned int capacity;
			unsigned int batchSize;
			unsigned int threadNum;
			unsigned int mainFullThreshold;
			unsigned int otherFullThreshold;

			T* containers;
			Semaphore* stoppedSem;
			Semaphore* restartSem;
			SpinReadWriteLock rwLock;
			std::atomic<bool> shutdownFlag;
			std::atomic<bool> gracefulShutdown;

			TPBlockQueue<T>* queues;
			std::atomic<unsigned int> index;
			std::vector<std::thread> workers;
			TPGroupAllocator<T> groupAllocator;

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
			bool init(unsigned int capacity, unsigned int threadNum, unsigned int batchSize = 1) noexcept
			{
				assert(!queues);

				if (!batchSize || !threadNum || capacity < 2)
					return false;

				if (!initResourse(capacity, threadNum, batchSize))
					return false;

				this->capacity = capacity;
				this->batchSize = std::min(batchSize, capacity / 2);
				this->threadNum = threadNum;
				this->mainFullThreshold = std::max(2u, (unsigned int)(capacity * HSLL_QUEUE_FULL_FACTOR_MAIN));
				this->otherFullThreshold = std::max(2u, (unsigned int)(capacity * HSLL_QUEUE_FULL_FACTOR_OTHER));
				this->shutdownFlag = false;
				this->gracefulShutdown = true;
				this->index = 0;

				workers.reserve(threadNum);
				groupAllocator.initialize(queues, threadNum, capacity, (unsigned int)(capacity * 0.05 > 1 ? capacity * 0.05 : 1));

				for (unsigned i = 0; i < threadNum; ++i)
					workers.emplace_back(&ThreadPool::worker, this, i);

				return true;
			}

#define HSLL_ENQUEUE_HELPER(exp1,exp2)							\
																\
		assert(queues);											\
																\
		if (threadNum == 1)										\
			return exp1;										\
																\
		ReadLockGuard lock(rwLock);								\
																\
		unsigned int size;										\
		TPBlockQueue<T>* queue;									\
		std::thread::id id = std::this_thread::get_id();		\
		RoundRobinGroup<T>* group = groupAllocator.find(id);	\
																\
		if(!group)												\
		{														\
			queue = select_queue();								\
																\
			if (queue)											\
				return exp2;									\
																\
			return 0;											\
		}														\
																\
		queue = group->current_queue();							\
																\
		if (queue)												\
			size = exp2;										\
		else													\
			size = 0;											\
																\
		if (size)												\
		{														\
			group->record(size);								\
			return size;										\
		}														\
		else													\
		{														\
			if ((queue = group->available_queue()))				\
			{													\
				size = exp2;									\
				group->record(size);							\
			}													\
			else												\
			{													\
				queue = groupAllocator.available_queue(group);	\
																\
				if(queue)										\
				return exp2;									\
			}													\
		}														\
																\
		return size;											

			/**
			 * @brief Non-blocking task submission to the thread pool
			 * @tparam POS Insertion position (HEAD or TAIL, default: TAIL)
			 * @param args Arguments forwarded to task constructor
			 * @return true if task was successfully added, false otherwise
			 * @note
			 * Supports two argument structures:
			 * 1. TaskStack object (must be passed by rvalue reference, using move semantics)
			 * 2. Callable object (function pointer/lambda/functor...) + bound arguments
			 */
			template <INSERT_POS POS = TAIL, typename... Args>
			bool submit(Args &&...args) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(queues-> template emplace<POS>(std::forward<Args>(args)...)),
					(queue-> template emplace<POS>(std::forward<Args>(args)...))
				)
			}

			/**
			 * @brief Blocking task submission with indefinite wait
			 * @tparam POS Insertion position (HEAD or TAIL, default: TAIL)
			 * @param args Arguments forwarded to task constructor
			 * @return true if task was added successfully, false if thread pool was stopped
			 * @note
			 * Supports two argument structures:
			 * 1. TaskStack object (must be passed by rvalue reference, using move semantics)
			 * 2. Callable object (function pointer/lambda/functor...) + bound arguments
			 */
			template <INSERT_POS POS = TAIL, typename... Args>
			bool wait_submit(Args &&...args) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(queues-> template wait_emplace<POS>(std::forward<Args>(args)...)),
					(queue-> template wait_emplace<POS>(std::forward<Args>(args)...))
				)
			}

			/**
			 * @brief Blocking task submission with timeout
			 * @tparam POS Insertion position (HEAD or TAIL, default: TAIL)
			 * @param timeout Maximum duration to wait for space
			 * @param args Arguments forwarded to task constructor
			 * @return true if task was added successfully, false on timeout or thread pool stop
			 * @note
			 * Supports two argument structures:
			 * 1. TaskStack object (must be passed by rvalue reference, using move semantics)
			 * 2. Callable object (function pointer/lambda/functor...) + bound arguments
			 */
			template <INSERT_POS POS = TAIL, typename Rep, typename Period, typename... Args>
			bool wait_submit_for(const std::chrono::duration<Rep, Period>& timeout, Args &&...args) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(queues-> template wait_emplace_for<POS>(timeout, std::forward<Args>(args)...)),
					(queue-> template wait_emplace_for<POS>(timeout, std::forward<Args>(args)...))
				)
			}

			/**
			 * @brief Blocking task submission with absolute timeout
			 * @tparam POS Insertion position (HEAD or TAIL, default: TAIL)
			 * @param abs Absolute timeout point
			 * @param args Arguments forwarded to task constructor
			 * @return true if task was added successfully, false on timeout or thread pool stop
			 * @note
			 * Supports two argument structures:
			 * 1. TaskStack object (must be passed by rvalue reference, using move semantics)
			 * 2. Callable object (function pointer/lambda/functor...) + bound arguments
			 */
			template <INSERT_POS POS = TAIL, typename Clock, typename Duration, typename... Args>
			bool wait_submit_until(const std::chrono::time_point<Clock, Duration>& abs, Args &&...args)
			{
				HSLL_ENQUEUE_HELPER(
					(queues-> template wait_emplace_until<POS>(abs, std::forward<Args>(args)...)),
					(queue-> template wait_emplace_until<POS>(abs, std::forward<Args>(args)...))
				)
			}

			/**
			 * @brief Non-blocking bulk task submission (using move semantics)
			 * @tparam POS Insertion position (HEAD or TAIL, default: TAIL)
			 * @param tasks Array of tasks to enqueue
			 * @param count Number of tasks in array (must be > 0)
			 * @return Actual number of tasks added (may be less than count)
			 */
			template <INSERT_POS POS = TAIL>
			unsigned int submit_bulk(T* tasks, unsigned int count) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(queues-> template enqueue_bulk<POS>(tasks, count)),
					(queue-> template enqueue_bulk<POS>(tasks, count))
				)
			}

			/**
			 * @brief Blocking bulk submission with indefinite wait (using move semantics)
			 * @tparam POS Insertion position (HEAD or TAIL, default: TAIL)
			 * @param tasks Array of tasks to add
			 * @param count Number of tasks to add (must be > 0)
			 * @return Actual number of tasks added (may be less than count)
			 */
			template <INSERT_POS POS = TAIL>
			unsigned int wait_submit_bulk(T* tasks, unsigned int count) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(queues-> template wait_enqueue_bulk<POS>(tasks, count)),
					(queue-> template wait_enqueue_bulk<POS>(tasks, count))
				)
			}

			/**
			 * @brief Blocking bulk submission with timeout (using move semantics)
			 * @tparam POS Insertion position (HEAD or TAIL, default: TAIL)
			 * @param timeout Maximum duration to wait for space
			 * @param tasks Array of tasks to add
			 * @param count Number of tasks to add (must be > 0)
			 * @return Actual number of tasks added (may be less than count)
			 */
			template <INSERT_POS POS = TAIL, typename Rep, typename Period>
			unsigned int wait_submit_bulk_for(const std::chrono::duration<Rep, Period>& timeout, T* tasks, unsigned int count) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(queues-> template wait_enqueue_bulk_for<POS>(timeout, tasks, count)),
					(queue-> template wait_enqueue_bulk_for<POS>(timeout, tasks, count))
				)
			}

			/**
			 * @brief Blocking bulk submission with absolute timeout (using move semantics)
			 * @tparam POS Insertion position (HEAD or TAIL, default: TAIL)
			 * @param abs Absolute timeout point
			 * @param tasks Array of tasks to add
			 * @param count Number of tasks to add (must be > 0)
			 * @return Actual number of tasks added (may be less than count)
			 */
			template <INSERT_POS POS = TAIL, typename Clock, typename Duration>
			unsigned int wait_submit_bulk_until(const std::chrono::time_point<Clock, Duration>& abs, T* tasks, unsigned int count)
			{
				HSLL_ENQUEUE_HELPER(
					(queues-> template wait_enqueue_bulk_until<POS>(abs, tasks, count)),
					(queue-> template wait_enqueue_bulk_until<POS>(abs, tasks, count))
				)
			}

			/**
			 * @brief Waits for all tasks to complete.
			 * @note
			 *  1. During the join operation, adding any new tasks is prohibited.
			 *  2. This function is not thread-safe.
			 *	3. This function does not clean up resources. After the call, the queue can be used normally.
			 */
			void drain() noexcept
			{
				assert(queues);

				for (unsigned int i = 0; i < threadNum; ++i)
				{
					restartSem[i].release();
					queues[i].disableWait();
				}

				for (unsigned int i = 0; i < threadNum; ++i)
				{
					stoppedSem[i].acquire();
					queues[i].enableWait();
				}
			}

			/**
			 * @brief Stops all workers and releases resources.
			 * @param graceful If true, performs a graceful shutdown (waits for tasks to complete);
			 *                if false, forces an immediate shutdown.
			 * @note This function is not thread-safe.
			 * @note After calling this function, the thread pool can be reused by calling init again.
			 */
			void shutdown(bool graceful = true) noexcept
			{
				assert(queues);

				this->shutdownFlag = true;
				this->gracefulShutdown = graceful;

				{
					for (unsigned i = 0; i < workers.size(); ++i)
						restartSem[i].release();

					for (unsigned i = 0; i < threadNum; ++i)
						queues[i].disableWait();

					for (auto& worker : workers)
						worker.join();
				}

				releaseResourse();
			}

			/**
			 * @brief Registers the current thread. Registered threads participate in queue grouping and obtain a dedicated queue group.
			 * @note
			 *   1. The registered thread must be a producer thread
			 *   2. Production capacity between registered threads should not vary significantly
			 *   3. If the thread pool will continue to be used after this thread exits, you MUST unregister
			 *      the thread before exit to allow queue reallocation
			 */
			void register_this_thread() noexcept
			{
				assert(queues);
				std::thread::id id = std::this_thread::get_id();
				WriteLockGuard lock(rwLock);
				groupAllocator.register_thread(id);
			}

			/**
			 * @brief Unregisters the current thread. It will no longer participate in queue grouping.
			 */
			void unregister_this_thread() noexcept
			{
				assert(queues);
				std::thread::id id = std::this_thread::get_id();
				WriteLockGuard lock(rwLock);
				groupAllocator.unregister_thread(id);
			}

			~ThreadPool() noexcept
			{
				if (queues)
					shutdown(false);
			}

			ThreadPool(const ThreadPool&) = delete;
			ThreadPool& operator=(const ThreadPool&) = delete;
			ThreadPool(ThreadPool&&) = delete;
			ThreadPool& operator=(ThreadPool&&) = delete;

		private:

			template <INSERT_POS POS = TAIL>
			unsigned int submit_bulk(T* part1, unsigned int count1, T* part2, unsigned int count2) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(queues-> template enqueue_bulk<POS>(part1, count1, part2, count2)),
					(queue-> template enqueue_bulk<POS>(part1, count1, part2, count2))
				)
			}

			unsigned int next_index() noexcept
			{
				return index.fetch_add(1, std::memory_order_relaxed) % threadNum;
			}

			TPBlockQueue<T>* select_queue() noexcept
			{
				unsigned int now = next_index();
				TPBlockQueue<T>* queue = queues + now;

				if (queue->get_size_weak() <= mainFullThreshold)
					return queue;

				for (unsigned int i = 1; i <= threadNum - 1; ++i)
				{
					queue = queues + ((now + i) % threadNum);

					if (queue->get_size_weak() <= otherFullThreshold)
						return queue;
				}

				return nullptr;
			}

			void worker(unsigned int index) noexcept
			{
				if (batchSize == 1)
					process_single(queues + index, index);
				else
					process_bulk(queues + index, index);
			}

			static void execute_tasks(T* tasks, unsigned int count)
			{
				for (unsigned int i = 0; i < count; ++i)
				{
					tasks[i].execute();
					tasks[i].~T();
				}
			}

			void process_single(TPBlockQueue<T>* queue, unsigned int index) noexcept
			{
				bool enableSteal = (threadNum != 1);
				T* task = containers + index * batchSize;
				SingleStealer<T> stealer(queues, queue, capacity, threadNum);

				while (true)
				{
					while (true)
					{
						while (queue->dequeue(*task))
						{
							task->execute();
							task->~T();
						}

						if (enableSteal && stealer.steal(*task))
						{
							task->execute();
							task->~T();
							goto cheak;
						}

						if (queue->wait_dequeue_for(std::chrono::milliseconds(HSLL_THREADPOOL_DEQUEUE_TIMEOUT_MILLISECONDS), *task))
						{
							task->execute();
							task->~T();
						}

					cheak:

						if (queue->is_stopped_weak())
							break;
					}

					if (gracefulShutdown)
					{
						while (queue->dequeue(*task))
						{
							task->execute();
							task->~T();
						}
					}

					stoppedSem[index].release();
					restartSem[index].acquire();

					if (shutdownFlag)
						break;
				}
			}

			void process_bulk(TPBlockQueue<T>* queue, unsigned int index) noexcept
			{
				bool enableSteal = (threadNum != 1);
				T* tasks = containers + index * batchSize;
				BulkStealer<T> stealer(queues, queue, capacity, threadNum, batchSize);

				while (true)
				{
					unsigned int count;

					while (true)
					{
						while (true)
						{
							unsigned int size = queue->get_size_weak();
							unsigned int round = batchSize;

							while (round && size < batchSize)
							{
								std::this_thread::yield();
								size = queue->get_size_weak();
								round--;
							}

							if (size && (count = queue->dequeue_bulk(tasks, batchSize)))
								execute_tasks(tasks, count);
							else
								break;
						}

						if (enableSteal && (count = stealer.steal(tasks)))
						{
							execute_tasks(tasks, count);
							goto cheak;
						}

						if (count = queue->wait_dequeue_bulk_for(std::chrono::milliseconds(HSLL_THREADPOOL_DEQUEUE_TIMEOUT_MILLISECONDS),
							tasks, batchSize))
							execute_tasks(tasks, count);

					cheak:

						if (queue->is_stopped_weak())
							break;
					}

					if (gracefulShutdown)
					{
						while (count = queue->dequeue_bulk(tasks, batchSize))
							execute_tasks(tasks, count);
					}

					stoppedSem[index].release();
					restartSem[index].acquire();

					if (shutdownFlag)
						break;
				}
			}

			bool initResourse(unsigned int capacity, unsigned int maxThreadNum, unsigned int batchSize) noexcept
			{
				unsigned int succeed = 0;

				if (!(restartSem = new(std::nothrow) Semaphore[2 * maxThreadNum]))
					goto clean_1;

				stoppedSem = restartSem + maxThreadNum;

				if (!(containers = (T*)HSLL_ALIGNED_MALLOC(sizeof(T) * batchSize * maxThreadNum, alignof(T))))
					goto clean_2;

				if (!(queues = (TPBlockQueue<T>*)HSLL_ALIGNED_MALLOC(maxThreadNum * sizeof(TPBlockQueue<T>), alignof(TPBlockQueue<T>))))
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

				HSLL_ALIGNED_FREE(queues);
				queues = nullptr;

			clean_2:

				HSLL_ALIGNED_FREE(containers);

			clean_1:

				delete[] restartSem;

				return false;
			}

			void releaseResourse() noexcept
			{
				for (unsigned i = 0; i < threadNum; ++i)
					queues[i].~TPBlockQueue<T>();

				HSLL_ALIGNED_FREE(queues);
				HSLL_ALIGNED_FREE(containers);
				delete[] restartSem;
				queues = nullptr;
				workers.clear();
				workers.shrink_to_fit();
				groupAllocator.reset();
			}
		};

		template <typename T, unsigned int BATCH, INSERT_POS POS = TAIL>
		class BatchSubmitter
		{
			static_assert(is_TaskStack<T>::value, "T must be a TaskStack type");
			static_assert(BATCH > 0, "BATCH > 0");
			alignas(alignof(T)) unsigned char buf[BATCH * sizeof(T)];

			T* elements;
			unsigned int size;
			unsigned int index;
			ThreadPool<T>& pool;

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
			BatchSubmitter(ThreadPool<T>& pool) noexcept : size(0), index(0), elements((T*)buf), pool(pool) {
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
			 * @param args Arguments forwarded to task constructor
			 * @return true if task was added to buffer (or submitted successfully when buffer full),
			 *         false if buffer was full and submission failed (task not added)
			 * @note
			 * Supports two argument structures:
			 * 1. TaskStack object (must be passed by rvalue reference, using move semantics)
			 * 2. Callable object (function pointer/lambda/functor...) + bound arguments
			 *
			 * @details
			 *   - If buffer not full: adds task to buffer
			 *   - If buffer full:
			 *       1. First attempts to submit full batch
			 *       2. Only if submission succeeds, adds new task to buffer
			 *   - Returns false only when submission of full batch fails
			 */
			template <typename... Args>
			bool add(Args &&...args) noexcept
			{
				if (!check_and_submit())
					return false;

				new (elements + index) T(std::forward<Args>(args)...);
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
						submitted = pool.template submit<POS>(std::move(*(elements + start))) ? 1 : 0;
					else
						submitted = pool.template submit_bulk<POS>(elements + start, len1);
				}
				else
				{
					submitted = pool.template submit_bulk<POS>(
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

	using INNER::ThreadPool;
	using INNER::BatchSubmitter;
}

#endif