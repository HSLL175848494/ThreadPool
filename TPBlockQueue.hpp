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
	 * @brief Enumeration defining the method of element extraction during pop operations
	 */
	enum POP_METHOD
	{
		ASSIGN, ///< Use assignment operator for element transfer
		PLACE	///< Use placement new for element transfer
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
	 * @brief Helper template for conditional element extraction
	 */
	template <typename T, POP_METHOD Method>
	struct PopHelper;

	template <typename T>
	struct PopHelper<T, ASSIGN>
	{
		static void extract(T &dest, T &src)
		{
			dest = std::move(src);
		}
	};

	template <typename T>
	struct PopHelper<T, PLACE>
	{
		static void extract(T &dest, T &src)
		{
			new (&dest) T(std::move(src));
		}
	};

	template <POP_METHOD Method, typename T>
	void pop_extract(T &dest, T &src)
	{
		PopHelper<T, Method>::extract(dest, src);
	}

	/**
	 * @brief Circular buffer based blocking queue implementation
	 */
	template <class TYPE>
	class TPBlockQueue
	{
		template <class T>
		friend class ThreadPool;

	private:
		// Memory management
		void *memoryBlock;		///< Raw memory block for element storage
		uintptr_t border;		///< End address of allocated memory
		unsigned int isStopped; ///< Flag for stopping all operations

		// Queue state tracking
		unsigned int size;		///< Current number of elements in queue
		unsigned int maxSize;	///< Capacity of the queue
		unsigned int totalsize; ///< Total allocated memory size

		// Buffer pointers
		TYPE *dataListHead; ///< Pointer to first element in queue
		TYPE *dataListTail; ///< Pointer to next insertion position

		// Synchronization primitives
		std::mutex dataMutex;				  ///< Mutex protecting all queue operations
		std::condition_variable notEmptyCond; ///< Signaled when data becomes available
		std::condition_variable notFullCond;  ///< Signaled when space becomes available

		// Helper functions for pointer movement
		void MoveTailNext()
		{
			dataListTail = (TYPE *)((uint8_t *)dataListTail + sizeof(TYPE));
			if (UNLIKELY((uintptr_t)dataListTail == border))
				dataListTail = (TYPE *)(uintptr_t)memoryBlock;
		}

		void MoveHeadNext()
		{
			dataListHead = (TYPE *)((uint8_t *)dataListHead + sizeof(TYPE));
			if (UNLIKELY((uintptr_t)dataListHead == border))
				dataListHead = (TYPE *)(uintptr_t)memoryBlock;
		}

		// Reserve for head push
		void MoveHeadPrev()
		{
			dataListHead = (TYPE *)((uint8_t *)dataListHead - sizeof(TYPE));
			if (UNLIKELY((uintptr_t)dataListHead < (uintptr_t)memoryBlock))
				dataListHead = (TYPE *)(border - sizeof(TYPE));
		}

		// Insert position implementations
		template <typename... Args>
		void emplace_impl(InsertAtTailTag, Args &&...args)
		{
			new (dataListTail) TYPE(std::forward<Args>(args)...);
			size++;
			MoveTailNext();
		}

		template <typename... Args>
		void emplace_impl(InsertAtHeadTag, Args &&...args)
		{
			MoveHeadPrev();
			new (dataListHead) TYPE(std::forward<Args>(args)...);
			size++;
		}

		template <class T>
		void push_impl(InsertAtTailTag, T &&element)
		{
			new (dataListTail) TYPE(std::forward<T>(element));
			size++;
			MoveTailNext();
		}

		template <class T>
		void push_impl(InsertAtHeadTag, T &&element)
		{
			MoveHeadPrev();
			new (dataListHead) TYPE(std::forward<T>(element));
			size++;
		}

		template <BULK_CMETHOD METHOD>
		void bulk_push_impl(InsertAtTailTag, TYPE *elements, unsigned int toPush)
		{
			for (unsigned int i = 0; i < toPush; ++i)
			{
				bulk_construct<METHOD>(*dataListTail, elements[i]);
				MoveTailNext();
			}
		}

		template <BULK_CMETHOD METHOD>
		void bulk_push_impl(InsertAtHeadTag, TYPE *elements, unsigned int toPush)
		{
			for (unsigned int i = 0; i < toPush; ++i)
			{
				MoveHeadPrev();
				bulk_construct<METHOD>(*dataListHead, elements[toPush - i - 1]);
			}
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
			if (memoryBlock || capacity == 0)
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

			typedef typename std::conditional<POS == HEAD, InsertAtHeadTag, InsertAtTailTag>::type InsertTag;
			emplace_impl(InsertTag(), std::forward<Args>(args)...);

			lock.unlock();
			notEmptyCond.notify_one();
			return true;
		}

		/**
		 * @brief Blocking element emplacement with indefinite wait
		 */
		template <INSERT_POS POS = TAIL, typename... Args>
		bool wait_emplace(Args &&...args)
		{
			std::unique_lock<std::mutex> lock(dataMutex);
			notFullCond.wait(lock, [this]
							 { return LIKELY(size != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(size == maxSize))
				return false;

			typedef typename std::conditional<POS == HEAD, InsertAtHeadTag, InsertAtTailTag>::type InsertTag;
			emplace_impl(InsertTag(), std::forward<Args>(args)...);

			lock.unlock();
			notEmptyCond.notify_one();
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

			if (UNLIKELY(!success || size == maxSize))
				return false;

			typedef typename std::conditional<POS == HEAD, InsertAtHeadTag, InsertAtTailTag>::type InsertTag;
			emplace_impl(InsertTag(), std::forward<Args>(args)...);

			lock.unlock();
			notEmptyCond.notify_one();
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

			typedef typename std::conditional<POS == HEAD, InsertAtHeadTag, InsertAtTailTag>::type InsertTag;
			push_impl(InsertTag(), std::forward<T>(element));

			lock.unlock();
			notEmptyCond.notify_one();
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

			if (UNLIKELY(size == maxSize))
				return false;

			typedef typename std::conditional<POS == HEAD, InsertAtHeadTag, InsertAtTailTag>::type InsertTag;
			push_impl(InsertTag(), std::forward<T>(element));

			lock.unlock();
			notEmptyCond.notify_one();
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

			if (UNLIKELY(!success || size == maxSize))
				return false;

			typedef typename std::conditional<POS == HEAD, InsertAtHeadTag, InsertAtTailTag>::type InsertTag;
			push_impl(InsertTag(), std::forward<T>(element));

			lock.unlock();
			notEmptyCond.notify_one();
			return true;
		}

		/**
		 * @brief Bulk push for multiple elements
		 */
		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL>
		unsigned int pushBulk(TYPE *elements, unsigned int count)
		{
			if (UNLIKELY(count == 0))
				return 0;

			std::unique_lock<std::mutex> lock(dataMutex);
			unsigned int available = maxSize - size;

			if (UNLIKELY(available == 0))
				return 0;

			unsigned int toPush = std::min(count, available);

			typedef typename std::conditional<POS == HEAD, InsertAtHeadTag, InsertAtTailTag>::type InsertTag;
			bulk_push_impl<METHOD>(InsertTag(), elements, toPush);

			size += toPush;
			lock.unlock();

			if (UNLIKELY(toPush == 1))
				notEmptyCond.notify_one();
			else
				notEmptyCond.notify_all();

			return toPush;
		}

		/**
		 * @brief Blocking bulk push with indefinite wait
		 */
		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL>
		unsigned int wait_pushBulk(TYPE *elements, unsigned int count)
		{
			if (UNLIKELY(count == 0))
				return 0;

			std::unique_lock<std::mutex> lock(dataMutex);
			notFullCond.wait(lock, [this]
							 { return LIKELY(size != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(size == maxSize))
				return 0;

			unsigned int toPush = std::min(count, maxSize - size);

			typedef typename std::conditional<POS == HEAD, InsertAtHeadTag, InsertAtTailTag>::type InsertTag;
			bulk_push_impl<METHOD>(InsertTag(), elements, toPush);

			size += toPush;
			lock.unlock();

			if (UNLIKELY(toPush == 1))
				notEmptyCond.notify_one();
			else
				notEmptyCond.notify_all();

			return toPush;
		}

		/**
		 * @brief Blocking bulk push with timeout
		 */
		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL, class Rep, class Period>
		unsigned int wait_pushBulk(TYPE *elements, unsigned int count, const std::chrono::duration<Rep, Period> &timeout)
		{
			if (UNLIKELY(count == 0))
				return 0;

			std::unique_lock<std::mutex> lock(dataMutex);
			bool success = notFullCond.wait_for(lock, timeout, [this]
												{ return LIKELY(size != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || size == maxSize))
				return 0;

			unsigned int toPush = std::min(count, maxSize - size);

			typedef typename std::conditional<POS == HEAD, InsertAtHeadTag, InsertAtTailTag>::type InsertTag;
			bulk_push_impl<METHOD>(InsertTag(), elements, toPush);

			size += toPush;
			lock.unlock();

			if (UNLIKELY(toPush == 1))
				notEmptyCond.notify_one();
			else
				notEmptyCond.notify_all();

			return toPush;
		}

		/**
		 * @brief Non-blocking element removal
		 */
		template <POP_METHOD M = ASSIGN>
		bool pop(TYPE &element)
		{
			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(!size))
				return false;

			size--;
			pop_extract<M>(element, *dataListHead);
			conditional_destroy(*dataListHead);
			MoveHeadNext();
			lock.unlock();
			notFullCond.notify_one();
			return true;
		}

		/**
		 * @brief Blocking element removal with indefinite wait
		 */
		template <POP_METHOD M = ASSIGN>
		bool wait_pop(TYPE &element)
		{
			std::unique_lock<std::mutex> lock(dataMutex);
			notEmptyCond.wait(lock, [this]
							  { return LIKELY(size) || UNLIKELY(isStopped); });

			if (UNLIKELY(!size))
				return false;

			size--;
			pop_extract<M>(element, *dataListHead);
			conditional_destroy(*dataListHead);
			MoveHeadNext();
			lock.unlock();
			notFullCond.notify_one();
			return true;
		}

		/**
		 * @brief Blocking element removal with timeout
		 */
		template <POP_METHOD M = ASSIGN, class Rep, class Period>
		bool wait_pop(TYPE &element, const std::chrono::duration<Rep, Period> &timeout)
		{
			std::unique_lock<std::mutex> lock(dataMutex);
			bool success = notEmptyCond.wait_for(lock, timeout, [this]
												 { return LIKELY(size) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || !size))
				return false;

			size--;
			pop_extract<M>(element, *dataListHead);
			conditional_destroy(*dataListHead);
			MoveHeadNext();
			lock.unlock();
			notFullCond.notify_one();
			return true;
		}

		/**
		 * @brief Bulk element retrieval
		 */
		template <POP_METHOD M = ASSIGN>
		unsigned int popBulk(TYPE *elements, unsigned int count)
		{
			if (UNLIKELY(count == 0))
				return 0;

			std::unique_lock<std::mutex> lock(dataMutex);
			unsigned int available = size;

			if (UNLIKELY(available == 0))
				return 0;

			unsigned int toPop = std::min(count, available);
			for (unsigned int i = 0; i < toPop; ++i)
			{
				pop_extract<M>(elements[i], *dataListHead);
				conditional_destroy(*dataListHead);
				MoveHeadNext();
			}

			size -= toPop;
			lock.unlock();

			if (UNLIKELY(toPop == 1))
				notFullCond.notify_one();
			else
				notFullCond.notify_all();

			return toPop;
		}

		/**
		 * @brief Blocking bulk retrieval with indefinite wait
		 */
		template <POP_METHOD M = ASSIGN>
		unsigned int wait_popBulk(TYPE *elements, unsigned int count)
		{
			if (UNLIKELY(count == 0))
				return 0;

			std::unique_lock<std::mutex> lock(dataMutex);
			notEmptyCond.wait(lock, [this]
							  { return LIKELY(size) || UNLIKELY(isStopped); });

			if (UNLIKELY(!size))
				return 0;

			unsigned int toPop = std::min(count, size);
			for (unsigned int i = 0; i < toPop; ++i)
			{
				pop_extract<M>(elements[i], *dataListHead);
				conditional_destroy(*dataListHead);
				MoveHeadNext();
			}

			size -= toPop;
			lock.unlock();

			if (UNLIKELY(toPop == 1))
				notFullCond.notify_one();
			else
				notFullCond.notify_all();

			return toPop;
		}

		/**
		 * @brief Blocking bulk retrieval with timeout
		 */
		template <POP_METHOD M = ASSIGN, class Rep, class Period>
		unsigned int wait_popBulk(TYPE *elements, unsigned int count, const std::chrono::duration<Rep, Period> &timeout)
		{
			if (UNLIKELY(count == 0))
				return 0;

			std::unique_lock<std::mutex> lock(dataMutex);
			bool success = notEmptyCond.wait_for(lock, timeout, [this]
												 { return LIKELY(size) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || !size))
				return 0;

			unsigned int toPop = std::min(count, size);
			for (unsigned int i = 0; i < toPop; ++i)
			{
				pop_extract<M>(elements[i], *dataListHead);
				conditional_destroy(*dataListHead);
				MoveHeadNext();
			}

			size -= toPop;
			lock.unlock();

			if (UNLIKELY(toPop == 1))
				notFullCond.notify_one();
			else
				notFullCond.notify_all();

			return toPop;
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
					conditional_destroy(*current);
					current = (TYPE *)((uint8_t *)current + sizeof(TYPE));
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