#ifndef HSLL_TPBLOCKQUEUE
#define HSLL_TPBLOCKQUEUE

#include <mutex>
#include <cassert>
#include <condition_variable>

//Branch Prediction
#if defined(__GNUC__) || defined(__clang__)
#define HSLL_LIKELY(x) __builtin_expect(!!(x), 1)
#define HSLL_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define HSLL_LIKELY(x) (x)
#define HSLL_UNLIKELY(x) (x)
#endif

// Aligned Malloc
#if defined(_WIN32)
#include <malloc.h>
#define HSLL_ALIGNED_MALLOC(size, align) _aligned_malloc(size, align)
#define HSLL_ALIGNED_FREE(ptr) _aligned_free(ptr)
#else
#include <stdlib.h>
#if defined(__APPLE__) || !defined(_ISOC11_SOURCE)

namespace HSLL
{
	namespace INNER
	{
		inline void* hsll_aligned_alloc(size_t align, size_t size)
		{
			void* ptr = nullptr;

			if (posix_memalign(&ptr, align, size) != 0)
			{
				return nullptr;
			}

			return ptr;
		}
	}
}

#define HSLL_ALIGNED_MALLOC(size, align) HSLL::INNER::hsll_aligned_alloc(align, size)
#else

namespace HSLL
{
	namespace INNER
	{
		inline void* hsll_aligned_alloc(size_t align, size_t size)
		{
			assert(align > 0 && (align & (align - 1)) == 0 && "align must be > 0 and be an nth power of 2.");

			const size_t aligned_size = (size + align - 1) & ~(align - 1);
			return aligned_alloc(align, aligned_size);
		}
	}
}

#define HSLL_ALIGNED_MALLOC(size, align) HSLL::INNER::hsll_aligned_alloc(align, size)
#endif
#define HSLL_ALIGNED_FREE(ptr) free(ptr)
#endif

namespace HSLL
{
	namespace INNER
	{
		/**
			@brief Enumeration defining the insertion position
		*/
		enum INSERT_POS
		{
			TAIL, ///< Insert at the tail (default)
			HEAD  ///< Insert at the head
		};

		/**
			@brief Circular buffer based blocking queue implementation
		*/
		template <typename TYPE>
		class alignas(64) TPBlockQueue
		{
			// Memory management
			bool m_stopped;
			void* m_memBlock;

			// Queue state tracking
			unsigned int m_maxSpin;
			unsigned int m_capacity;
			unsigned int m_totalsize;
			unsigned int m_size;

			// Buffer pointers
			TYPE* m_dataListHead;
			TYPE* m_dataListTail;
			uintptr_t m_border;

			// Synchronization primitives
			std::mutex m_dataMutex;
			std::condition_variable m_notEmptyCond;
			std::condition_variable m_notFullCond;

			void move_tail_next()
			{
				m_dataListTail = (TYPE*)((char*)m_dataListTail + sizeof(TYPE));

				if (HSLL_UNLIKELY((uintptr_t)m_dataListTail == m_border))
				{
					m_dataListTail = (TYPE*)(uintptr_t)m_memBlock;
				}
			}

			void move_head_next()
			{
				m_dataListHead = (TYPE*)((char*)m_dataListHead + sizeof(TYPE));

				if (HSLL_UNLIKELY((uintptr_t)m_dataListHead == m_border))
				{
					m_dataListHead = (TYPE*)(uintptr_t)m_memBlock;
				}
			}

			// Reserve for head push
			void move_head_prev()
			{
				m_dataListHead = (TYPE*)((char*)m_dataListHead - sizeof(TYPE));

				if (HSLL_UNLIKELY((uintptr_t)m_dataListHead < (uintptr_t)m_memBlock))
				{
					m_dataListHead = (TYPE*)(m_border - sizeof(TYPE));
				}
			}

			// Insert position implementations
			template <INSERT_POS POS, typename... Args>
			typename std::enable_if<POS == TAIL>::type emplace_impl(Args &&...args)
			{
				new(m_dataListTail) TYPE(std::forward<Args>(args)...);
				move_tail_next();
			}

			template <INSERT_POS POS, typename... Args>
			typename std::enable_if<POS == HEAD>::type emplace_impl(Args &&...args)
			{
				move_head_prev();
				new(m_dataListHead) TYPE(std::forward<Args>(args)...);
			}

			template <INSERT_POS POS>
			typename std::enable_if<POS == TAIL>::type enqueue_bulk_impl(TYPE* elements, unsigned int toPush)
			{
				for (unsigned int i = 0; i < toPush; ++i)
				{
					new(m_dataListTail) TYPE(std::move(elements[i]));
					move_tail_next();
				}
			}

			template <INSERT_POS POS>
			typename std::enable_if<POS == HEAD>::type enqueue_bulk_impl(TYPE* elements, unsigned int toPush)
			{
				for (unsigned int i = 0; i < toPush; ++i)
				{
					move_head_prev();
					new(m_dataListHead) TYPE(std::move(elements[toPush - i - 1]));
				}
			}

			template <INSERT_POS POS>
			typename std::enable_if<POS == TAIL>::type enqueue_bulk_impl(TYPE* part1, unsigned int count1, TYPE* part2, unsigned int count2)
			{
				for (unsigned int i = 0; i < count1; ++i)
				{
					new(m_dataListTail) TYPE(std::move(part1[i]));
					move_tail_next();
				}

				for (unsigned int i = 0; i < count2; ++i)
				{
					new(m_dataListTail) TYPE(std::move(part2[i]));
					move_tail_next();
				}
			}

			template <INSERT_POS POS>
			typename std::enable_if<POS == HEAD>::type enqueue_bulk_impl(TYPE* part1, unsigned int count1, TYPE* part2, unsigned int count2)
			{
				for (unsigned int i = 0; i < count1; ++i)
				{
					move_head_prev();
					new(m_dataListHead) TYPE(std::move(part1[count1 - i - 1]));
				}

				for (unsigned int i = 0; i < count2; ++i)
				{
					move_head_prev();
					new(m_dataListHead) TYPE(std::move(part2[count2 - i - 1]));
				}
			}

			template <INSERT_POS POS, typename... Args>
			void emplace_helper(std::unique_lock<std::mutex>& lock, Args &&...args)
			{
				m_size++;
				emplace_impl<POS>(std::forward<Args>(args)...);
				lock.unlock();
				m_notEmptyCond.notify_one();
			}

			template <INSERT_POS POS>
			unsigned int enqueue_bulk_helper(std::unique_lock<std::mutex>& lock, TYPE* elements, unsigned int count)
			{
				unsigned int toPush = std::min(count, m_capacity - m_size);
				m_size += toPush;
				enqueue_bulk_impl<POS>(elements, toPush);
				lock.unlock();

				if (HSLL_UNLIKELY(toPush == 1))
				{
					m_notEmptyCond.notify_one();
				}
				else
				{
					m_notEmptyCond.notify_all();
				}

				return toPush;
			}

			template <INSERT_POS POS>
			unsigned int enqueue_bulk_helper(std::unique_lock<std::mutex>& lock,
				TYPE* part1, unsigned int count1, TYPE* part2, unsigned int count2)
			{
				unsigned int toPush = std::min(count1 + count2, m_capacity - m_size);
				m_size += toPush;

				if (toPush > count1)
				{
					enqueue_bulk_impl<POS>(part1, count1, part2, toPush - count1);
				}
				else
				{
					enqueue_bulk_impl<POS>(part1, toPush);
				}

				lock.unlock();

				if (HSLL_UNLIKELY(toPush == 1))
				{
					m_notEmptyCond.notify_one();
				}
				else
				{
					m_notEmptyCond.notify_all();
				}

				return toPush;
			}

			void dequeue_helper(std::unique_lock<std::mutex>& lock, TYPE& element)
			{
				m_size -= 1;
				move_element(element, *m_dataListHead);
				move_head_next();
				lock.unlock();
				m_notFullCond.notify_one();
			}

			unsigned int dequeue_bulk_helper(std::unique_lock<std::mutex>& lock, TYPE* elements, unsigned int count)
			{
				unsigned int toPop = std::min(count, m_size);
				m_size -= toPop;

				for (unsigned int i = 0; i < toPop; ++i)
				{
					move_element(elements[i], *m_dataListHead);
					move_head_next();
				}

				lock.unlock();

				if (HSLL_UNLIKELY(toPop == 1))
				{
					m_notFullCond.notify_one();
				}
				else
				{
					m_notFullCond.notify_all();
				}

				return toPop;
			}

			void move_element(TYPE& dst, TYPE& src)
			{
				new(&dst) TYPE(std::move(src));
				src.~TYPE();
			}

			void wait_element()
			{
				for (unsigned int i = 0; i < m_maxSpin; ++i)
				{
					if (m_size)
					{
						return;
					}
				}

				std::this_thread::yield();
				return;
			}

		public:

			TPBlockQueue() : m_stopped(false), m_memBlock(nullptr) {}

			bool init(unsigned int capacity, unsigned int maxSpin = 5000)
			{
				assert(!m_memBlock);

				if (!capacity)
				{
					return false;
				}

				m_totalsize = (unsigned int)(sizeof(TYPE) * capacity);
				m_memBlock = HSLL_ALIGNED_MALLOC(m_totalsize, alignof(TYPE));

				if (!m_memBlock)
				{
					return false;
				}

				m_size = 0;
				m_maxSpin = maxSpin;
				m_capacity = capacity;
				m_dataListHead = (TYPE*)m_memBlock;
				m_dataListTail = (TYPE*)m_memBlock;
				m_border = (uintptr_t)m_memBlock + m_totalsize;
				return true;
			}

			template <INSERT_POS POS = TAIL, typename... Args>
			bool emplace(Args && ...args)
			{
				assert(m_memBlock);
				std::unique_lock<std::mutex> lock(m_dataMutex);

				if (HSLL_UNLIKELY(m_size == m_capacity))
				{
					return false;
				}

				emplace_helper<POS>(lock, std::forward<Args>(args)...);
				return true;
			}

			template <INSERT_POS POS = TAIL, typename... Args>
			bool wait_emplace(Args && ...args)
			{
				assert(m_memBlock);

				{
					std::unique_lock<std::mutex> lock(m_dataMutex);

					if (HSLL_LIKELY(m_size != m_capacity))
					{
						emplace_helper<POS>(lock, std::forward<Args>(args)...);
						return true;
					}
				}

				wait_element();
				std::unique_lock<std::mutex> lock(m_dataMutex);

				m_notFullCond.wait(lock, [this]
					{
						return HSLL_LIKELY(m_size != m_capacity) || HSLL_UNLIKELY(m_stopped);
					});

				if (HSLL_UNLIKELY(m_stopped))
				{
					return false;
				}

				emplace_helper<POS>(lock, std::forward<Args>(args)...);
				return true;
			}

			template <INSERT_POS POS = TAIL, typename Rep, typename Period, typename... Args>
			bool wait_emplace_for(const std::chrono::duration<Rep, Period>& timeout, Args && ...args)
			{
				return wait_emplace_until<POS>(std::chrono::steady_clock::now() + timeout, std::forward<Args>(args)...);
			}

			template <INSERT_POS POS = TAIL, typename Clock, typename Duration, typename... Args>
			bool wait_emplace_until(const std::chrono::time_point<Clock, Duration>& abs, Args && ...args)
			{
				assert(m_memBlock);

				{
					std::unique_lock<std::mutex> lock(m_dataMutex);

					if (HSLL_LIKELY(m_size != m_capacity))
					{
						emplace_helper<POS>(lock, std::forward<Args>(args)...);
						return true;
					}
				}

				wait_element();
				std::unique_lock<std::mutex> lock(m_dataMutex);

				bool success = m_notFullCond.wait_until(lock, abs, [this]
					{ return HSLL_LIKELY(m_size != m_capacity) || HSLL_UNLIKELY(m_stopped); });

				if (HSLL_UNLIKELY(!success || m_stopped))
				{
					return false;
				}

				emplace_helper<POS>(lock, std::forward<Args>(args)...);
				return true;
			}

			template <INSERT_POS POS = TAIL>
			unsigned int enqueue_bulk(TYPE* elements, unsigned int count)
			{
				assert(m_memBlock);
				assert(elements && count);
				std::unique_lock<std::mutex> lock(m_dataMutex);

				if (HSLL_UNLIKELY(!(m_capacity - m_size)))
				{
					return 0;
				}

				return enqueue_bulk_helper<POS>(lock, elements, count);
			}

			template <INSERT_POS POS = TAIL>
			unsigned int enqueue_bulk(TYPE* part1, unsigned int count1, TYPE* part2, unsigned int count2)
			{
				assert(m_memBlock);
				assert(part1 && count1);

				if (!part2 || !count2)
				{
					return enqueue_bulk<POS>(part1, count1);
				}

				std::unique_lock<std::mutex> lock(m_dataMutex);

				if (HSLL_UNLIKELY(!(m_capacity - m_size)))
				{
					return 0;
				}

				return enqueue_bulk_helper<POS>(lock, part1, count1, part2, count2);
			}

			template <INSERT_POS POS = TAIL>
			unsigned int wait_enqueue_bulk(TYPE* elements, unsigned int count)
			{
				assert(m_memBlock);
				assert(elements && count);

				{
					std::unique_lock<std::mutex> lock(m_dataMutex);

					if (HSLL_LIKELY(m_size != m_capacity))
					{
						return enqueue_bulk_helper<POS>(lock, elements, count);
					}
				}

				wait_element();
				std::unique_lock<std::mutex> lock(m_dataMutex);

				m_notFullCond.wait(lock, [this]
					{ return HSLL_LIKELY(m_size != m_capacity) || HSLL_UNLIKELY(m_stopped); });

				if (HSLL_UNLIKELY(m_stopped))
				{
					return 0;
				}

				return enqueue_bulk_helper<POS>(lock, elements, count);
			}

			template <INSERT_POS POS = TAIL, typename Rep, typename Period>
			unsigned int wait_enqueue_bulk_for(const std::chrono::duration<Rep, Period>& timeout, TYPE* elements, unsigned int count)
			{
				return  wait_enqueue_bulk_until<POS>(std::chrono::steady_clock::now() + timeout, elements, count);
			}

			template <INSERT_POS POS = TAIL, typename Clock, typename Duration>
			unsigned int wait_enqueue_bulk_until(const std::chrono::time_point<Clock, Duration>& abs, TYPE* elements, unsigned int count)
			{
				assert(m_memBlock);
				assert(elements && count);

				{
					std::unique_lock<std::mutex> lock(m_dataMutex);

					if (HSLL_LIKELY(m_size != m_capacity))
					{
						return enqueue_bulk_helper<POS>(lock, elements, count);
					}
				}

				wait_element();
				std::unique_lock<std::mutex> lock(m_dataMutex);

				bool success = m_notFullCond.wait_until(lock, abs, [this]
					{
						return HSLL_LIKELY(m_size != m_capacity) || HSLL_UNLIKELY(m_stopped);
					});

				if (HSLL_UNLIKELY(!success || m_stopped))
				{
					return 0;
				}

				return enqueue_bulk_helper<POS>(lock, elements, count);
			}

			bool dequeue(TYPE& element)
			{
				assert(m_memBlock);
				std::unique_lock<std::mutex> lock(m_dataMutex);

				if (HSLL_UNLIKELY(!m_size))
				{
					return false;
				}

				dequeue_helper(lock, element);
				return true;
			}

			bool wait_dequeue(TYPE& element)
			{
				assert(m_memBlock);

				{
					std::unique_lock<std::mutex> lock(m_dataMutex);

					if (HSLL_LIKELY(m_size))
					{
						dequeue_helper(lock, element);
						return true;
					}
				}

				wait_element();
				std::unique_lock<std::mutex> lock(m_dataMutex);

				m_notEmptyCond.wait(lock, [this]
					{ return HSLL_LIKELY(m_size) || HSLL_UNLIKELY(m_stopped); });

				if (HSLL_UNLIKELY(m_stopped))
				{
					return false;
				}

				dequeue_helper(lock, element);
				return true;
			}

			template <typename Rep, typename Period>
			bool wait_dequeue_for(const std::chrono::duration<Rep, Period>& timeout, TYPE& element)
			{
				return wait_dequeue_until(std::chrono::steady_clock::now() + timeout, element);
			}

			template <typename Clock, typename Duration>
			bool wait_dequeue_until(const std::chrono::time_point<Clock, Duration>& abs, TYPE& element)
			{
				assert(m_memBlock);

				{
					std::unique_lock<std::mutex> lock(m_dataMutex);

					if (HSLL_LIKELY(m_size))
					{
						dequeue_helper(lock, element);
						return true;
					}
				}

				wait_element();
				std::unique_lock<std::mutex> lock(m_dataMutex);

				bool success = m_notEmptyCond.wait_until(lock, abs, [this]
					{ return HSLL_LIKELY(m_size) || HSLL_UNLIKELY(m_stopped); });

				if (HSLL_UNLIKELY(!success || m_stopped))
				{
					return false;
				}

				dequeue_helper(lock, element);
				return true;
			}

			unsigned int dequeue_bulk(TYPE* elements, unsigned int count)
			{
				assert(m_memBlock);
				assert(elements && count);
				std::unique_lock<std::mutex> lock(m_dataMutex);

				if (HSLL_UNLIKELY(!m_size))
				{
					return 0;
				}

				return dequeue_bulk_helper(lock, elements, count);
			}

			unsigned int wait_dequeue_bulk(TYPE* elements, unsigned int count)
			{
				assert(m_memBlock);
				assert(elements && count);

				{
					std::unique_lock<std::mutex> lock(m_dataMutex);

					if (HSLL_LIKELY(m_size))
					{
						return dequeue_bulk_helper(lock, elements, count);
					}
				}

				wait_element();
				std::unique_lock<std::mutex> lock(m_dataMutex);

				m_notEmptyCond.wait(lock, [this]
					{ return HSLL_LIKELY(m_size) || HSLL_UNLIKELY(m_stopped); });

				if (HSLL_UNLIKELY(m_stopped))
				{
					return 0;
				}

				return dequeue_bulk_helper(lock, elements, count);
			}

			template <typename Rep, typename Period>
			unsigned int wait_dequeue_bulk_for(const std::chrono::duration<Rep, Period>& timeout, TYPE* elements, unsigned int count)
			{
				return wait_dequeue_bulk_until(std::chrono::steady_clock::now() + timeout, elements, count);
			}

			template <typename Clock, typename Duration>
			unsigned int wait_dequeue_bulk_until(const std::chrono::time_point<Clock, Duration>& abs, TYPE* elements, unsigned int count)
			{
				assert(m_memBlock);
				assert(elements && count);

				{
					std::unique_lock<std::mutex> lock(m_dataMutex);

					if (HSLL_LIKELY(m_size))
					{
						return dequeue_bulk_helper(lock, elements, count);
					}
				}

				wait_element();
				std::unique_lock<std::mutex> lock(m_dataMutex);

				bool success = m_notEmptyCond.wait_until(lock, abs, [this]
					{ return HSLL_LIKELY(m_size) || HSLL_UNLIKELY(m_stopped); });

				if (HSLL_UNLIKELY(!success || m_stopped))
				{
					return 0;
				}

				return dequeue_bulk_helper(lock, elements, count);
			}

			unsigned int get_size_weak()
			{
				assert(m_memBlock);
				return m_size;
			}

			unsigned int get_size_strong()
			{
				assert(m_memBlock);
				std::lock_guard<std::mutex> lock(m_dataMutex);
				return m_size;
			}

			unsigned int is_stopped_weak()
			{
				assert(m_memBlock);
				return m_stopped;
			}

			unsigned int is_stopped_strong()
			{
				assert(m_memBlock);
				std::lock_guard<std::mutex> lock(m_dataMutex);
				return m_stopped;
			}

			void disableWait()
			{
				assert(m_memBlock);

				{
					std::lock_guard<std::mutex> lock(m_dataMutex);
					m_stopped = true;
				}

				m_notFullCond.notify_all();
				m_notEmptyCond.notify_all();
			}

			void enableWait()
			{
				assert(m_memBlock);
				std::lock_guard<std::mutex> lock(m_dataMutex);
				m_stopped = false;
			}

			void release()
			{
				assert(m_memBlock);
				TYPE* current = m_dataListHead;

				for (unsigned int i = 0; i < m_size; ++i)
				{
					current->~TYPE();
					current = (TYPE*)((char*)current + sizeof(TYPE));

					if ((uintptr_t)(current) == m_border)
					{
						current = (TYPE*)(m_memBlock);
					}
				}

				HSLL_ALIGNED_FREE(m_memBlock);
				m_stopped = false;
				m_memBlock = nullptr;
			}

			~TPBlockQueue()
			{
				if (m_memBlock)
				{
					release();
				}
			}

			// Disable copying
			TPBlockQueue(const TPBlockQueue&) = delete;
			TPBlockQueue& operator=(const TPBlockQueue&) = delete;
		};
	}

	using INNER::INSERT_POS;
}

#endif // HSLL_TPBLOCKQUEUE