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

			unsigned int m_index;
			unsigned int m_capacity;
			unsigned int m_threshold;
			unsigned int m_threadNum;

			TPBlockQueue<T>* m_queues;
			TPBlockQueue<T>* m_ignore;

			SingleStealer(TPBlockQueue<T>* queues, TPBlockQueue<T>* ignore,
				unsigned int capacity, unsigned int threadNum)
			{
				m_index = 0;
				m_capacity = capacity;
				m_threadNum = threadNum;
				m_threshold = std::min(capacity / 2, threadNum / 2);
				m_queues = queues;
				m_ignore = ignore;
			}

			unsigned int steal(T& element)
			{
				return steal_inner(element);
			}

			bool steal_inner(T& element)
			{
				unsigned int num = m_threadNum;

				for (unsigned int i = 0; i < num; ++i)
				{
					unsigned int now = (m_index + i) % num;
					TPBlockQueue<T>* queue = m_queues + now;

					if (queue != m_ignore && queue->get_size_weak() >= m_threshold)
					{
						if (queue->dequeue(element))
						{
							m_index = now;
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

			unsigned int m_index;
			unsigned int m_capacity;
			unsigned int m_batchSize;
			unsigned int m_threshold;
			unsigned int m_threadNum;

			TPBlockQueue<T>* m_queues;
			TPBlockQueue<T>* m_ignore;

			BulkStealer(TPBlockQueue<T>* queues, TPBlockQueue<T>* ignore, unsigned int capacity,
				unsigned int threadNum, unsigned int batchSize)
			{
				m_index = 0;
				m_batchSize = batchSize;
				m_capacity = capacity;
				m_threadNum = threadNum;
				m_threshold = std::min(capacity / 2, batchSize * threadNum / 2);
				m_queues = queues;
				m_ignore = ignore;
			}

			unsigned int steal(T* elements)
			{
				return steal_inner(elements);
			}

			unsigned int steal_inner(T* elements)
			{
				unsigned int count;
				unsigned int num = m_threadNum;

				for (unsigned int i = 0; i < num; ++i)
				{
					unsigned int now = (m_index + i) % num;
					TPBlockQueue<T>* queue = m_queues + now;

					if (queue != m_ignore && queue->get_size_weak() >= m_threshold)
					{
						count = queue->dequeue_bulk(elements, m_batchSize);

						if (count)
						{
							if (count == m_batchSize)
							{
								m_index = now;
							}
							else
							{
								m_index = (now + 1) % num;
							}

							return count;
						}
					}
				}

				return 0;
			}
		};

		/**
			@brief Thread pool implementation with multiple queues for task distribution
		*/
		template <typename T = TaskStack<>>
		class ThreadPool
		{
			static_assert(is_TaskStack<T>::value, "TYPE must be a TaskStack type");

			template <typename TYPE, unsigned int, INSERT_POS POS>
			friend class BatchSubmitter;

		private:

			unsigned int m_capacity;
			unsigned int m_batchSize;
			unsigned int m_threadNum;
			unsigned int m_mainFullThreshold;
			unsigned int m_otherFullThreshold;

			T* m_containers;
			Semaphore* m_stoppedSem;
			Semaphore* m_restartSem;
			SpinReadWriteLock m_rwLock;
			std::atomic<bool> m_shutdownFlag;
			std::atomic<bool> m_gracefulShutdown;

			TPBlockQueue<T>* m_queues;
			std::atomic<unsigned int> m_index;
			std::vector<std::thread> m_workers;
			TPGroupAllocator<T> m_groupAllocator;

		public:

#define HSLL_ENQUEUE_HELPER(exp1,exp2)                      \
															\
    assert(m_queues);                                       \
														    \
    if (m_threadNum == 1)                                   \
	{														\
		return exp1;                                        \
	}														\
														    \
														    \
    ReadLockGuard lock(m_rwLock);                           \
														    \
    unsigned int size;                                      \
    TPBlockQueue<T>* queue;                                 \
    std::thread::id id = std::this_thread::get_id();        \
    RoundRobinGroup<T>* group = m_groupAllocator.find(id);  \
														    \
    if(!group)                                              \
    {                                                       \
        queue = select_queue();                             \
														    \
        if (queue)                                          \
		{													\
			return exp2;                                    \
		}													\
														    \
        return 0;                                           \
    }                                                       \
														    \
    queue = group->current_queue();                         \
														    \
    if (queue)                                              \
	{														\
		size = exp2;                                        \
	}														\
	else                                                    \
	{														\
		size = 0;                                           \
	}														\
															\
    if (size)                                               \
    {                                                       \
        group->record(size);                                \
        return size;                                        \
    }                                                       \
    else                                                    \
    {                                                       \
        if ((queue = group->available_queue()))             \
        {                                                   \
            size = exp2;                                    \
            group->record(size);                            \
        }                                                   \
        else                                                \
        {                                                   \
            queue = m_groupAllocator.available_queue(group);\
														    \
            if(queue)                                       \
			{												\
				return exp2;								\
			}												\
        }                                                   \
    }                                                       \
														    \
    return size;

			ThreadPool() : m_queues(nullptr) {}

			/**
				@brief Initializes thread pool with fixed number of threads (no dynamic scaling)
				@param capacity Capacity of each internal task queue (must be >= 2)
				@param threadNum Fixed number of worker threads (must be != 0)
				@param batchSize Maximum number of tasks processed per batch (must be != 0)
				@note This function is not thread-safe.
				@return true  Initialization successful
				@return false Initialization failed (invalid parameters or resource allocation failure)
			*/
			bool init(unsigned int capacity, unsigned int threadNum, unsigned int batchSize = 1) noexcept
			{
				assert(!m_queues);

				if (!batchSize || !threadNum || capacity < 2)
				{
					return false;
				}

				if (!initResourse(capacity, threadNum, batchSize))
				{
					return false;
				}

				m_capacity = capacity;
				m_batchSize = std::min(batchSize, capacity / 2);
				m_threadNum = threadNum;
				m_mainFullThreshold = std::max(2u, (unsigned int)(capacity * HSLL_QUEUE_FULL_FACTOR_MAIN));
				m_otherFullThreshold = std::max(2u, (unsigned int)(capacity * HSLL_QUEUE_FULL_FACTOR_OTHER));
				m_shutdownFlag = false;
				m_gracefulShutdown = true;
				m_index = 0;

				m_workers.reserve(threadNum);
				m_groupAllocator.initialize(m_queues, threadNum, capacity, (unsigned int)(capacity * 0.05 > 1 ? capacity * 0.05 : 1));

				for (unsigned i = 0; i < threadNum; ++i)
				{
					m_workers.emplace_back(&ThreadPool::worker, this, i);
				}

				return true;
			}

			/**
				@brief Checks if the thread pool has been initialized.
				@note This function is not thread-safe.
				@return true  if the thread pool has been successfully initialized
				@return false if the thread pool is not initialized
			*/
			bool isInited()
			{
				return m_queues;
			}

			/**
				@brief Non-blocking task submission to the thread pool
				@tparam POS Insertion position (HEAD or TAIL, default: TAIL)
				@param args Arguments forwarded to task constructor
				@return true if task was successfully added, false otherwise
				@note
				Supports two argument structures:
				1. TaskStack object (must be passed by rvalue reference, using move semantics)
				2. Callable object (function pointer/lambda/functor...) + bound arguments
			*/
			template <INSERT_POS POS = TAIL, typename... Args>
			bool submit(Args && ...args) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(m_queues-> template emplace<POS>(std::forward<Args>(args)...)),
					(queue-> template emplace<POS>(std::forward<Args>(args)...))
				)
			}

			/**
				@brief Blocking task submission with indefinite wait
				@tparam POS Insertion position (HEAD or TAIL, default: TAIL)
				@param args Arguments forwarded to task constructor
				@return true if task was added successfully, false if thread pool was stopped
				@note
				Supports two argument structures:
				1. TaskStack object (must be passed by rvalue reference, using move semantics)
				2. Callable object (function pointer/lambda/functor...) + bound arguments
			*/
			template <INSERT_POS POS = TAIL, typename... Args>
			bool wait_submit(Args && ...args) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(m_queues-> template wait_emplace<POS>(std::forward<Args>(args)...)),
					(queue-> template wait_emplace<POS>(std::forward<Args>(args)...))
				)
			}

			/**
				@brief Blocking task submission with timeout
				@tparam POS Insertion position (HEAD or TAIL, default: TAIL)
				@param timeout Maximum duration to wait for space
				@param args Arguments forwarded to task constructor
				@return true if task was added successfully, false on timeout or thread pool stop
				@note
				Supports two argument structures:
				1. TaskStack object (must be passed by rvalue reference, using move semantics)
				2. Callable object (function pointer/lambda/functor...) + bound arguments
			*/
			template <INSERT_POS POS = TAIL, typename Rep, typename Period, typename... Args>
			bool wait_submit_for(const std::chrono::duration<Rep, Period>& timeout, Args && ...args) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(m_queues-> template wait_emplace_for<POS>(timeout, std::forward<Args>(args)...)),
					(queue-> template wait_emplace_for<POS>(timeout, std::forward<Args>(args)...))
				)
			}

			/**
				@brief Blocking task submission with absolute timeout
				@tparam POS Insertion position (HEAD or TAIL, default: TAIL)
				@param abs Absolute timeout point
				@param args Arguments forwarded to task constructor
				@return true if task was added successfully, false on timeout or thread pool stop
				@note
				Supports two argument structures:
				1. TaskStack object (must be passed by rvalue reference, using move semantics)
				2. Callable object (function pointer/lambda/functor...) + bound arguments
			*/
			template <INSERT_POS POS = TAIL, typename Clock, typename Duration, typename... Args>
			bool wait_submit_until(const std::chrono::time_point<Clock, Duration>& abs, Args && ...args)
			{
				HSLL_ENQUEUE_HELPER(
					(m_queues-> template wait_emplace_until<POS>(abs, std::forward<Args>(args)...)),
					(queue-> template wait_emplace_until<POS>(abs, std::forward<Args>(args)...))
				)
			}

			/**
				@brief Non-blocking bulk task submission (using move semantics)
				@tparam POS Insertion position (HEAD or TAIL, default: TAIL)
				@param tasks Array of tasks to enqueue
				@param count Number of tasks in array (must be > 0)
				@return Actual number of tasks added (may be less than count)
			*/
			template <INSERT_POS POS = TAIL>
			unsigned int submit_bulk(T* tasks, unsigned int count) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(m_queues-> template enqueue_bulk<POS>(tasks, count)),
					(queue-> template enqueue_bulk<POS>(tasks, count))
				)
			}

			/**
				@brief Blocking bulk submission with indefinite wait (using move semantics)
				@tparam POS Insertion position (HEAD or TAIL, default: TAIL)
				@param tasks Array of tasks to add
				@param count Number of tasks to add (must be > 0)
				@return Actual number of tasks added (may be less than count)
			*/
			template <INSERT_POS POS = TAIL>
			unsigned int wait_submit_bulk(T* tasks, unsigned int count) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(m_queues-> template wait_enqueue_bulk<POS>(tasks, count)),
					(queue-> template wait_enqueue_bulk<POS>(tasks, count))
				)
			}

			/**
				@brief Blocking bulk submission with timeout (using move semantics)
				@tparam POS Insertion position (HEAD or TAIL, default: TAIL)
				@param timeout Maximum duration to wait for space
				@param tasks Array of tasks to add
				@param count Number of tasks to add (must be > 0)
				@return Actual number of tasks added (may be less than count)
			*/
			template <INSERT_POS POS = TAIL, typename Rep, typename Period>
			unsigned int wait_submit_bulk_for(const std::chrono::duration<Rep, Period>& timeout, T* tasks, unsigned int count) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(m_queues-> template wait_enqueue_bulk_for<POS>(timeout, tasks, count)),
					(queue-> template wait_enqueue_bulk_for<POS>(timeout, tasks, count))
				)
			}

			/**
				@brief Blocking bulk submission with absolute timeout (using move semantics)
				@tparam POS Insertion position (HEAD or TAIL, default: TAIL)
				@param abs Absolute timeout point
				@param tasks Array of tasks to add
				@param count Number of tasks to add (must be > 0)
				@return Actual number of tasks added (may be less than count)
			*/
			template <INSERT_POS POS = TAIL, typename Clock, typename Duration>
			unsigned int wait_submit_bulk_until(const std::chrono::time_point<Clock, Duration>& abs, T* tasks, unsigned int count)
			{
				HSLL_ENQUEUE_HELPER(
					(m_queues-> template wait_enqueue_bulk_until<POS>(abs, tasks, count)),
					(queue-> template wait_enqueue_bulk_until<POS>(abs, tasks, count))
				)
			}

			/**
				@brief Waits for all tasks to complete.
				@note
				1. During the join operation, adding any new tasks is prohibited.
				2. This function is not thread-safe.
				3. This function does not clean up resources. After the call, the queue can be used normally.
			*/
			void drain() noexcept
			{
				assert(m_queues);

				for (unsigned int i = 0; i < m_threadNum; ++i)
				{
					m_restartSem[i].release();
					m_queues[i].disableWait();
				}

				for (unsigned int i = 0; i < m_threadNum; ++i)
				{
					m_stoppedSem[i].acquire();
					m_queues[i].enableWait();
				}
			}

			/**
				@brief Stops all workers and releases resources.
				@param graceful If true, performs a graceful shutdown (waits for tasks to complete);
							  if false, forces an immediate shutdown.
				@note This function is not thread-safe.
				@note After calling this function, the thread pool can be reused by calling init again.
			*/
			void shutdown(bool graceful = true) noexcept
			{
				if (!m_queues)
				{
					return;
				}

				this->m_shutdownFlag = true;
				this->m_gracefulShutdown = graceful;

				{
					for (unsigned i = 0; i < m_workers.size(); ++i)
					{
						m_restartSem[i].release();
					}

					for (unsigned i = 0; i < m_threadNum; ++i)
					{
						m_queues[i].disableWait();
					}

					for (auto& worker : m_workers)
					{
						worker.join();
					}
				}

				releaseResourse();
			}

			/**
				@brief Registers the current thread. Registered threads participate in queue grouping and obtain a dedicated queue group.
				@note
				 1. The registered thread must be a producer thread
				 2. Production capacity between registered threads should not vary significantly
				 3. If the thread pool will continue to be used after this thread exits, you MUST unregister
					the thread before exit to allow queue reallocation
			*/
			void register_this_thread() noexcept
			{
				assert(m_queues);
				std::thread::id id = std::this_thread::get_id();
				WriteLockGuard lock(m_rwLock);
				m_groupAllocator.register_thread(id);
			}

			/**
				@brief Unregisters the current thread. It will no longer participate in queue grouping.
			*/
			void unregister_this_thread() noexcept
			{
				assert(m_queues);
				std::thread::id id = std::this_thread::get_id();
				WriteLockGuard lock(m_rwLock);
				m_groupAllocator.unregister_thread(id);
			}

			~ThreadPool() noexcept
			{
				if (m_queues)
				{
					shutdown(false);
				}
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
					(m_queues-> template enqueue_bulk<POS>(part1, count1, part2, count2)),
					(queue-> template enqueue_bulk<POS>(part1, count1, part2, count2))
				)
			}

			unsigned int next_index() noexcept
			{
				return m_index.fetch_add(1, std::memory_order_relaxed) % m_threadNum;
			}

			TPBlockQueue<T>* select_queue() noexcept
			{
				unsigned int now = next_index();
				TPBlockQueue<T>* queue = m_queues + now;

				if (queue->get_size_weak() <= m_mainFullThreshold)
				{
					return queue;
				}

				for (unsigned int i = 1; i <= m_threadNum - 1; ++i)
				{
					queue = m_queues + ((now + i) % m_threadNum);

					if (queue->get_size_weak() <= m_otherFullThreshold)
					{
						return queue;
					}
				}

				return nullptr;
			}

			void worker(unsigned int index) noexcept
			{
				if (m_batchSize == 1)
				{
					process_single(m_queues + index, index);
				}
				else
				{
					process_bulk(m_queues + index, index);
				}
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
				bool enableSteal = (m_threadNum != 1);
				T* task = m_containers + index * m_batchSize;
				SingleStealer<T> stealer(m_queues, queue, m_capacity, m_threadNum);

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
						{
							break;
						}
					}

					if (m_gracefulShutdown)
					{
						while (queue->dequeue(*task))
						{
							task->execute();
							task->~T();
						}
					}

					m_stoppedSem[index].release();
					m_restartSem[index].acquire();

					if (m_shutdownFlag)
					{
						break;
					}
				}
			}

			void process_bulk(TPBlockQueue<T>* queue, unsigned int index) noexcept
			{
				bool enableSteal = (m_threadNum != 1);
				T* tasks = m_containers + index * m_batchSize;
				BulkStealer<T> stealer(m_queues, queue, m_capacity, m_threadNum, m_batchSize);

				while (true)
				{
					unsigned int count;

					while (true)
					{
						while (true)
						{
							unsigned int size = queue->get_size_weak();
							unsigned int round = m_batchSize;

							while (round && size < m_batchSize)
							{
								std::this_thread::yield();
								size = queue->get_size_weak();
								round--;
							}

							if (size && (count = queue->dequeue_bulk(tasks, m_batchSize)))
							{
								execute_tasks(tasks, count);
							}
							else
							{
								break;
							}
						}

						if (enableSteal && (count = stealer.steal(tasks)))
						{
							execute_tasks(tasks, count);
							goto cheak;
						}

						count = queue->wait_dequeue_bulk_for(std::chrono::milliseconds(HSLL_THREADPOOL_DEQUEUE_TIMEOUT_MILLISECONDS),
							tasks, m_batchSize);

						if (count)
						{
							execute_tasks(tasks, count);
						}

					cheak:

						if (queue->is_stopped_weak())
						{
							break;
						}
					}

					if (m_gracefulShutdown)
					{
						count = queue->dequeue_bulk(tasks, m_batchSize);

						while (count)
						{
							execute_tasks(tasks, count);
						}
					}

					m_stoppedSem[index].release();
					m_restartSem[index].acquire();

					if (m_shutdownFlag)
					{
						break;
					}
				}
			}

			bool initResourse(unsigned int capacity, unsigned int maxThreadNum, unsigned int batchSize) noexcept
			{
				unsigned int succeed = 0;

				if (!(m_restartSem = new(std::nothrow) Semaphore[2 * maxThreadNum]))
				{
					goto clean_1;
				}

				m_stoppedSem = m_restartSem + maxThreadNum;

				if (!(m_containers = (T*)HSLL_ALIGNED_MALLOC(sizeof(T) * batchSize * maxThreadNum, alignof(T))))
				{
					goto clean_2;
				}

				if (!(m_queues = (TPBlockQueue<T>*)HSLL_ALIGNED_MALLOC(maxThreadNum * sizeof(TPBlockQueue<T>), alignof(TPBlockQueue<T>))))
				{
					goto clean_3;
				}

				for (unsigned i = 0; i < maxThreadNum; ++i)
				{
					new(&m_queues[i]) TPBlockQueue<T>();

					if (!m_queues[i].init(capacity))
					{
						goto clean_4;
					}

					succeed++;
				}

				return true;

			clean_4:

				for (unsigned i = 0; i < succeed; ++i)
				{
					m_queues[i].~TPBlockQueue<T>();
				}

			clean_3:

				HSLL_ALIGNED_FREE(m_queues);
				m_queues = nullptr;

			clean_2:

				HSLL_ALIGNED_FREE(m_containers);

			clean_1:

				delete[] m_restartSem;

				return false;
			}

			void releaseResourse() noexcept
			{
				for (unsigned i = 0; i < m_threadNum; ++i)
				{
					m_queues[i].~TPBlockQueue<T>();
				}

				HSLL_ALIGNED_FREE(m_queues);
				HSLL_ALIGNED_FREE(m_containers);
				delete[] m_restartSem;
				m_queues = nullptr;
				m_workers.clear();
				m_workers.shrink_to_fit();
				m_groupAllocator.reset();
			}
		};

		template <typename T, unsigned int BATCH, INSERT_POS POS = TAIL>
		class BatchSubmitter
		{
			static_assert(is_TaskStack<T>::value, "T must be a TaskStack type");
			static_assert(BATCH > 0, "BATCH > 0");
			alignas(alignof(T)) unsigned char buf[BATCH * sizeof(T)];

			T* m_elements;
			unsigned int m_size;
			unsigned int m_index;
			ThreadPool<T>& m_pool;

			bool check_and_submit()
			{
				if (m_size == BATCH)
				{
					return submit() == BATCH;
				}

				return true;
			}

		public:
			/**
				@brief Constructs a batch submitter associated with a thread pool
				@param pool Pointer to the thread pool for batch task submission
			*/
			BatchSubmitter(ThreadPool<T>& pool) noexcept : m_elements((T*)buf), m_size(0), m_index(0), m_pool(pool)
			{
			}

			/**
				@brief Gets current number of buffered tasks
				@return Number of tasks currently held in the batch buffer
			*/
			unsigned int get_size() const noexcept
			{
				return m_size;
			}

			/**
				@brief Checks if batch buffer is empty
				@return true if no tasks are buffered, false otherwise
			*/
			bool empty() const noexcept
			{
				return m_size == 0;
			}

			/**
				@brief Checks if batch buffer is full
				@return true if batch buffer has reached maximum capacity (BATCH), false otherwise
			*/
			bool full() const noexcept
			{
				return m_size == BATCH;
			}

			/**
				@brief Constructs task in-place in batch buffer
				@param args Arguments forwarded to task constructor
				@return true if task was added to buffer (or submitted successfully when buffer full),
					   false if buffer was full and submission failed (task not added)
				@note
				Supports two argument structures:
				1. TaskStack object (must be passed by rvalue reference, using move semantics)
				2. Callable object (function pointer/lambda/functor...) + bound arguments

				@details
				 - If buffer not full: adds task to buffer
				 - If buffer full:
					 1. First attempts to submit full batch
					 2. Only if submission succeeds, adds new task to buffer
				 - Returns false only when submission of full batch fails
			*/
			template <typename... Args>
			bool add(Args &&...args) noexcept
			{
				if (!check_and_submit())
				{
					return false;
				}

				new(m_elements + m_index) T(std::forward<Args>(args)...);
				m_index = (m_index + 1) % BATCH;
				m_size++;
				return true;
			}

			/**
				@brief Submits all buffered tasks to thread pool
				@return Number of tasks successfully submitted
				@details Moves buffered tasks to thread pool in bulk.
			*/
			unsigned int submit() noexcept
			{
				if (!m_size)
				{
					return 0;
				}

				unsigned int start = (m_index - m_size + BATCH) % BATCH;
				unsigned int len1 = (start + m_size <= BATCH) ? m_size : (BATCH - start);
				unsigned int len2 = m_size - len1;
				unsigned int submitted;

				if (!len2)
				{
					if (len1 == 1)
					{
						submitted = m_pool.template submit<POS>(std::move(*(m_elements + start))) ? 1 : 0;
					}
					else
					{
						submitted = m_pool.template submit_bulk<POS>(m_elements + start, len1);
					}
				}
				else
				{
					submitted = m_pool.template submit_bulk<POS>(
						m_elements + start, len1,
						m_elements, len2
					);
				}

				if (submitted > 0)
				{
					if (submitted <= len1)
					{
						for (unsigned i = 0; i < submitted; ++i)
						{
							m_elements[(start + i) % BATCH].~T();
						}
					}
					else
					{
						for (unsigned i = 0; i < len1; ++i)
						{
							m_elements[(start + i) % BATCH].~T();
						}

						for (unsigned i = 0; i < submitted - len1; ++i)
						{
							m_elements[i].~T();
						}
					}

					m_size -= submitted;
				}

				return submitted;
			}

			~BatchSubmitter() noexcept
			{
				if (m_size > 0)
				{
					unsigned int start = (m_index - m_size + BATCH) % BATCH;
					unsigned int len1 = (start + m_size <= BATCH) ? m_size : (BATCH - start);
					unsigned int len2 = m_size - len1;

					for (unsigned int i = 0; i < len1; i++)
					{
						m_elements[(start + i) % BATCH].~T();
					}

					for (unsigned int i = 0; i < len2; i++)
					{
						m_elements[i].~T();
					}
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