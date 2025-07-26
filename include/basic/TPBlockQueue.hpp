#ifndef HSLL_TPBLOCKQUEUE
#define HSLL_TPBLOCKQUEUE

#include <atomic>
#include <assert.h>
#include <condition_variable>

#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

#if defined(_WIN32)
#include <malloc.h>
#define ALIGNED_MALLOC(size, align) _aligned_malloc(size, align)
#define ALIGNED_FREE(ptr) _aligned_free(ptr)
#else
#include <stdlib.h>
#if !defined(_ISOC11_SOURCE) && !defined(__APPLE__)
#define ALIGNED_MALLOC(size, align) ({ \
    void* ptr = NULL; \
    if (posix_memalign(&ptr, align, size) != 0) ptr = NULL; \
    ptr; \
})
#else
#define ALIGNED_MALLOC(size, align) aligned_alloc(align, (size + align - 1) & ~(size_t)(align - 1))
#endif
#define ALIGNED_FREE(ptr) free(ptr)
#endif

namespace HSLL
{
	/**
	 * @brief Enumeration defining the method of bulk construction
	 */
	enum BULK_CMETHOD
	{
		COPY, ///< Use copy construction semantics
		MOVE  ///< Use move construction semantics
	};

	/**
	 * @brief Enumeration defining the insertion position
	 */
	enum INSERT_POS
	{
		TAIL, ///< Insert at the tail (default)
		HEAD  ///< Insert at the head
	};

	/**
	 * @brief Circular buffer based blocking queue implementation
	 */
	template <class TYPE>
	class alignas(64) TPBlockQueue
	{
		template <typename T>
		struct is_duration : std::false_type
		{
		};

		template <typename Rep, typename Period>
		struct is_duration<std::chrono::duration<Rep, Period>> : std::true_type
		{
		};

		/**
		 * @brief Helper template for bulk construction (copy/move)
		 */
		template <typename T, BULK_CMETHOD Method>
		struct BulkConstructHelper;

		template <typename T>
		struct BulkConstructHelper<T, COPY>
		{
			static void construct(T& dst, T& src)
			{
				new (&dst) T(src);
			}
		};

		template <typename T>
		struct BulkConstructHelper<T, MOVE>
		{
			static void construct(T& dst, T& src)
			{
				new (&dst) T(std::move(src));
			}
		};

		template <BULK_CMETHOD Method, typename T>
		void bulk_construct(T& dst, T& src)
		{
			BulkConstructHelper<T, Method>::construct(dst, src);
		}

	private:
		// Memory management
		void* memoryBlock;		///< Raw memory block for element storage
		unsigned int isStopped; ///< Flag for stopping all operations

		// Queue state tracking
		unsigned int size;		///< Current number of elements in queue
		unsigned int maxSpin;
		unsigned int capacity;	///< Capacity of the queue
		unsigned int totalsize; ///< Total allocated memory size

		// Buffer pointers
		TYPE* dataListHead; ///< Pointer to first element in queue
		TYPE* dataListTail; ///< Pointer to next insertion position
		uintptr_t border;	///< End address of allocated memory

		// Synchronization primitives
		std::mutex dataMutex;				  ///< Mutex protecting all queue operations
		std::condition_variable notEmptyCond; ///< Signaled when data becomes available
		std::condition_variable notFullCond;  ///< Signaled when space becomes available

		void move_tail_next()
		{
			dataListTail = (TYPE*)((char*)dataListTail + sizeof(TYPE));
			if (UNLIKELY((uintptr_t)dataListTail == border))
				dataListTail = (TYPE*)(uintptr_t)memoryBlock;
		}

		void move_head_next()
		{
			dataListHead = (TYPE*)((char*)dataListHead + sizeof(TYPE));
			if (UNLIKELY((uintptr_t)dataListHead == border))
				dataListHead = (TYPE*)(uintptr_t)memoryBlock;
		}

		// Reserve for head push
		void move_head_prev()
		{
			dataListHead = (TYPE*)((char*)dataListHead - sizeof(TYPE));
			if (UNLIKELY((uintptr_t)dataListHead < (uintptr_t)memoryBlock))
				dataListHead = (TYPE*)(border - sizeof(TYPE));
		}

		// Insert position implementations
		template <INSERT_POS POS, typename... Args>
		typename std::enable_if<POS == TAIL>::type emplace_impl(Args &&...args)
		{
			new (dataListTail) TYPE(std::forward<Args>(args)...);
			move_tail_next();
		}

		template <INSERT_POS POS, typename... Args>
		typename std::enable_if<POS == HEAD>::type emplace_impl(Args &&...args)
		{
			move_head_prev();
			new (dataListHead) TYPE(std::forward<Args>(args)...);
		}

		template <INSERT_POS POS, class T>
		typename std::enable_if<POS == TAIL>::type enqueue_impl(T&& element)
		{
			new (dataListTail) TYPE(std::forward<T>(element));
			move_tail_next();
		}

		template <INSERT_POS POS, class T>
		typename std::enable_if<POS == HEAD>::type enqueue_impl(T&& element)
		{
			move_head_prev();
			new (dataListHead) TYPE(std::forward<T>(element));
		}

		template <INSERT_POS POS, BULK_CMETHOD METHOD>
		typename std::enable_if<POS == TAIL>::type enqueue_bulk_impl(TYPE* elements, unsigned int toPush)
		{
			for (unsigned int i = 0; i < toPush; ++i)
			{
				bulk_construct<METHOD>(*dataListTail, elements[i]);
				move_tail_next();
			}
		}

		template <INSERT_POS POS, BULK_CMETHOD METHOD>
		typename std::enable_if<POS == HEAD>::type enqueue_bulk_impl(TYPE* elements, unsigned int toPush)
		{
			for (unsigned int i = 0; i < toPush; ++i)
			{
				move_head_prev();
				bulk_construct<METHOD>(*dataListHead, elements[toPush - i - 1]);
			}
		}

		template <INSERT_POS POS, BULK_CMETHOD METHOD>
		typename std::enable_if<POS == TAIL>::type enqueue_bulk_impl(TYPE* part1, unsigned int count1, TYPE* part2, unsigned int count2)
		{
			for (unsigned int i = 0; i < count1; ++i)
			{
				bulk_construct<METHOD>(*dataListTail, part1[i]);
				move_tail_next();
			}

			for (unsigned int i = 0; i < count2; ++i)
			{
				bulk_construct<METHOD>(*dataListTail, part2[i]);
				move_tail_next();
			}
		}

		template <INSERT_POS POS, BULK_CMETHOD METHOD>
		typename std::enable_if<POS == HEAD>::type enqueue_bulk_impl(TYPE* part1, unsigned int count1, TYPE* part2, unsigned int count2)
		{
			for (unsigned int i = 0; i < count1; ++i)
			{
				move_head_prev();
				bulk_construct<METHOD>(*dataListHead, part1[count1 - i - 1]);
			}

			for (unsigned int i = 0; i < count2; ++i)
			{
				move_head_prev();
				bulk_construct<METHOD>(*dataListHead, part2[count2 - i - 1]);
			}
		}

		template <INSERT_POS POS, typename... Args>
		void emplace_helper(std::unique_lock<std::mutex>& lock, Args &&...args)
		{
			size += 1;
			emplace_impl<POS>(std::forward<Args>(args)...);
			lock.unlock();
			notEmptyCond.notify_one();
		}

		template <INSERT_POS POS, class T>
		void enqueue_helper(std::unique_lock<std::mutex>& lock, T&& element)
		{
			size += 1;
			enqueue_impl<POS>(std::forward<T>(element));
			lock.unlock();
			notEmptyCond.notify_one();
		}

		template <BULK_CMETHOD METHOD, INSERT_POS POS>
		unsigned int enqueue_bulk_helper(std::unique_lock<std::mutex>& lock, TYPE* elements, unsigned int count)
		{
			unsigned int toPush = std::min(count, capacity - size);
			size += toPush;
			enqueue_bulk_impl<POS, METHOD>(elements, toPush);
			lock.unlock();

			if (UNLIKELY(toPush == 1))
				notEmptyCond.notify_one();
			else
				notEmptyCond.notify_all();
			return toPush;
		}

		template <BULK_CMETHOD METHOD, INSERT_POS POS>
		unsigned int enqueue_bulk_helper(std::unique_lock<std::mutex>& lock,
			TYPE* part1, unsigned int count1, TYPE* part2, unsigned int count2)
		{
			unsigned int toPush = std::min(count1 + count2, capacity - size);
			size += toPush;

			if (toPush > count1)
				enqueue_bulk_impl<POS, METHOD>(part1, count1, part2, toPush - count1);
			else
				enqueue_bulk_impl<POS, METHOD>(part1, toPush);

			lock.unlock();

			if (UNLIKELY(toPush == 1))
				notEmptyCond.notify_one();
			else
				notEmptyCond.notify_all();
			return toPush;
		}

		void dequeue_helper(std::unique_lock<std::mutex>& lock, TYPE& element)
		{
			size -= 1;
			move_element_dequeue(element, *dataListHead);
			move_head_next();
			lock.unlock();
			notFullCond.notify_one();
		}

		unsigned int dequeue_bulk_helper(std::unique_lock<std::mutex>& lock, TYPE* elements, unsigned int count)
		{
			unsigned int toPop = std::min(count, size);
			size -= toPop;
			for (unsigned int i = 0; i < toPop; ++i)
			{
				move_element_dequeue(elements[i], *dataListHead);
				move_head_next();
			}
			lock.unlock();

			if (UNLIKELY(toPop == 1))
				notFullCond.notify_one();
			else
				notFullCond.notify_all();
			return toPop;
		}

		void move_element_enqueue(TYPE& dst, TYPE& src)
		{
			new (&dst) TYPE(std::move(src));
			src.~TYPE();
		}

		void move_element_dequeue(TYPE& dst, TYPE& src)
		{
			new (&dst) TYPE(std::move(src));
			src.~TYPE();
		}

		void spin()
		{
			for (unsigned int i = 0; i < maxSpin; ++i)
			{
				if (size)
					return;
			}
			return;
		}

	public:

		TPBlockQueue() : memoryBlock(nullptr), isStopped(0) {}

		bool init(unsigned int capacity, unsigned int spin = 2000)
		{
			if (memoryBlock || !capacity)
				return false;

			totalsize = sizeof(TYPE) * capacity;
			memoryBlock = ALIGNED_MALLOC(totalsize, std::max(alignof(TYPE), (size_t)64));

			if (!memoryBlock)
				return false;

			size = 0;
			maxSpin = spin;
			this->capacity = capacity;
			dataListHead = (TYPE*)memoryBlock;
			dataListTail = (TYPE*)memoryBlock;
			border = (uintptr_t)memoryBlock + totalsize;
			return true;
		}

		template <INSERT_POS POS = TAIL, typename... Args>
		bool emplace(Args &&...args)
		{
			assert(memoryBlock);

			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(size == capacity))
				return false;

			emplace_helper<POS>(lock, std::forward<Args>(args)...);
			return true;
		}

		template <INSERT_POS POS = TAIL, typename... Args>
		typename std::enable_if<!is_duration<typename std::tuple_element<0, std::tuple<Args...>>::type>::value, bool>::type
			wait_emplace(Args &&...args)
		{
			assert(memoryBlock);
			spin();
			std::unique_lock<std::mutex> lock(dataMutex);

			notFullCond.wait(lock, [this]
				{ return LIKELY(size != capacity) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return false;

			emplace_helper<POS>(lock, std::forward<Args>(args)...);
			return true;
		}

		template <INSERT_POS POS = TAIL, class Rep, class Period, typename... Args>
		bool wait_emplace(const std::chrono::duration<Rep, Period>& timeout, Args &&...args)
		{
			assert(memoryBlock);
			spin();
			std::unique_lock<std::mutex> lock(dataMutex);

			bool success = notFullCond.wait_for(lock, timeout, [this]
				{ return LIKELY(size != capacity) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || isStopped))
				return false;

			emplace_helper<POS>(lock, std::forward<Args>(args)...);
			return true;
		}

		template <INSERT_POS POS = TAIL, class T>
		bool enqueue(T&& element)
		{
			assert(memoryBlock);
			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(size == capacity))
				return false;

			enqueue_helper<POS>(lock, std::forward<T>(element));
			return true;
		}

		template <INSERT_POS POS = TAIL, class T>
		bool wait_enqueue(T&& element)
		{
			assert(memoryBlock);
			spin();
			std::unique_lock<std::mutex> lock(dataMutex);

			notFullCond.wait(lock, [this]
				{ return LIKELY(size != capacity) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return false;

			enqueue_helper<POS>(lock, std::forward<T>(element));
			return true;
		}

		template <INSERT_POS POS = TAIL, class T, class Rep, class Period>
		bool wait_enqueue(T&& element, const std::chrono::duration<Rep, Period>& timeout)
		{
			assert(memoryBlock);
			spin();
			std::unique_lock<std::mutex> lock(dataMutex);

			bool success = notFullCond.wait_for(lock, timeout, [this]
				{ return LIKELY(size != capacity) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || isStopped))
				return false;

			enqueue_helper<POS>(lock, std::forward<T>(element));
			return true;
		}

		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL>
		unsigned int enqueue_bulk(TYPE* elements, unsigned int count)
		{
			assert(memoryBlock);
			assert(elements && count);
			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(!(capacity - size)))
				return 0;

			return enqueue_bulk_helper<METHOD, POS>(lock, elements, count);
		}

		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL>
		unsigned int enqueue_bulk(TYPE* part1, unsigned int count1, TYPE* part2, unsigned int count2)
		{
			assert(memoryBlock);
			assert(part1 && count1);

			if (!part2 || !count2)
				return enqueue_bulk<METHOD, POS>(part1, count1);

			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(!(capacity - size)))
				return 0;

			return enqueue_bulk_helper<METHOD, POS>(lock, part1, count1, part2, count2);
		}

		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL>
		unsigned int wait_enqueue_bulk(TYPE* elements, unsigned int count)
		{
			assert(memoryBlock);
			assert(elements && count);
			spin();
			std::unique_lock<std::mutex> lock(dataMutex);

			notFullCond.wait(lock, [this]
				{ return LIKELY(size != capacity) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return 0;

			return enqueue_bulk_helper<METHOD, POS>(lock, elements, count);
		}

		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL, class Rep, class Period>
		unsigned int wait_enqueue_bulk(TYPE* elements, unsigned int count, const std::chrono::duration<Rep, Period>& timeout)
		{
			assert(memoryBlock);
			assert(elements && count);
			spin();
			std::unique_lock<std::mutex> lock(dataMutex);

			bool success = notFullCond.wait_for(lock, timeout, [this]
				{ return LIKELY(size != capacity) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || isStopped))
				return 0;

			return enqueue_bulk_helper<METHOD, POS>(lock, elements, count);
		}

		bool dequeue(TYPE& element)
		{
			assert(memoryBlock);
			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(!size))
				return false;

			dequeue_helper(lock, element);
			return true;
		}

		bool wait_dequeue(TYPE& element)
		{
			assert(memoryBlock);
			spin();
			std::unique_lock<std::mutex> lock(dataMutex);

			notEmptyCond.wait(lock, [this]
				{ return LIKELY(size) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return false;

			dequeue_helper(lock, element);
			return true;
		}

		template <class Rep, class Period>
		bool wait_dequeue(TYPE& element, const std::chrono::duration<Rep, Period>& timeout)
		{
			assert(memoryBlock);
			spin();
			std::unique_lock<std::mutex> lock(dataMutex);

			bool success = notEmptyCond.wait_for(lock, timeout, [this]
				{ return LIKELY(size) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || isStopped))
				return false;

			dequeue_helper(lock, element);
			return true;
		}

		unsigned int dequeue_bulk(TYPE* elements, unsigned int count)
		{
			assert(memoryBlock);
			assert(elements && count);
			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(!size))
				return 0;

			return dequeue_bulk_helper(lock, elements, count);
		}

		unsigned int wait_dequeue_bulk(TYPE* elements, unsigned int count)
		{
			assert(memoryBlock);
			assert(elements && count);
			spin();
			std::unique_lock<std::mutex> lock(dataMutex);

			notEmptyCond.wait(lock, [this]
				{ return LIKELY(size) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return 0;

			return dequeue_bulk_helper(lock, elements, count);
		}

		template <class Rep, class Period>
		unsigned int wait_dequeue_bulk(TYPE* elements, unsigned int count, const std::chrono::duration<Rep, Period>& timeout)
		{
			assert(memoryBlock);
			assert(elements && count);
			spin();
			std::unique_lock<std::mutex> lock(dataMutex);

			bool success = notEmptyCond.wait_for(lock, timeout, [this]
				{ return LIKELY(size) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || isStopped))
				return 0;

			return dequeue_bulk_helper(lock, elements, count);
		}

		unsigned int get_size()
		{
			assert(memoryBlock);
			return size;
		}

		unsigned int is_Stopped()
		{
			assert(memoryBlock);
			return isStopped;
		}

		unsigned long long get_bsize()
		{
			assert(memoryBlock);
			return (unsigned long long)border - (unsigned long long)memoryBlock;
		}

		void stopWait()
		{
			{
				std::lock_guard<std::mutex> lock(dataMutex);
				isStopped = 1;
			}

			notEmptyCond.notify_all();
			notFullCond.notify_all();
		}

		void enableWait()
		{
			std::lock_guard<std::mutex> lock(dataMutex);
			isStopped = 0;
		}

		void release()
		{
			assert(memoryBlock);
			TYPE* current = dataListHead;
			for (unsigned int i = 0; i < size; ++i)
			{
				current->~TYPE();
				current = (TYPE*)((char*)current + sizeof(TYPE));
				if ((uintptr_t)(current) == border)
					current = (TYPE*)(memoryBlock);
			}

			ALIGNED_FREE(memoryBlock);

			size = 0;
			capacity = 0;
			isStopped = 0;
			memoryBlock = nullptr;
			dataListHead = nullptr;
			dataListTail = nullptr;
		}

		~TPBlockQueue()
		{
			if (memoryBlock)
				release();
		}

		// Disable copying
		TPBlockQueue(const TPBlockQueue&) = delete;
		TPBlockQueue& operator=(const TPBlockQueue&) = delete;
	};
}
#endif // HSLL_TPBLOCKQUEUE