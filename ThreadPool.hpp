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
		static bool bind_current_thread_to_core(unsigned id) noexcept
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

#include "TPTask.hpp"
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
		TPBlockQueue<T>* queues;		  ///< Per-worker task queues
		std::vector<std::thread> workers; ///< Worker thread collection
		std::atomic<unsigned int> index;  ///< Atomic counter for round-robin task distribution to worker queues

		struct SingleStealer
		{
			unsigned int index;
			unsigned int threshold;
			std::vector<TPBlockQueue<T>*>& other;

			SingleStealer(std::vector<TPBlockQueue<T>*>& other, unsigned int maxLength)
				: index(0), other(other), threshold(std::min((unsigned int)2, maxLength)) {}

			unsigned int steal(T& element)
			{
				for (int i = 0; i < other.size(); ++i)
				{
					unsigned int now = (index + i) % other.size();
					if (other[now]->get_size() >= threshold)
					{
						if (other[now]->pop(element))
						{
							index = now;
							return 1;
						}
					}
				}
				return 0;
			}
		};

		struct BulkStealer
		{
			unsigned int index;
			unsigned int batchSize;
			unsigned int threshold;
			std::vector<TPBlockQueue<T>*>& other;

			BulkStealer(std::vector<TPBlockQueue<T>*>& other, unsigned int batchSize, unsigned int maxLength)
				: other(other), index(0), batchSize(batchSize), threshold(std::min(2 * batchSize, maxLength)) {}

			unsigned int steal(T* elements)
			{
				for (int i = 0; i < other.size(); ++i)
				{
					unsigned int now = (index + i) % other.size();
					if (other[now]->get_size() >= threshold)
					{
						unsigned int count = other[now]->popBulk(elements, batchSize);
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
		using task_type = T;

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
		bool init(unsigned int queueLength, unsigned int threadNum, unsigned int batchSize = 1) noexcept
		{
			if (batchSize == 0 || threadNum == 0 || batchSize > queueLength)
				return false;

			queues = (TPBlockQueue<T>*)ALIGNED_MALLOC(threadNum * sizeof(TPBlockQueue<T>), 64);

			if (!queues)
				return false;

			for (unsigned i = 0; i < threadNum; ++i)
			{
				new (&queues[i]) TPBlockQueue<T>();

				if (!queues[i].init(queueLength))
				{
					for (unsigned j = 0; j < i; ++j)
						queues[j].~TPBlockQueue<T>();

					ALIGNED_FREE(queues);
					queues = nullptr;
					return false;
				}
			}

			unsigned cores = std::thread::hardware_concurrency();
			this->threadNum = threadNum;
			this->queueLength = queueLength;
			workers.reserve(threadNum);

			for (unsigned i = 0; i < threadNum; ++i)
			{
				workers.emplace_back([this, i, cores, batchSize] {
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
				this->shutdownPolicy = shutdownPolicy;

				for (unsigned i = 0; i < workers.size(); ++i)
				{
					queues[i].stopWait();
				}

				for (auto& worker : workers)
				{
					if (worker.joinable())
						worker.join();
				}

				workers.clear();
				workers.shrink_to_fit();

				for (unsigned i = 0; i < threadNum; ++i)
					queues[i].~TPBlockQueue<T>();

				ALIGNED_FREE(queues);
				queues = nullptr;
				threadNum = 0;
				queueLength = 0;
			}
		}

		/**
		 * @brief Destroys thread pool and releases resources
		 */
		~ThreadPool() noexcept
		{
			exit(false);
		}

		// Deleted copy operations
		ThreadPool(const ThreadPool&) = delete;
		ThreadPool& operator=(const ThreadPool&) = delete;

	private:
		/**
		 * @brief Gets next queue index using round-robin
		 */
		unsigned int next_index() noexcept
		{
			return index.fetch_add(1, std::memory_order_relaxed) % threadNum;
		}

		TPBlockQueue<T>& select_queue() noexcept
		{
			unsigned int index = next_index();
			if (queues[index].get_exact_size() < queueLength)
			{
				return queues[index];
			}
			unsigned int half = threadNum / 2;
			return queues[(index + half) % threadNum];
		}

		TPBlockQueue<T>& select_queue_for_bulk(unsigned required) noexcept
		{
			unsigned int index = next_index();
			if (queues[index].get_exact_size() + required <= queueLength)
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
			std::vector<TPBlockQueue<T>*> other;
			other.reserve(threadNum - 1);

			for (unsigned i = 0; i < threadNum; ++i)
			{
				if (i != index)
					other.push_back(&queues[i]);
			}

			if (batchSize == 1)
				process_single(queues[index], other);
			else
				process_bulk(queues[index], other, batchSize);
		}

		/**
		 * @brief  Processes single task at a time
		 */
		void process_single(TPBlockQueue<T>& queue, std::vector<TPBlockQueue<T>*>& other) noexcept
		{
			alignas(alignof(T)) char storage[sizeof(T)];
			T* task = (T*)(&storage);

			if (!other.size())
			{
				while (queue.wait_pop(*task))
				{
					task->execute();
					task->~T();
				}
			}
			else
			{
				SingleStealer stealer(other, queueLength);

				while (true)
				{
					while (queue.get_exact_size() && queue.pop(*task))
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
						if (queue.wait_pop(*task, std::chrono::milliseconds(HSLL_THREADPOOL_TIMEOUT)))
						{
							task->execute();
							task->~T();
						}
					}

					if (queue.is_Stopped())
						break;
				}
			}

			while (shutdownPolicy && queue.pop(*task))
			{
				task->execute();
				task->~T();
			}
		}

		/**
		 * @brief  Execute multiple tasks at a time
		 */
		static inline void execute_tasks(T* tasks, unsigned int count)
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
		void process_bulk(TPBlockQueue<T>& queue, std::vector<TPBlockQueue<T>*>& other, unsigned batchSize) noexcept
		{
			T* tasks = (T*)ALIGNED_MALLOC(sizeof(T) * batchSize, alignof(T));

			if (!tasks)
			{
				printf("Failed to allocate task buffer");
				std::abort();
			}

			unsigned int count;

			if (!other.size())
			{
				while (count = queue.wait_popBulk(tasks, batchSize))
					execute_tasks(tasks, count);
			}
			else
			{
				BulkStealer stealer(other, batchSize, queueLength);

				while (true)
				{
					while (queue.get_exact_size() && (count = queue.popBulk(tasks, batchSize)))
						execute_tasks(tasks, count);

					count = stealer.steal(tasks);
					if (count)
					{
						execute_tasks(tasks, count);
					}
					else
					{
						count = queue.wait_popBulk(tasks, batchSize, std::chrono::milliseconds(HSLL_THREADPOOL_TIMEOUT));

						if (count)
							execute_tasks(tasks, count);
					}

					if (queue.is_Stopped())
						break;
				}
			}

			while (shutdownPolicy && (count = queue.popBulk(tasks, batchSize)))
				execute_tasks(tasks, count);

			ALIGNED_FREE(tasks);
		}
	};
}

#endif