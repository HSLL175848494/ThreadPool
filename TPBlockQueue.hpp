#ifndef HSLL_TPBLOCKQUEUE
#define HSLL_TPBLOCKQUEUE

#include <new>
#include <mutex>
#include <chrono>
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
#define ALIGNED_MALLOC(size, align) aligned_alloc(align, size)
#define ALIGNED_FREE(ptr) free(ptr)
#endif

	/**
	 * @brief Enumeration defining the method of bulk construction
	 * This enum is used to specify whether bulk construction operations should use
	 * copy semantics or move semantics when constructing objects.
	 */
	enum BULK_CMETHOD
	{
		COPY, ///< Use copy construction semantics
		MOVE  ///< Use move construction semantics
	};

	/**
	 * @brief Enumeration defining the method of element extraction during pop operations
	 * This enum specifies how elements should be transferred from the queue to the caller
	 * during pop operations.
	 */
	enum POP_METHOD
	{
		ASSIGN, ///< Use assignment operator for element transfer (efficient for movable types)
		PLACE	///< Use placement new for element transfer (enables transfer of non-movable types)
	};

	/**
	 * @brief Helper template for conditional object destruction
	 * @tparam T Type of object to manage
	 * @tparam IsTrivial Boolean indicating if type is trivially destructible
	 * @details Provides destruction mechanism that respects type traits:
	 *          - For non-trivial types: calls object destructor explicitly
	 *          - For trivial types: no-operation
	 */
	template <typename T, bool IsTrivial = std::is_trivially_destructible<T>::value>
	struct DestroyHelper
	{
		static void destroy(T& obj) { obj.~T(); }
	};

	/**
	 * @brief Specialization for trivially destructible types
	 */
	template <typename T>
	struct DestroyHelper<T, true>
	{
		static void destroy(T&) {}
	};

	/**
	 * @brief Conditionally destroys an object based on its type traits
	 * @tparam T Type of the object to potentially destroy
	 * @param obj Reference to the object being managed
	 * @details Uses DestroyHelper to decide whether to call destructor:
	 *          - Calls destructor for non-trivial types
	 *          - No action for trivial types
	 */
	template <typename T>
	void conditional_destroy(T& obj)
	{
		DestroyHelper<T>::destroy(obj);
	}

	/**
	 * @brief Helper template for bulk construction (copy/move)
	 * @tparam T Type of object to construct
	 * @tparam Method BULK_CONSTRUCT_METHOD selection
	 */
	template <typename T, BULK_CMETHOD Method>
	struct BulkConstructHelper;

	/**
	 * @brief Specialization for copy construction
	 */
	template <typename T>
	struct BulkConstructHelper<T, COPY>
	{
		static void construct(T& ptr, T& source)
		{
			new (&ptr) T(source);
		}
	};

	/**
	 * @brief Specialization for move construction
	 */
	template <typename T>
	struct BulkConstructHelper<T, MOVE>
	{
		static void construct(T& ptr, T& source)
		{
			new (&ptr) T(std::move(source));
		}
	};

	/**
	 * @brief Conditionally constructs an object using copy/move semantics
	 * @tparam Method BULK_CONSTRUCT_METHOD selection
	 * @tparam T Type of the object to construct
	 * @param ptr Pointer to memory location where object should be constructed
	 * @param source Source object reference for construction
	 */
	template <BULK_CMETHOD Method, typename T>
	void bulk_construct(T& ptr, T& source)
	{
		BulkConstructHelper<T, Method>::construct(ptr, source);
	}

	/**
	 * @brief Helper template for conditional element extraction
	 * @tparam T Type of object to extract
	 * @tparam Method POP_METHOD selection
	 * @details Provides extraction mechanism based on POP_METHOD:
	 *          - For ASSIGN method: uses move assignment
	 *          - For PLACE method: uses copy construction via placement new
	 */
	template <typename T, POP_METHOD Method>
	struct PopHelper;

	/**
	 * @brief Specialization for assignment-based extraction
	 * @tparam T Type of object to extract
	 */
	template <typename T>
	struct PopHelper<T, ASSIGN>
	{
		static void extract(T& dest, T& src)
		{
			dest = std::move(src);
		}
	};

	/**
	 * @brief Specialization for placement-based extraction
	 * @tparam T Type of object to extract
	 */
	template <typename T>
	struct PopHelper<T, PLACE>
	{
		static void extract(T& dest, T& src)
		{
			new (&dest) T(std::move(src));
		}
	};

	/**
	 * @brief Conditionally extracts an element based on POP_METHOD
	 * @tparam Method POP_METHOD selection
	 * @tparam T Type of the object to extract
	 * @param dest Reference to destination storage
	 * @param src Reference to source element in queue
	 * @details Uses PopHelper to determine extraction method:
	 *          - ASSIGN: Efficient move assignment for movable types
	 *          - PLACE: Safe copy-construction for non-movable types
	 */
	template <POP_METHOD Method, typename T>
	void pop_extract(T& dest, T& src)
	{
		PopHelper<T, Method>::extract(dest, src);
	}

	/**
	 * @brief Circular buffer based blocking queue implementation
	 * @tparam TYPE Element type stored in the queue
	 * @details Features:
	 *          - Fixed-size circular buffer using preallocated nodes
	 *          - Single mutex design for all operations
	 *          - Size tracking for quick capacity checks
	 *          - Optimized for types with trivial destructors
	 *          - Efficient memory layout for cache locality
	 */
	template <class TYPE>
	class TPBlockQueue
	{
	private:
		struct Node
		{
			TYPE data;	///< Storage for element data
			Node* next; ///< Pointer to next node in circular list
			Node() = default;
		};

		// Memory management
		void* memoryBlock;		///< Raw memory block for node storage
		unsigned int isStopped; ///< Flag for stopping all operations

		// Queue state tracking
		unsigned int size;	  ///< Current number of elements in queue
		unsigned int maxSize; ///< Maximum capacity of the queue

		// List pointers
		Node* dataListHead; ///< Pointer to first element in queue
		Node* dataListTail; ///< Pointer to next insertion position

		// Synchronization primitives
		std::mutex dataMutex;				  ///< Mutex protecting all queue operations
		std::condition_variable notEmptyCond; ///< Signaled when data becomes available
		std::condition_variable notFullCond;  ///< Signaled when space becomes available

		template <class T>
		friend class ThreadPool;

	public:
		TPBlockQueue() : memoryBlock(nullptr), isStopped(0) {}

		~TPBlockQueue() { release(); }

		/**
		 * @brief Initializes queue with fixed capacity
		 * @param capacity Maximum number of elements the queue can hold
		 * @return true if initialization succeeded, false otherwise
		 * @details Creates circular linked list from preallocated nodes
		 */
		bool init(unsigned int capacity)
		{
			if (memoryBlock || capacity == 0)
				return false;

			unsigned int totalSize = sizeof(Node) * capacity;
			totalSize = (totalSize + 64 - 1) & ~(64 - 1);
			memoryBlock = ALIGNED_MALLOC(totalSize, 64);

			if (!memoryBlock)
				return false;

			Node* nodes = (Node*)(memoryBlock);

			for (unsigned int i = 0; i < capacity - 1; ++i)
				nodes[i].next = &nodes[i + 1];
			nodes[capacity - 1].next = &nodes[0];

			size = 0;
			maxSize = capacity;
			dataListHead = nodes;
			dataListTail = nodes;
			return true;
		}

		/**
		 * @brief Non-blocking element emplacement with perfect forwarding
		 * @tparam Args Types of arguments to forward to element constructor
		 * @param args Arguments to forward to element constructor
		 * @return true if element was emplaced, false if queue was full
		 * @details Constructs element in-place at the tail of the queue using
		 *          perfect forwarding. Notifies consumers if successful.
		 */
		template <typename... Args>
		bool emplace(Args &&...args)
		{
			std::unique_lock<std::mutex> lock(dataMutex);
			if (UNLIKELY(size == maxSize))
				return false;

			new (&dataListTail->data) TYPE(std::forward<Args>(args)...);
			size++;
			dataListTail = dataListTail->next;
			lock.unlock();
			notEmptyCond.notify_one();
			return true;
		}

		/**
		 * @brief Blocking element emplacement with indefinite wait
		 * @tparam Args Types of arguments to forward to element constructor
		 * @param args Arguments to forward to element constructor
		 * @return true if element was emplaced, false if queue was stopped
		 * @details Waits until queue has space or is stopped. Constructs element
		 *          in-place and notifies consumers upon success.
		 */
		template <typename... Args>
		bool wait_emplace(Args &&...args)
		{
			std::unique_lock<std::mutex> lock(dataMutex);
			notFullCond.wait(lock, [this]
				{ return LIKELY(size != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(size == maxSize))
				return false;

			new (&dataListTail->data) TYPE(std::forward<Args>(args)...);
			size++;
			dataListTail = dataListTail->next;
			lock.unlock();
			notEmptyCond.notify_one();
			return true;
		}

		/**
		 * @brief Blocking element emplacement with timeout
		 * @tparam Rep Chrono duration representation type
		 * @tparam Period Chrono duration period type
		 * @tparam Args Types of arguments to forward to element constructor
		 * @param timeout Maximum duration to wait for space
		 * @param args Arguments to forward to element constructor
		 * @return true if element was emplaced, false on timeout or stop
		 * @details Waits up to specified duration for space. Constructs element
		 *          in-place if space becomes available and notifies consumers.
		 */
		template <class Rep, class Period, typename... Args>
		bool wait_emplace(const std::chrono::duration<Rep, Period>& timeout, Args &&...args)
		{
			std::unique_lock<std::mutex> lock(dataMutex);
			bool success = notFullCond.wait_for(lock, timeout, [this]
				{ return LIKELY(size != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || size == maxSize))
				return false;

			new (&dataListTail->data) TYPE(std::forward<Args>(args)...);
			size++;
			dataListTail = dataListTail->next;
			lock.unlock();
			notEmptyCond.notify_one();
			return true;
		}

		/**
		 * @brief Non-blocking element push
		 * @tparam T Deduced element type (supports perfect forwarding)
		 * @param element Element to push into queue
		 * @return true if element was added, false if queue was full
		 */
		template <class T>
		bool push(T&& element)
		{
			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(size == maxSize))
				return false;

			new (&dataListTail->data) TYPE(std::forward<T>(element));
			size++;
			dataListTail = dataListTail->next;
			lock.unlock();
			notEmptyCond.notify_one();
			return true;
		}

		/**
		 * @brief Blocking element push with indefinite wait
		 * @tparam T Deduced element type (supports perfect forwarding)
		 * @param element Element to push into queue
		 * @return true if element was added, false if queue was stopped
		 */
		template <class T>
		bool wait_push(T&& element)
		{
			std::unique_lock<std::mutex> lock(dataMutex);
			notFullCond.wait(lock, [this]
				{ return LIKELY(size != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(size == maxSize))
				return false;

			new (&dataListTail->data) TYPE(std::forward<T>(element));
			size++;
			dataListTail = dataListTail->next;
			lock.unlock();
			notEmptyCond.notify_one();
			return true;
		}

		/**
		 * @brief Blocking element push with timeout
		 * @tparam T Deduced element type
		 * @tparam Rep Chrono duration representation type
		 * @tparam Period Chrono duration period type
		 * @param element Element to push into queue
		 * @param timeout Maximum time to wait for space
		 * @return true if element was added, false on timeout or stop
		 */
		template <class T, class Rep, class Period>
		bool wait_push(T&& element, const std::chrono::duration<Rep, Period>& timeout)
		{
			std::unique_lock<std::mutex> lock(dataMutex);
			bool success = notFullCond.wait_for(lock, timeout, [this]
				{ return LIKELY(size != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || size == maxSize))
				return false;

			new (&dataListTail->data) TYPE(std::forward<T>(element));
			size++;
			dataListTail = dataListTail->next;
			lock.unlock();
			notEmptyCond.notify_one();
			return true;
		}

		/**
		 * @brief Bulk push for multiple elements using specified construction method
		 * @param elements Array of elements to construct from
		 * @param count Number of elements to push
		 * @return Actual number of elements pushed
		 * @details Constructs elements using either copy or move semantics based on
		 *          BULK_CONSTRUCT_METHOD template parameter. Notifies consumers based
		 *          on inserted quantity.
		 */
		template <BULK_CMETHOD METHOD = COPY>
		unsigned int pushBulk(TYPE* elements, unsigned int count)
		{
			if (UNLIKELY(count == 0))
				return 0;

			std::unique_lock<std::mutex> lock(dataMutex);
			unsigned int available = maxSize - size;

			if (UNLIKELY(available == 0))
				return 0;

			unsigned int toPush = std::min(count, available);
			for (unsigned int i = 0; i < toPush; ++i)
			{
				bulk_construct<METHOD>(dataListTail->data, elements[i]);
				dataListTail = dataListTail->next;
			}

			size += toPush;
			lock.unlock();

			if (LIKELY(toPush == 1))
				notEmptyCond.notify_one();
			else
				notEmptyCond.notify_all();

			return toPush;
		}

		/**
		 * @brief Blocking bulk push with specified construction semantics
		 * @param elements Array of elements to construct from
		 * @param count Number of elements to push
		 * @return Actual number of elements pushed before stop
		 * @details Waits indefinitely for space. Constructs elements using either
		 *          copy or move semantics based on BULK_CONSTRUCT_METHOD template.
		 */
		template <BULK_CMETHOD METHOD = COPY>
		unsigned int wait_pushBulk(TYPE* elements, unsigned int count)
		{
			if (UNLIKELY(count == 0))
				return 0;

			std::unique_lock<std::mutex> lock(dataMutex);
			notFullCond.wait(lock, [this]
				{ return LIKELY(size != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(size == maxSize))
				return 0;

			unsigned int available = maxSize - size;
			unsigned int toPush = std::min(count, available);
			for (unsigned int i = 0; i < toPush; ++i)
			{
				bulk_construct<METHOD>(dataListTail->data, elements[i]);
				dataListTail = dataListTail->next;
			}

			size += toPush;
			lock.unlock();

			if (LIKELY(toPush == 1))
				notEmptyCond.notify_one();
			else
				notEmptyCond.notify_all();

			return toPush;
		}

		/**
		 * @brief Timed bulk push with specified construction method
		 * @tparam METHOD BULK_CONSTRUCT_METHOD selection (copy/move)
		 * @param elements Array of elements to construct from
		 * @param count Number of elements to push
		 * @param timeout Maximum time to wait for space
		 * @return Actual number of elements pushed
		 * @details Waits up to timeout duration. Constructs elements using
		 *          selected METHOD (copy/move). Processes maximum available
		 *          capacity when awakened.
		 */
		template <BULK_CMETHOD METHOD = COPY, class Rep, class Period>
		unsigned int wait_pushBulk(TYPE* elements, unsigned int count, const std::chrono::duration<Rep, Period>& timeout)
		{
			if (UNLIKELY(count == 0))
				return 0;

			std::unique_lock<std::mutex> lock(dataMutex);
			bool success = notFullCond.wait_for(lock, timeout, [this]
				{ return LIKELY(size != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || size == maxSize))
				return 0;

			unsigned int available = maxSize - size;
			unsigned int toPush = std::min(count, available);
			for (unsigned int i = 0; i < toPush; ++i)
			{
				bulk_construct<METHOD>(dataListTail->data, elements[i]);
				dataListTail = dataListTail->next;
			}

			size += toPush;
			lock.unlock();

			if (LIKELY(toPush == 1))
				notEmptyCond.notify_one();
			else
				notEmptyCond.notify_all();

			return toPush;
		}

		/**
		 * @brief Non-blocking element removal
		 * @tparam M METHOD POP_METHOD selection (assign/place)
		 * @param element Reference to store popped element
		 * @return true if element was retrieved, false if queue was empty
		 */
		template <POP_METHOD M = ASSIGN>
		bool pop(TYPE& element)
		{
			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(!size))
				return false;

			size--;
			pop_extract<M>(element, dataListHead->data);
			conditional_destroy(dataListHead->data);
			dataListHead = dataListHead->next;
			lock.unlock();
			notFullCond.notify_one();
			return true;
		}

		/**
		 * @brief Blocking element removal with indefinite wait
		 * @tparam M METHOD POP_METHOD selection (assign/place)
		 * @param element Reference to store popped element
		 * @return true if element was retrieved, false if queue was stopped
		 */
		template <POP_METHOD M = ASSIGN>
		bool wait_pop(TYPE& element)
		{
			std::unique_lock<std::mutex> lock(dataMutex);
			notEmptyCond.wait(lock, [this]
				{ return LIKELY(size) || UNLIKELY(isStopped); });

			if (UNLIKELY(!size))
				return false;

			size--;
			pop_extract<M>(element, dataListHead->data);
			conditional_destroy(dataListHead->data);
			dataListHead = dataListHead->next;
			lock.unlock();
			notFullCond.notify_one();
			return true;
		}

		/**
		 * @brief Blocking element removal with timeout
		 * @tparam M METHOD POP_METHOD selection (assign/place)
		 * @tparam Rep Chrono duration representation type
		 * @tparam Period Chrono duration period type
		 * @param element Reference to store popped element
		 * @param timeout Maximum time to wait for data
		 * @return true if element was retrieved, false on timeout or stop
		 */
		template <POP_METHOD M = ASSIGN, class Rep, class Period>
		bool wait_pop(TYPE& element, const std::chrono::duration<Rep, Period>& timeout)
		{
			std::unique_lock<std::mutex> lock(dataMutex);
			bool success = notEmptyCond.wait_for(lock, timeout, [this]
				{ return LIKELY(size) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || !size))
				return false;

			size--;
			pop_extract<M>(element, dataListHead->data);
			conditional_destroy(dataListHead->data);
			dataListHead = dataListHead->next;
			lock.unlock();
			notFullCond.notify_one();
			return true;
		}

		/**
		 * @brief Bulk element retrieval
		 * @tparam M METHOD POP_METHOD selection (assign/place)
		 * @param elements Array to store retrieved elements
		 * @param count Maximum number of elements to retrieve
		 * @return Actual number of elements retrieved
		 */
		template <POP_METHOD M = ASSIGN>
		unsigned int popBulk(TYPE* elements, unsigned int count)
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
				pop_extract<M>(elements[i], dataListHead->data);
				conditional_destroy(dataListHead->data);
				dataListHead = dataListHead->next;
			}

			size -= toPop;
			lock.unlock();

			if (LIKELY(toPop == 1))
				notFullCond.notify_one();
			else
				notFullCond.notify_all();

			return toPop;
		}

		/**
		 * @brief Blocking bulk retrieval with indefinite wait
		 * @tparam M METHOD POP_METHOD selection (assign/place)
		 * @param elements Array to store retrieved elements
		 * @param count Maximum number of elements to retrieve
		 * @return Actual number of elements retrieved before stop
		 */
		template <POP_METHOD M = ASSIGN>
		unsigned int wait_popBulk(TYPE* elements, unsigned int count)
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
				pop_extract<M>(elements[i], dataListHead->data);
				conditional_destroy(dataListHead->data);
				dataListHead = dataListHead->next;
			}

			size -= toPop;
			lock.unlock();

			if (LIKELY(toPop == 1))
				notFullCond.notify_one();
			else
				notFullCond.notify_all();

			return toPop;
		}

		/**
		 * @brief Blocking bulk retrieval with timeout
		 * @tparam M METHOD POP_METHOD selection (assign/place)
		 * @tparam Rep Chrono duration representation type
		 * @tparam Period Chrono duration period type
		 * @param elements Array to store retrieved elements
		 * @param count Maximum number of elements to retrieve
		 * @param timeout Maximum time to wait for data
		 * @return Actual number of elements retrieved
		 */
		template <POP_METHOD M = ASSIGN, class Rep, class Period>
		unsigned int wait_popBulk(TYPE* elements, unsigned int count, const std::chrono::duration<Rep, Period>& timeout)
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
				pop_extract<M>(elements[i], dataListHead->data);
				conditional_destroy(dataListHead->data);
				dataListHead = dataListHead->next;
			}

			size -= toPop;
			lock.unlock();

			if (LIKELY(toPop == 1))
				notFullCond.notify_one();
			else
				notFullCond.notify_all();

			return toPop;
		}

		/**
		 * @brief Signals all waiting threads to stop blocking
		 * @details Sets the stop flag and notifies all condition variables
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
		 * @details Frees memory block and resets to initial state
		 */
		void release()
		{
			if (memoryBlock)
			{
				Node* nodes = (Node*)memoryBlock;
				Node* current = dataListHead;

				for (unsigned int i = 0; i < size; ++i)
				{
					conditional_destroy(current->data);
					current = current->next;
				}

				ALIGNED_FREE(memoryBlock);
				isStopped = 0;
				memoryBlock = nullptr;
			}
		}

		// Disable copying
		TPBlockQueue(const TPBlockQueue&) = delete;
		TPBlockQueue& operator=(const TPBlockQueue&) = delete;
	};
}
#endif // HSLL_TPBLOCKQUEUE