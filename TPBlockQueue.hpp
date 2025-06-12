#ifndef HSLL_TPBLOCKQUEUE
#define HSLL_TPBLOCKQUEUE

#include <new>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <cstdint>

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
#define ALIGNED_MALLOC(size, align) aligned_alloc(align, size)
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

	// Insert position tags for zero-overhead dispatch
	struct InsertAtHeadTag
	{
	};

	struct InsertAtTailTag
	{
	};

	/**
	 * @brief Helper template for conditional object destruction
	 */
	template <typename T, bool IsTrivial = std::is_trivially_destructible<T>::value>
	struct DestroyHelper
	{
		static void destroy(T &obj) { obj.~T(); }
	};

	template <typename T>
	struct DestroyHelper<T, true>
	{
		static void destroy(T &) {}
	};

	template <typename T>
	void conditional_destroy(T &obj)
	{
		DestroyHelper<T>::destroy(obj);
	}

	/**
	 * @brief Helper template for bulk construction (copy/move)
	 */
	template <typename T, BULK_CMETHOD Method>
	struct BulkConstructHelper;

	template <typename T>
	struct BulkConstructHelper<T, COPY>
	{
		static void construct(T &ptr, T &source)
		{
			new (&ptr) T(source);
		}
	};

	template <typename T>
	struct BulkConstructHelper<T, MOVE>
	{
		static void construct(T &ptr, T &source)
		{
			new (&ptr) T(std::move(source));
		}
	};

	template <BULK_CMETHOD Method, typename T>
	void bulk_construct(T &ptr, T &source)
	{
		BulkConstructHelper<T, Method>::construct(ptr, source);
	}

	/**
	 * @brief Circular buffer based blocking queue implementation
	 */
	template <class TYPE>
	class TPBlockQueue
	{
		template <class T>
		friend class ThreadPool;

		template <typename T>
		struct is_duration : std::false_type
		{
		};

		template <typename Rep, typename Period>
		struct is_duration<std::chrono::duration<Rep, Period>> : std::true_type
		{
		};

	private:
		// Memory management
		void *memoryBlock;		///< Raw memory block for element storage
		unsigned int isStopped; ///< Flag for stopping all operations

		// Queue state tracking
		unsigned int size;		///< Current number of elements in queue
		unsigned int maxSize;	///< Capacity of the queue
		unsigned int totalsize; ///< Total allocated memory size

		// Buffer pointers
		TYPE *dataListHead; ///< Pointer to first element in queue
		TYPE *dataListTail; ///< Pointer to next insertion position
		uintptr_t border;	///< End address of allocated memory

		// Synchronization primitives
		std::mutex dataMutex;				  ///< Mutex protecting all queue operations
		std::condition_variable notEmptyCond; ///< Signaled when data becomes available
		std::condition_variable notFullCond;  ///< Signaled when space becomes available

		// Helper functions for pointer movement
		inline void move_tail_next()
		{
			dataListTail = (TYPE *)((char *)dataListTail + sizeof(TYPE));
			if (UNLIKELY((uintptr_t)dataListTail == border))
				dataListTail = (TYPE *)(uintptr_t)memoryBlock;
		}

		inline void move_head_next()
		{
			dataListHead = (TYPE *)((char *)dataListHead + sizeof(TYPE));
			if (UNLIKELY((uintptr_t)dataListHead == border))
				dataListHead = (TYPE *)(uintptr_t)memoryBlock;
		}

		// Reserve for head push
		inline void move_head_prev()
		{
			dataListHead = (TYPE *)((char *)dataListHead - sizeof(TYPE));
			if (UNLIKELY((uintptr_t)dataListHead < (uintptr_t)memoryBlock))
				dataListHead = (TYPE *)(border - sizeof(TYPE));
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
		inline void push_impl(InsertAtTailTag, T &&element)
		{
			new (dataListTail) TYPE(std::forward<T>(element));
			move_tail_next();
		}

		template <class T>
		inline void push_impl(InsertAtHeadTag, T &&element)
		{
			move_head_prev();
			new (dataListHead) TYPE(std::forward<T>(element));
		}

		template <BULK_CMETHOD METHOD>
		inline void bulk_push_impl(InsertAtTailTag, TYPE *elements, unsigned int toPush)
		{
			for (unsigned int i = 0; i < toPush; ++i)
			{
				bulk_construct<METHOD>(*dataListTail, elements[i]);
				move_tail_next();
			}
		}

		template <BULK_CMETHOD METHOD>
		inline void bulk_push_impl(InsertAtHeadTag, TYPE *elements, unsigned int toPush)
		{
			for (unsigned int i = 0; i < toPush; ++i)
			{
				move_head_prev();
				bulk_construct<METHOD>(*dataListHead, elements[toPush - i - 1]);
			}
		}

		template <INSERT_POS POS, typename... Args>
		void emplace_helper(std::unique_lock<std::mutex> &lock, Args &&...args)
		{
			size++;
			using InsertTag = typename std::conditional<POS == HEAD, InsertAtHeadTag, InsertAtTailTag>::type;
			emplace_impl(InsertTag(), std::forward<Args>(args)...);
			lock.unlock();
			notEmptyCond.notify_one();
		}

		template <INSERT_POS POS, class T>
		void push_helper(std::unique_lock<std::mutex> &lock, T &&element)
		{
			size++;
			using InsertTag = typename std::conditional<POS == HEAD, InsertAtHeadTag, InsertAtTailTag>::type;
			push_impl(InsertTag(), std::forward<T>(element));
			lock.unlock();
			notEmptyCond.notify_one();
		}

		template <BULK_CMETHOD METHOD, INSERT_POS POS>
		unsigned int pushBulk_helper(std::unique_lock<std::mutex> &lock, TYPE *elements, unsigned int count)
		{
			unsigned int toPush = std::min(count, maxSize - size);
			size += toPush;
			using InsertTag = typename std::conditional<POS == HEAD, InsertAtHeadTag, InsertAtTailTag>::type;
			bulk_push_impl<METHOD>(InsertTag(), elements, toPush);
			lock.unlock();

			if (UNLIKELY(toPush == 1))
				notEmptyCond.notify_one();
			else
				notEmptyCond.notify_all();
			return toPush;
		}

		void pop_helper(std::unique_lock<std::mutex> &lock, TYPE &element)
		{
			size--;
			move_element(element, *dataListHead);
			move_head_next();
			lock.unlock();
			notFullCond.notify_one();
		}

		unsigned int popbulk_helper(std::unique_lock<std::mutex> &lock, TYPE *elements, unsigned int count)
		{
			unsigned int toPop = std::min(count, size);
			size -= toPop;
			for (unsigned int i = 0; i < toPop; ++i)
			{
				move_element(elements[i], *dataListHead);
				move_head_next();
			}
			lock.unlock();

			if (UNLIKELY(toPop == 1))
				notFullCond.notify_one();
			else
				notFullCond.notify_all();
			return toPop;
		}

		inline void move_element(TYPE &dest, TYPE &src)
		{
			new (&dest) TYPE(std::move(src));
			conditional_destroy<TYPE>(src);
		}

	public:
		using task_type = TYPE;

		TPBlockQueue() : memoryBlock(nullptr), isStopped(0) {}

		~TPBlockQueue() { release(); }

		/**
		 * @brief Initializes queue with fixed capacity
		 */
		bool init(unsigned int capacity)
		{
			if (memoryBlock || !capacity)
				return false;

			unsigned int align = std::max(alignof(TYPE), (size_t)64);
			totalsize = sizeof(TYPE) * capacity;
			totalsize = (totalsize + align - 1) & ~(align - 1);
			memoryBlock = ALIGNED_MALLOC(totalsize, align);

			if (!memoryBlock)
				return false;

			size = 0;
			maxSize = capacity;
			dataListHead = (TYPE *)memoryBlock;
			dataListTail = (TYPE *)memoryBlock;
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

			if (UNLIKELY(size == maxSize))
				return false;

			emplace_helper<POS>(lock, std::forward<Args>(args)...);
			return true;
		}

		/**
		 * @brief Blocking element emplacement with indefinite wait
		 */
		template <INSERT_POS POS = TAIL, typename... Args>
		typename std::enable_if<!is_duration<typename std::tuple_element<0, std::tuple<Args...>>::type>::value, bool>::type
		wait_emplace(Args &&...args)
		{
			std::unique_lock<std::mutex> lock(dataMutex);

			notFullCond.wait(lock, [this]
							 { return LIKELY(size != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return false;

			emplace_helper<POS>(lock, std::forward<Args>(args)...);
			return true;
		}

		/**
		 * @brief Blocking element emplacement with timeout
		 */
		template <INSERT_POS POS = TAIL, class Rep, class Period, typename... Args>
		bool wait_emplace(const std::chrono::duration<Rep, Period> &timeout, Args &&...args)
		{
			std::unique_lock<std::mutex> lock(dataMutex);

			bool success = notFullCond.wait_for(lock, timeout, [this]
												{ return LIKELY(size != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || isStopped))
				return false;

			emplace_helper<POS>(lock, std::forward<Args>(args)...);
			return true;
		}

		/**
		 * @brief Non-blocking element push
		 */
		template <INSERT_POS POS = TAIL, class T>
		bool push(T &&element)
		{
			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(size == maxSize))
				return false;

			push_helper<POS>(lock, std::forward<T>(element));
			return true;
		}

		/**
		 * @brief Blocking element push with indefinite wait
		 */
		template <INSERT_POS POS = TAIL, class T>
		bool wait_push(T &&element)
		{
			std::unique_lock<std::mutex> lock(dataMutex);

			notFullCond.wait(lock, [this]
							 { return LIKELY(size != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return false;

			push_helper<POS>(lock, std::forward<T>(element));
			return true;
		}

		/**
		 * @brief Blocking element push with timeout
		 */
		template <INSERT_POS POS = TAIL, class T, class Rep, class Period>
		bool wait_push(T &&element, const std::chrono::duration<Rep, Period> &timeout)
		{
			std::unique_lock<std::mutex> lock(dataMutex);

			bool success = notFullCond.wait_for(lock, timeout, [this]
												{ return LIKELY(size != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || isStopped))
				return false;

			push_helper<POS>(lock, std::forward<T>(element));
			return true;
		}

		/**
		 * @brief Bulk push for multiple elements
		 */
		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL>
		unsigned int pushBulk(TYPE *elements, unsigned int count)
		{
			if (UNLIKELY(!count))
				return 0;

			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(!(maxSize - size)))
				return 0;

			return pushBulk_helper<METHOD, POS>(lock, elements, count);
		}

		/**
		 * @brief Blocking bulk push with indefinite wait
		 */
		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL>
		unsigned int wait_pushBulk(TYPE *elements, unsigned int count)
		{
			if (UNLIKELY(!count))
				return 0;

			std::unique_lock<std::mutex> lock(dataMutex);

			notFullCond.wait(lock, [this]
							 { return LIKELY(size != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return 0;

			return pushBulk_helper<METHOD, POS>(lock, elements, count);
		}

		/**
		 * @brief Blocking bulk push with timeout
		 */
		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL, class Rep, class Period>
		unsigned int wait_pushBulk(TYPE *elements, unsigned int count, const std::chrono::duration<Rep, Period> &timeout)
		{
			if (UNLIKELY(!count))
				return 0;

			std::unique_lock<std::mutex> lock(dataMutex);

			bool success = notFullCond.wait_for(lock, timeout, [this]
												{ return LIKELY(size != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || isStopped))
				return 0;

			return pushBulk_helper<METHOD, POS>(lock, elements, count);
		}

		/**
		 * @brief Non-blocking element removal
		 */
		bool pop(TYPE &element)
		{
			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(!size))
				return false;

			pop_helper(lock, element);
			return true;
		}

		/**
		 * @brief Blocking element removal with indefinite wait
		 */
		bool wait_pop(TYPE &element)
		{
			std::unique_lock<std::mutex> lock(dataMutex);
			notEmptyCond.wait(lock, [this]
							  { return LIKELY(size) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return false;

			pop_helper(lock, element);
			return true;
		}

		/**
		 * @brief Blocking element removal with timeout
		 */
		template <class Rep, class Period>
		bool wait_pop(TYPE &element, const std::chrono::duration<Rep, Period> &timeout)
		{
			std::unique_lock<std::mutex> lock(dataMutex);

			bool success = notEmptyCond.wait_for(lock, timeout, [this]
												 { return LIKELY(size) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || isStopped))
				return false;

			pop_helper(lock, element);
			return true;
		}

		/**
		 * @brief Bulk element retrieval
		 */
		unsigned int popBulk(TYPE *elements, unsigned int count)
		{
			if (UNLIKELY(!count))
				return 0;

			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(!size))
				return 0;

			return popbulk_helper(lock, elements, count);
		}

		/**
		 * @brief Blocking bulk retrieval with indefinite wait
		 */
		unsigned int wait_popBulk(TYPE *elements, unsigned int count)
		{
			if (UNLIKELY(!count))
				return 0;

			std::unique_lock<std::mutex> lock(dataMutex);

			notEmptyCond.wait(lock, [this]
							  { return LIKELY(size) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return 0;

			return popbulk_helper(lock, elements, count);
		}

		/**
		 * @brief Blocking bulk retrieval with timeout
		 */
		template <class Rep, class Period>
		unsigned int wait_popBulk(TYPE *elements, unsigned int count, const std::chrono::duration<Rep, Period> &timeout)
		{
			if (UNLIKELY(!count))
				return 0;

			std::unique_lock<std::mutex> lock(dataMutex);
			bool success = notEmptyCond.wait_for(lock, timeout, [this]
												 { return LIKELY(size) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || isStopped))
				return 0;

			return popbulk_helper(lock, elements, count);
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
				TYPE *current = dataListHead;
				for (unsigned int i = 0; i < size; ++i)
				{
					conditional_destroy<TYPE>(*current);
					current = (TYPE *)((char *)current + sizeof(TYPE));
					if ((uintptr_t)(current) == border)
						current = (TYPE *)(memoryBlock);
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
		TPBlockQueue(const TPBlockQueue &) = delete;
		TPBlockQueue &operator=(const TPBlockQueue &) = delete;
	};
}
#endif // HSLL_TPBLOCKQUEUE