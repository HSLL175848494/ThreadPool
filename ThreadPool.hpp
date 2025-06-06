#ifndef HSLL_THREADPOOL
#define HSLL_THREADPOOL

#if defined(__linux__)
#include <pthread.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace HSLL
{
	struct ThreadBinder
	{
		/**
		 * @brief Binds the calling thread to a specific CPU core
		 * @param id Target core ID (0-indexed)
		 * @return true if binding succeeded, false otherwise
		 *
		 * @note Platform-specific implementation:
		 *       - Linux: Uses pthread affinity
		 *       - Windows: Uses thread affinity mask
		 *       - Other platforms: No-op (always returns true)
		 */
		static bool bind_current_thread_to_core(unsigned id)
		{
#if defined(__linux__)
			cpu_set_t cpuset;
			CPU_ZERO(&cpuset);
			CPU_SET(id, &cpuset);
			return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;

#elif defined(_WIN32)
			return SetThreadAffinityMask(GetCurrentThread(), 1ull << id) != 0;

#else
			return true;
#endif
		}
	};
};

#include <vector>
#include <thread>
#include <atomic>
#include <assert.h>

#include "TPTask.h"
#include "TPBlockQueue.hpp"

namespace HSLL
{

#define HSLL_THREADPOOL_TIMEOUT 5

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
		unsigned int queueLength;		  ///< Capacity of each internal queue
		TPBlockQueue<T> *queues;		  ///< Per-worker task queues
		std::vector<std::thread> workers; ///< Worker thread collection
		std::atomic<unsigned int> index;  ///< Atomic counter for round-robin task distribution to worker queues

	public:
		/**
		 * @brief Constructs an uninitialized thread pool
		 */
		ThreadPool() : queues(nullptr), threadNum(0), queueLength(0), shutdownPolicy(true) {}

		/**
		 * @brief Initializes thread pool resources
		 * @param queueLength Capacity of each internal queue
		 * @param threadNum Number of worker threads/queues to create
		 * @param batchSize Maximum tasks to process per batch (min 1)
		 * @return true if initialization succeeded, false otherwise
		 */
		bool init(unsigned int queueLength, unsigned int threadNum, unsigned int batchSize = 1)
		{
			if (batchSize == 0 || threadNum == 0 || batchSize > queueLength)
				return false;

			queues = new (std::nothrow) TPBlockQueue<T>[threadNum];

			if (!queues)
				return false;

			for (unsigned i = 0; i < threadNum; ++i)
			{
				if (!queues[i].init(queueLength))
				{
					delete[] queues;
					queues = nullptr;
					return false;
				}
			}

			unsigned cores = std::thread::hardware_concurrency();

			if (!cores)
			{
				delete[] queues;
				queues = nullptr;
				return false;
			}

			this->threadNum = threadNum;
			this->queueLength = queueLength;
			workers.reserve(threadNum);

			for (unsigned i = 0; i < threadNum; ++i)
			{
				workers.emplace_back([this, i, cores, batchSize]
									 {
						if (cores > 0)
						{
							unsigned id = i % cores;
							ThreadBinder::bind_current_thread_to_core(id);
						}
						worker(i, batchSize); });
			}

			return true;
		}

		/**
		 * @brief Enqueues a task using perfect forwarding
		 * @tparam Args Constructor argument types for task type T
		 * @param args Arguments to forward to task constructor
		 * @return true if task was enqueued successfully
		 */
		template <typename... Args>
		bool emplace(Args &&...args)
		{
			return select_queue().emplace(std::forward<Args>(args)...);
		}

		/**
		 * @brief Blockingly adds a task to the thread pool using in-place construction
		 * @tparam Args Types of arguments for task constructor
		 * @param args Arguments forwarded to task constructor
		 * @return true if task was added successfully, false if thread pool was stopped
		 * @details Waits indefinitely until space is available in a worker queue.
		 *          Constructs the task in-place in the selected worker queue.
		 */
		template <typename... Args>
		bool wait_emplace(Args &&...args)
		{
			return select_queue().wait_emplace(std::forward<Args>(args)...);
		}

		/**
		 * @brief Adds a task with timeout using in-place construction
		 * @tparam Rep Chrono duration representation type
		 * @tparam Period Chrono duration period type
		 * @tparam Args Types of arguments for task constructor
		 * @param timeout Maximum duration to wait for space
		 * @param args Arguments forwarded to task constructor
		 * @return true if task was added, false on timeout or thread pool stop
		 * @details Waits up to specified duration for space in a worker queue.
		 *          Constructs the task if space becomes available.
		 */
		template <class Rep, class Period, typename... Args>
		bool wait_emplace(const std::chrono::duration<Rep, Period> &timeout, Args &&...args)
		{
			return select_queue().wait_emplace(timeout, std::forward<Args>(args)...);
		}

		/**
		 * @brief Enqueues a single preconstructed task
		 * @tparam U Forwarding reference type (deduced)
		 * @param task Task object to enqueue
		 * @return true if task was enqueued successfully
		 */
		template <typename U>
		bool append(U &&task)
		{
			return select_queue().push(std::forward<U>(task));
		}

		/**
		 * @brief Blockingly adds a preconstructed task to the thread pool
		 * @tparam U Deduced task type (supports perfect forwardingk)
		 * @param task Task object to add
		 * @return true if task was added, false if thread pool was stopped
		 * @details Waits indefinitely until space is available in a worker queue.
		 *          Adds the task to the selected worker queue.
		 */
		template <class U>
		bool wait_append(U &&task)
		{
			return select_queue().wait_push(std::forward<U>(task));
		}

		/**
		 * @brief Adds a preconstructed task with timeout
		 * @tparam U Deduced task type
		 * @tparam Rep Chrono duration representation type
		 * @tparam Period Chrono duration period type
		 * @param task Task object to add
		 * @param timeout Maximum duration to wait for space
		 * @return true if task was added, false on timeout or thread pool stop
		 */
		template <class U, class Rep, class Period>
		bool wait_append(U &&task, const std::chrono::duration<Rep, Period> &timeout)
		{
			return select_queue().wait_push(std::forward<U>(task), timeout);
		}

		/**
		 * @brief Enqueues multiple preconstructed tasks
		 * @tparam METHOD Bulk insertion method (COPY/MOVE)
		 * @param tasks Task object array
		 * @param count Number of tasks in array (Required: count <= queueLength)
		 * @return Actual number of tasks enqueued
		 */
		template <BULK_CMETHOD METHOD = COPY>
		unsigned int append_bulk(T *tasks, unsigned int count)
		{
			return select_queue_for_bulk(count / 2).template pushBulk<METHOD>(tasks, count);
		}

		/**
		 * @brief Blockingly adds multiple tasks using specified construction method
		 * @tparam METHOD Bulk construction method (COPY/MOVE)
		 * @param tasks Array of tasks to add
		 * @param count Number of tasks in array
		 * @return Actual number of tasks added before stop
		 * @details Waits indefinitely until enough space is available in a worker queue.
		 *          Tasks are added using COPY or MOVE semantics as specified.
		 */
		template <BULK_CMETHOD METHOD = COPY>
		unsigned int wait_appendBulk(T *tasks, unsigned int count)
		{
			return select_queue().template wait_pushBulk<METHOD>(tasks, count);
		}

		/**
		 * @brief Adds multiple tasks with timeout and construction method
		 * @tparam METHOD Bulk construction method (COPY/MOVE)
		 * @tparam Rep Chrono duration representation type
		 * @tparam Period Chrono duration period type
		 * @param tasks Array of tasks to add
		 * @param count Number of tasks to add
		 * @param timeout Maximum duration to wait for space
		 * @return Actual number of tasks added (may be less than count)
		 * @details Waits up to specified duration for space in a worker queue.
		 *          Adds tasks using selected construction method on success.
		 */
		template <BULK_CMETHOD METHOD = COPY, class Rep, class Period>
		unsigned int wait_appendBulk(T *tasks, unsigned int count, const std::chrono::duration<Rep, Period> &timeout)
		{
			return select_queue().template wait_pushBulk<METHOD>(tasks, count, timeout);
		}

		/**
		 * @brief Stops all workers and releases resources
		 * @param shutdownPolicy true for graceful shutdown (waiting for tasks to complete), false for immediate shutdown
		 */
		void exit(bool shutdownPolicy = true) noexcept
		{
			if (queues)
			{
				this->shutdownPolicy = shutdownPolicy;

				for (unsigned i = 0; i < workers.size(); ++i)
				{
					queues[i].stopWait();
				}

				for (auto &worker : workers)
				{
					if (worker.joinable())
						worker.join();
				}

				workers.clear();
				workers.shrink_to_fit();
				threadNum = 0;
				queueLength = 0;
				delete[] queues;
				queues = nullptr;
			}
		}

		/**
		 * @brief Destroys thread pool and releases resources
		 */
		~ThreadPool()
		{
			exit(false);
		}

		// Deleted copy operations
		ThreadPool(const ThreadPool &) = delete;
		ThreadPool &operator=(const ThreadPool &) = delete;

	private:
		/**
		 * @brief Gets next queue index using round-robin
		 */
		unsigned int next_index() noexcept
		{
			return index.fetch_add(1, std::memory_order_relaxed) % threadNum;
		}

		TPBlockQueue<T> &select_queue() noexcept
		{
			unsigned int index = next_index();
			if (queues[index].size < queueLength)
			{
				return queues[index];
			}
			unsigned int half = threadNum / 2;
			return queues[(index + half) % threadNum];
		}

		TPBlockQueue<T> &select_queue_for_bulk(unsigned required_space) noexcept
		{
			unsigned int index = next_index();
			if (queues[index].size + required_space <= queueLength)
			{
				return queues[index];
			}
			unsigned int half = threadNum / 2;
			return queues[(index + half) % threadNum];
		}

		/**
		 * @brief Worker thread processing function
		 */
		void worker(unsigned int index, unsigned batchSize) noexcept
		{
			std::vector<TPBlockQueue<T> *> other;
			other.reserve(threadNum - 1);

			for (unsigned i = 0; i < threadNum; ++i)
			{
				if (i != index)
					other.push_back(&queues[i]);
			}

			if (batchSize == 1)
				process_single(std::ref(queues[index]), std::ref(other), shutdownPolicy);
			else
				process_bulk(std::ref(queues[index]), std::ref(other), batchSize, shutdownPolicy);
		}

		/**
		 * @brief  Processes single task at a time
		 */
		static void process_single(TPBlockQueue<T> &queue, std::vector<TPBlockQueue<T> *> &other, bool &safeExit)
		{
			struct Stealer
			{
				unsigned int index;
				unsigned int total;
				unsigned int threshold;
				std::vector<TPBlockQueue<T> *> &other;

				Stealer(std::vector<TPBlockQueue<T> *> &other, unsigned int maxLength)
					: other(other), index(0), total(other.size()),
					  threshold(std::min(total, maxLength)) {}

				bool steal(T &element)
				{
					for (int i = 0; i < total; ++i)
					{
						unsigned int now = (index + i) % total;
						if (other[now]->size >= threshold)
						{
							if (other[now]->template pop<PLACE>(element))
							{
								index = now;
								return true;
							}
						}
					}
					return false;
				}
			};

			char storage[sizeof(T)];
			T *task = (T *)(&storage);
			Stealer stealer(other, queue.maxSize);

			if (!other.size())
			{
				while (queue.template wait_pop<PLACE>(*task))
				{
					task->execute();
					task->~T();
				}
			}
			else
			{

				while (true)
				{
					while (queue.template pop<PLACE>(*task))
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
						if (queue.template wait_pop<PLACE>(*task, std::chrono::milliseconds(HSLL_THREADPOOL_TIMEOUT)))
						{
							task->execute();
							task->~T();
						}
						else if (queue.isStopped)
						{
							return;
						}
					}
				}
			}

			while (safeExit && queue.template pop<PLACE>(*task))
			{
				task->execute();
				task->~T();
			}
		}

		/**
		 * @brief  Execute multiple tasks at a time
		 */
		static inline void execute_tasks(T *tasks, unsigned int count)
		{
			for (unsigned int i = 0; i < count; ++i)
			{
				tasks[i].execute();
				tasks[i].~T();
			}
		}

		/**
		 * @brief  Processes multiple tasks at a time
		 */
		static void process_bulk(TPBlockQueue<T> &queue, std::vector<TPBlockQueue<T> *> &other, unsigned batchSize, bool &safeExit)
		{
			struct Stealer
			{
				unsigned int index;
				unsigned int total;
				unsigned int batchSize;
				unsigned int threshold;
				std::vector<TPBlockQueue<T> *> &other;

				Stealer(std::vector<TPBlockQueue<T> *> &other, unsigned int batchSize, unsigned int maxLength)
					: other(other), index(0), total(other.size()), batchSize(batchSize),
					  threshold(std::min(batchSize * total, maxLength)) {}

				unsigned int steal(T *elements)
				{
					for (int i = 0; i < total; ++i)
					{
						unsigned int now = (index + i) % total;
						if (other[now]->size >= threshold)
						{
							unsigned int count = other[now]->template popBulk<PLACE>(elements, batchSize);
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

			T *tasks = (T *)((void *)operator new[](batchSize * sizeof(T)));
			assert(tasks && "Failed to allocate task buffer");
			unsigned int count;
			Stealer stealer(other, batchSize, queue.maxSize);

			if (!other.size())
			{
				while (count = queue.template wait_popBulk<PLACE>(tasks, batchSize))
					execute_tasks(tasks, count);
			}
			else
			{
				while (true)
				{
					while (count = queue.template popBulk<PLACE>(tasks, batchSize))
						execute_tasks(tasks, count);

					count = stealer.steal(tasks);
					if (count)
					{
						execute_tasks(tasks, count);
					}
					else
					{
						count = queue.template wait_popBulk<PLACE>(tasks, batchSize, std::chrono::milliseconds(HSLL_THREADPOOL_TIMEOUT));

						if (count)
							execute_tasks(tasks, count);
						else if (queue.isStopped)
							break;
					}
				}
			}

			while (safeExit && (count = queue.template popBulk<PLACE>(tasks, batchSize)))
				execute_tasks(tasks, count);

			operator delete[](tasks);
		}
	};
}

#endif