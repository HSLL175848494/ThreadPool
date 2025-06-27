#ifndef HSLL_TPBLOCKQUEUE
#define HSLL_TPBLOCKQUEUE

#include <new>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <assert.h>
#include <condition_variable>

namespace HSLL
{

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
#define ALIGNED_MALLOC(size, align) aligned_alloc(align,(size + align - 1) & ~(align - 1))
#define ALIGNED_FREE(ptr) free(ptr)
#endif

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
		static_assert(is_generic_ts<TYPE>::value, "TYPE must be a TaskStack type");

		// Insert position tags for zero-overhead dispatch
		struct InsertAtHeadTag
		{
		};

		struct InsertAtTailTag
		{
		};

		template <typename T>
		struct is_generic_dr : std::false_type
		{
		};

		template <typename Rep, typename Period>
		struct is_generic_dr<std::chrono::duration<Rep, Period>> : std::true_type
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
			static void construct(T& ptr, T& source)
			{
				new (&ptr) T(source);
			}
		};

		template <typename T>
		struct BulkConstructHelper<T, MOVE>
		{
			static void construct(T& ptr, T& source)
			{
				new (&ptr) T(std::move(source));
			}
		};

		template <BULK_CMETHOD Method, typename T>
		void bulk_construct(T& ptr, T& source)
		{
			BulkConstructHelper<T, Method>::construct(ptr, source);
		}

	private:
		// Memory management
		void* memoryBlock;		///< Raw memory block for element storage
		unsigned int isStopped; ///< Flag for stopping all operations

		// Queue state tracking
		unsigned int maxSize;	///< Capacity of the queue
		unsigned int totalsize; ///< Total allocated memory size
		std::atomic<unsigned int> size;		///< Current number of elements in queue

		// Buffer pointers
		TYPE* dataListHead; ///< Pointer to first element in queue
		TYPE* dataListTail; ///< Pointer to next insertion position
		uintptr_t border;	///< End address of allocated memory

		// Synchronization primitives
		std::mutex dataMutex;				  ///< Mutex protecting all queue operations
		std::condition_variable notEmptyCond; ///< Signaled when data becomes available
		std::condition_variable notFullCond;  ///< Signaled when space becomes available

		// Helper functions for pointer movement
		inline void move_tail_next()
		{
			dataListTail = (TYPE*)((char*)dataListTail + sizeof(TYPE));
			if (UNLIKELY((uintptr_t)dataListTail == border))
				dataListTail = (TYPE*)(uintptr_t)memoryBlock;
		}

		inline void move_head_next()
		{
			dataListHead = (TYPE*)((char*)dataListHead + sizeof(TYPE));
			if (UNLIKELY((uintptr_t)dataListHead == border))
				dataListHead = (TYPE*)(uintptr_t)memoryBlock;
		}

		// Reserve for head push
		inline void move_head_prev()
		{
			dataListHead = (TYPE*)((char*)dataListHead - sizeof(TYPE));
			if (UNLIKELY((uintptr_t)dataListHead < (uintptr_t)memoryBlock))
				dataListHead = (TYPE*)(border - sizeof(TYPE));
		}

		// Insert position implementations
		template <typename... Args>
		inline void emplace_impl(InsertAtTailTag, Args &&...args)
		{
			new (dataListTail) TYPE(std::forward<Args>(args)...);
			move_tail_next();
		}

		template <typename... Args>
		inline void emplace_impl(InsertAtHeadTag, Args &&...args)
		{
			move_head_prev();
			new (dataListHead) TYPE(std::forward<Args>(args)...);
		}

		template <class T>
		inline void push_impl(InsertAtTailTag, T&& element)
		{
			new (dataListTail) TYPE(std::forward<T>(element));
			move_tail_next();
		}

		template <class T>
		inline void push_impl(InsertAtHeadTag, T&& element)
		{
			move_head_prev();
			new (dataListHead) TYPE(std::forward<T>(element));
		}

		template <BULK_CMETHOD METHOD>
		inline void bulk_push_impl(InsertAtTailTag, TYPE* elements, unsigned int toPush)
		{
			for (unsigned int i = 0; i < toPush; ++i)
			{
				bulk_construct<METHOD>(*dataListTail, elements[i]);
				move_tail_next();
			}
		}

		template <BULK_CMETHOD METHOD>
		inline void bulk_push_impl(InsertAtHeadTag, TYPE* elements, unsigned int toPush)
		{
			for (unsigned int i = 0; i < toPush; ++i)
			{
				move_head_prev();
				bulk_construct<METHOD>(*dataListHead, elements[toPush - i - 1]);
			}
		}

		template <INSERT_POS POS, typename... Args>
		inline void emplace_helper(std::unique_lock<std::mutex>& lock, Args &&...args)
		{
			size.fetch_add(1, std::memory_order_release);
			using InsertTag = typename std::conditional<POS == HEAD, InsertAtHeadTag, InsertAtTailTag>::type;
			emplace_impl(InsertTag(), std::forward<Args>(args)...);
			lock.unlock();
			notEmptyCond.notify_one();
		}

		template <INSERT_POS POS, class T>
		inline void push_helper(std::unique_lock<std::mutex>& lock, T&& element)
		{
			size.fetch_add(1, std::memory_order_release);
			using InsertTag = typename std::conditional<POS == HEAD, InsertAtHeadTag, InsertAtTailTag>::type;
			push_impl(InsertTag(), std::forward<T>(element));
			lock.unlock();
			notEmptyCond.notify_one();
		}

		template <BULK_CMETHOD METHOD, INSERT_POS POS>
		inline unsigned int pushBulk_helper(std::unique_lock<std::mutex>& lock, TYPE* elements, unsigned int count)
		{
			unsigned int toPush = std::min(count, maxSize - size.load(std::memory_order_relaxed));
			size.fetch_add(toPush, std::memory_order_release);
			using InsertTag = typename std::conditional<POS == HEAD, InsertAtHeadTag, InsertAtTailTag>::type;
			bulk_push_impl<METHOD>(InsertTag(), elements, toPush);
			lock.unlock();

			if (UNLIKELY(toPush == 1))
				notEmptyCond.notify_one();
			else
				notEmptyCond.notify_all();
			return toPush;
		}

		inline void pop_helper(std::unique_lock<std::mutex>& lock, TYPE& element)
		{
			size.fetch_sub(1, std::memory_order_release);
			move_element_pop(element, *dataListHead);
			move_head_next();
			lock.unlock();
			notFullCond.notify_one();
		}

		inline unsigned int popbulk_helper(std::unique_lock<std::mutex>& lock, TYPE* elements, unsigned int count)
		{
			unsigned int toPop = std::min(count, size.load(std::memory_order_relaxed));
			size.fetch_sub(toPop, std::memory_order_release);
			for (unsigned int i = 0; i < toPop; ++i)
			{
				move_element_pop(elements[i], *dataListHead);
				move_head_next();
			}
			lock.unlock();

			if (UNLIKELY(toPop == 1))
				notFullCond.notify_one();
			else
				notFullCond.notify_all();
			return toPop;
		}

		inline void move_element_push(TYPE& dst, TYPE& src)
		{
			new (&dst) TYPE(std::move(src));
			src.~TYPE();
		}

		inline void move_element_pop(TYPE& dst, TYPE& src)
		{
			new (&dst) TYPE(std::move(src));
			src.~TYPE();
		}

	public:

		TPBlockQueue() : memoryBlock(nullptr), isStopped(0) {}

		~TPBlockQueue() { release(); }

		/**
		 * @brief Initializes queue with fixed capacity
		 */ 
		bool init(unsigned int capacity)
		{
			if (memoryBlock || !capacity)
				return false;

			totalsize = sizeof(TYPE) * capacity;
			memoryBlock = ALIGNED_MALLOC(totalsize, std::max(alignof(TYPE), (size_t)64));

			if (!memoryBlock)
				return false;

			size = 0;
			maxSize = capacity;
			dataListHead = (TYPE*)memoryBlock;
			dataListTail = (TYPE*)memoryBlock;
			border = (uintptr_t)memoryBlock + totalsize;

			return true;
		}

		/**
		 * @brief Non-blocking element emplacement with perfect forwarding
		 */
		template <INSERT_POS POS = TAIL, typename... Args>
		bool emplace(Args &&...args)
		{
			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(size.load(std::memory_order_relaxed) == maxSize))
				return false;

			emplace_helper<POS>(lock, std::forward<Args>(args)...);
			return true;
		}

		/**
		 * @brief Blocking element emplacement with indefinite wait
		 */
		template <INSERT_POS POS = TAIL, typename... Args>
		typename std::enable_if<!is_generic_dr<typename std::tuple_element<0, std::tuple<Args...>>::type>::value, bool>::type
			wait_emplace(Args &&...args)
		{
			std::unique_lock<std::mutex> lock(dataMutex);

			notFullCond.wait(lock, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed) != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return false;

			emplace_helper<POS>(lock, std::forward<Args>(args)...);
			return true;
		}

		/**
		 * @brief Blocking element emplacement with timeout
		 */
		template <INSERT_POS POS = TAIL, class Rep, class Period, typename... Args>
		bool wait_emplace(const std::chrono::duration<Rep, Period>& timeout, Args &&...args)
		{
			std::unique_lock<std::mutex> lock(dataMutex);

			bool success = notFullCond.wait_for(lock, timeout, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed) != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || isStopped))
				return false;

			emplace_helper<POS>(lock, std::forward<Args>(args)...);
			return true;
		}

		/**
		 * @brief Non-blocking element push
		 */
		template <INSERT_POS POS = TAIL, class T>
		bool push(T&& element)
		{
			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(size.load(std::memory_order_relaxed) == maxSize))
				return false;

			push_helper<POS>(lock, std::forward<T>(element));
			return true;
		}

		/**
		 * @brief Blocking element push with indefinite wait
		 */
		template <INSERT_POS POS = TAIL, class T>
		bool wait_push(T&& element)
		{
			std::unique_lock<std::mutex> lock(dataMutex);

			notFullCond.wait(lock, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed) != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return false;

			push_helper<POS>(lock, std::forward<T>(element));
			return true;
		}

		/**
		 * @brief Blocking element push with timeout
		 */
		template <INSERT_POS POS = TAIL, class T, class Rep, class Period>
		bool wait_push(T&& element, const std::chrono::duration<Rep, Period>& timeout)
		{
			std::unique_lock<std::mutex> lock(dataMutex);

			bool success = notFullCond.wait_for(lock, timeout, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed) != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || isStopped))
				return false;

			push_helper<POS>(lock, std::forward<T>(element));
			return true;
		}

		/**
		 * @brief Bulk push for multiple elements
		 */
		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL>
		unsigned int pushBulk(TYPE* elements, unsigned int count)
		{
			assert(count);

			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(!(maxSize - size.load(std::memory_order_relaxed))))
				return 0;

			return pushBulk_helper<METHOD, POS>(lock, elements, count);
		}

		/**
		 * @brief Blocking bulk push with indefinite wait
		 */
		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL>
		unsigned int wait_pushBulk(TYPE* elements, unsigned int count)
		{
			assert(count);

			std::unique_lock<std::mutex> lock(dataMutex);

			notFullCond.wait(lock, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed) != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return 0;

			return pushBulk_helper<METHOD, POS>(lock, elements, count);
		}

		/**
		 * @brief Blocking bulk push with timeout
		 */
		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL, class Rep, class Period>
		unsigned int wait_pushBulk(TYPE* elements, unsigned int count, const std::chrono::duration<Rep, Period>& timeout)
		{
			assert(count);

			std::unique_lock<std::mutex> lock(dataMutex);

			bool success = notFullCond.wait_for(lock, timeout, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed) != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || isStopped))
				return 0;

			return pushBulk_helper<METHOD, POS>(lock, elements, count);
		}

		/**
		 * @brief Non-blocking element removal
		 */
		bool pop(TYPE& element)
		{
			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(!size.load(std::memory_order_relaxed)))
				return false;

			pop_helper(lock, element);
			return true;
		}

		/**
		 * @brief Blocking element removal with indefinite wait
		 */
		bool wait_pop(TYPE& element)
		{
			std::unique_lock<std::mutex> lock(dataMutex);

			notEmptyCond.wait(lock, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed)) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return false;

			pop_helper(lock, element);
			return true;
		}

		/**
		 * @brief Blocking element removal with timeout
		 */
		template <class Rep, class Period>
		bool wait_pop(TYPE& element, const std::chrono::duration<Rep, Period>& timeout)
		{
			std::unique_lock<std::mutex> lock(dataMutex);

			bool success = notEmptyCond.wait_for(lock, timeout, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed)) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || isStopped))
				return false;

			pop_helper(lock, element);
			return true;
		}

		/**
		 * @brief Bulk element retrieval
		 */
		unsigned int popBulk(TYPE* elements, unsigned int count)
		{
			assert(count);

			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(!size.load(std::memory_order_relaxed)))
				return 0;

			return popbulk_helper(lock, elements, count);
		}

		/**
		 * @brief Blocking bulk retrieval with indefinite wait
		 */
		unsigned int wait_popBulk(TYPE* elements, unsigned int count)
		{
			assert(count);

			std::unique_lock<std::mutex> lock(dataMutex);

			notEmptyCond.wait(lock, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed)) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return 0;

			return popbulk_helper(lock, elements, count);
		}

		/**
		 * @brief Blocking bulk retrieval with timeout
		 */
		template <class Rep, class Period>
		unsigned int wait_popBulk(TYPE* elements, unsigned int count, const std::chrono::duration<Rep, Period>& timeout)
		{
			assert(count);

			std::unique_lock<std::mutex> lock(dataMutex);
			bool success = notEmptyCond.wait_for(lock, timeout, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed)) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || isStopped))
				return 0;

			return popbulk_helper(lock, elements, count);
		}

		unsigned int get_size()
		{
			return size.load(std::memory_order_relaxed);
		}

		unsigned int is_Stopped()
		{
			return isStopped;
		}

		unsigned int get_exact_size()
		{
			return size.load(std::memory_order_acquire);
		}

		/**
		 * @brief Signals all waiting threads to stop blocking
		 */
		void stopWait()
		{
			{
				std::lock_guard<std::mutex> lock(dataMutex);
				isStopped = 1;
			}

			notEmptyCond.notify_all();
			notFullCond.notify_all();
		}

		/**
		 * @brief Releases all resources and resets queue state
		 */
		void release()
		{
			if (memoryBlock)
			{
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
				maxSize = 0;
				isStopped = 0;
				memoryBlock = nullptr;
				dataListHead = nullptr;
				dataListTail = nullptr;
			}
		}

		// Disable copying
		TPBlockQueue(const TPBlockQueue&) = delete;
		TPBlockQueue& operator=(const TPBlockQueue&) = delete;
	};
}
#endif // HSLL_TPBLOCKQUEUE