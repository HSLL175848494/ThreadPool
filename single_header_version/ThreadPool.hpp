#ifndef HSLL_THREADPOOL
#define HSLL_THREADPOOL

#include <vector>
#include <atomic>
#include <thread>
#include <future>
#include <assert.h>

#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

#define HSLL_ALLOW_THROW

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
	//extern
	template <unsigned int TSIZE, unsigned int ALIGN>
	class TaskStack;

	template <class F, class... Args>
	struct TaskImpl;

	template <class F, class... Args>
	struct HeapCallable;

	template <class R, class F, class... Args>
	struct HeapCallable_Async;

	template <class R, class F, class... Args>
	struct HeapCallable_Cancelable;

	//helper1_sfinae
	template <typename T>
	struct is_generic_ti : std::false_type {};

	template <class T, class... Args>
	struct is_generic_ti<TaskImpl<T, Args...>> : std::true_type {};

	template <typename T>
	struct is_generic_ts : std::false_type {};

	template <unsigned int S, unsigned int A>
	struct is_generic_ts<TaskStack<S, A>> : std::true_type {};

	template <typename T>
	struct is_generic_hc : std::false_type {};

	template <class F, class... Args>
	struct is_generic_hc<HeapCallable<F, Args...>> : std::true_type {};

	template <typename T>
	struct is_generic_hc_async : std::false_type {};

	template <class R, class F, class... Args>
	struct is_generic_hc_async<HeapCallable_Async<R, F, Args...>> : std::true_type {};

	template <typename T>
	struct is_generic_hc_cancel : std::false_type {};

	template <class R, class F, class... Args>
	struct is_generic_hc_cancel<HeapCallable_Cancelable<R, F, Args...>> : std::true_type {};

	//helper2_is_moveable_copyable
	template <typename T>
	struct is_move_constructible
	{
	private:
		template <typename U, typename = decltype(U(std::declval<U&&>()))>
		static constexpr std::true_type test_move(int);

		template <typename>
		static constexpr std::false_type test_move(...);

	public:
		static constexpr bool value = decltype(test_move<T>(true))::value;
	};

	template <typename T>
	struct is_copy_constructible
	{
	private:
		template <typename U, typename = decltype(U{ std::declval<U&>() }) >
		static constexpr std::true_type test_copy(bool);

		template <typename>
		static constexpr std::false_type test_copy(...);

	public:
		static constexpr bool value = decltype(test_copy<T>(true))::value;
	};

	//helper3_index_sequence
	template <size_t... Is>
	struct index_sequence {};

	template <size_t N, size_t... Is>
	struct make_index_sequence_impl : make_index_sequence_impl<N - 1, N - 1, Is...> {};

	template <size_t... Is>
	struct make_index_sequence_impl<0, Is...>
	{
		using type = index_sequence<Is...>;
	};

	template <size_t N>
	struct make_index_sequence
	{
		using type = typename make_index_sequence_impl<N>::type;
	};

	//helper4
	template <bool...>
	struct bool_pack;

	template <bool... Bs>
	using all_true = std::is_same<bool_pack<true, Bs...>, bool_pack<Bs..., true>>;

	template <typename... Ts>
	using are_all_copy_constructible = all_true<is_copy_constructible<Ts>::value...>;

	//helper5_invoke
	enum TASK_TUPLE_TYPE
	{
		normal,
		async,
		cancelable
	};

	template<TASK_TUPLE_TYPE TYPE>
	struct Invoker {};

	template<>
	struct Invoker<normal>
	{
		template <class R, typename std::enable_if<std::is_void<R>::value>::type = true,
			typename Callable, typename... Ts>
		static void invoke(Callable& callable, Ts &...args)
		{
			callable(args...);
		}

		template <class R, typename Callable, typename... Ts>
		static R invoke(Callable& callable, Ts &...args)
		{
			return callable(args...);
		}
	};

	template<>
	struct Invoker<async>
	{
		template <class R, typename std::enable_if<std::is_void<R>::value>::type = true,
			typename Promise, typename Callable, typename... Ts>
		static void invoke(Promise& promise, Callable& callable, Ts &...args)
		{
			callable(args...);
		}

		template <class R, typename Promise, typename Callable, typename... Ts>
		static R invoke(Promise& promise, Callable& callable, Ts &...args)
		{
			return callable(args...);
		}
	};

	template<>
	struct Invoker<cancelable>
	{
		template <class R, typename std::enable_if<std::is_void<R>::value>::type = true,
			typename Promise, typename Callable, typename... Ts>
		static void invoke(Promise& promise, std::atomic<bool>& flag, Callable& callable, Ts &...args)
		{
			callable(args...);
		}

		template <class R, typename Promise, typename Callable, typename... Ts>
		static R invoke(Promise& promise, std::atomic<bool>& flag, Callable& callable, Ts &...args)
		{
			return callable(args...);
		}
	};

	//helper6_apply
	template <TASK_TUPLE_TYPE TYPE, class R, typename std::enable_if<std::is_void<R>::value>::type = true,
		typename Tuple, size_t... Is>
	void apply_impl(Tuple& tup, index_sequence<Is...>)
	{
		Invoker<TYPE>::template invoke<R>(std::get<Is>(tup)...);
	}

	template <TASK_TUPLE_TYPE TYPE, class R, typename Tuple, size_t... Is>
	R apply_impl(Tuple& tup, index_sequence<Is...>)
	{
		return Invoker<TYPE>::template invoke<R>(std::get<Is>(tup)...);
	}

	template <TASK_TUPLE_TYPE TYPE = normal, class R = void, typename std::enable_if<std::is_void<R>::value>::type = true,
		typename Tuple>
	void tuple_apply(Tuple& tup)
	{
		apply_impl<TYPE, R>(tup, typename make_index_sequence<std::tuple_size<Tuple>::value>::type{});
	}

	template <TASK_TUPLE_TYPE TYPE = normal, class R = void, typename Tuple>
	R tuple_apply(Tuple& tup)
	{
		return apply_impl<TYPE, R>(tup, typename make_index_sequence<std::tuple_size<Tuple>::value>::type{});
	}

	//helper7_make_unique
	template <typename T, typename... Args>
	typename std::enable_if<!std::is_array<T>::value, std::unique_ptr<T>>::type
		make_unique(Args&&... args) {
		return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
	}

	template <typename T>
	typename std::enable_if<std::is_array<T>::value&& std::extent<T>::value == 0, std::unique_ptr<T>>::type
		make_unique(std::size_t size) {
		using U = typename std::remove_extent<T>::type;
		return std::unique_ptr<T>(new U[size]());
	}

	template <typename T, typename... Args>
	typename std::enable_if<std::extent<T>::value != 0, void>::type
		make_unique(Args&&...) = delete;

	/**
	 * @class HeapCallable
	 * @brief Encapsulates a callable object and its arguments, storing them on the heap.
	 *        This class is move-only and non-assignable
	 * @tparam F Type of the callable object
	 * @tparam Args Types of the arguments bound to the callable
	 */
	template <class F, class... Args>
	class HeapCallable
	{
	private:
		using Package = std::tuple<typename std::decay<F>::type, typename std::decay<Args>::type...>;
		std::unique_ptr<Package> storage;

	public:
		/**
		 * @brief Constructs a HeapCallable by moving the callable and arguments
		 * @param func Callable object to store
		 * @param args Arguments to bind to the callable
		 */
		template<class Func, typename std::enable_if<!is_generic_hc<typename std::decay<Func>::type>::value, int>::type = 0 >
		HeapCallable(Func&& func, Args &&...args) HSLL_ALLOW_THROW
			: storage(HSLL::make_unique<Package>(std::forward<Func>(func), std::forward<Args>(args)...)) {}

		/**
		 * @brief Invokes the stored callable with bound arguments
		 * @pre Object must be in a valid state (storage != nullptr)
		 */
		void operator()()
		{
			assert(storage);
			tuple_apply(*storage);
		}
	};

	/**
	 * @class HeapCallable_Async
	 * @brief Asynchronous version of HeapCallable that provides a future for the result.
	 *        This class is move-only and non-assignable
	 * @tparam R Return type of the callable
	 * @tparam F Type of the callable object
	 * @tparam Args Types of the arguments bound to the callable
	 */
	template <class R, class F, class... Args>
	class HeapCallable_Async
	{
		using Package = std::tuple<std::promise<R>,
			typename std::decay<F>::type, typename std::decay<Args>::type...>;

	private:
		std::unique_ptr<Package> storage;

		template<class T = R, typename std::enable_if<std::is_void<T>::value>::type = true>
		void invoke() {
			auto& promise = std::get<0>(*storage);
			try {
				tuple_apply<async, R>(*storage);
				promise.set_value();
			}
			catch (...) {
				promise.set_exception(std::current_exception());
			}
		}

		template<class T = R>
		void invoke() {
			auto& promise = std::get<0>(*storage);
			try {
				promise.set_value(tuple_apply<async, R>(*storage));
			}
			catch (...) {
				promise.set_exception(std::current_exception());
			}
		}

	public:
		/**
		 * @brief Constructs an async callable object
		 * @param func Callable object to store
		 * @param args Arguments to bind to the callable
		 */
		template<class Func, typename std::enable_if<!is_generic_hc_async<typename std::decay<Func>::type>::value, int>::type = 0 >
		HeapCallable_Async(Func&& func, Args &&...args) HSLL_ALLOW_THROW
			: storage(HSLL::make_unique<Package>(std::promise<R>(), std::forward<Func>(func), std::forward<Args>(args)...)) {}

		/**
		 * @brief Executes the callable and sets promise value/exception
		 * @pre Object must be in a valid state (storage != nullptr)
		 */
		void operator()()
		{
			assert(storage);
			invoke();
		}

		/**
		 * @brief Retrieves the future associated with the promise
		 * @return std::future<R> Future object for the call result
		 * @pre Object must be in a valid state (storage != nullptr)
		 */
		std::future<R> get_future()
		{
			assert(storage);
			return std::get<0>(*storage).get_future();
		}
	};

	/**
	 * @class HeapCallable_Cancelable
	 * @brief Cancelable version of HeapCallable with atomic cancellation flag.
	 *        This class is move-only and non-assignable
	 * @tparam R Return type of the callable
	 * @tparam F Type of the callable object
	 * @tparam Args Types of the arguments bound to the callable
	 */
	template <class R, class F, class... Args>
	class HeapCallable_Cancelable
	{
		using Package = std::tuple<std::promise<R>, std::atomic<bool>,
			typename std::decay<F>::type, typename std::decay<Args>::type...>;

	private:
		std::shared_ptr<Package> storage;

		template<class T = R, typename std::enable_if<std::is_void<T>::value>::type = true>
		void invoke() {
			auto& promise = std::get<0>(*storage);
			try {
				tuple_apply<cancelable, T>(*storage);
				promise.set_value(1);
			}
			catch (...) {
				promise.set_exception(std::current_exception());
			}
		}

		template<class T = R>
		void invoke() {
			auto& promise = std::get<0>(*storage);
			try {
				promise.set_value(tuple_apply<cancelable, T>(*storage));
			}
			catch (...) {
				promise.set_exception(std::current_exception());
			}
		}

	public:

		struct Controller
		{
		private:
			std::shared_ptr<Package> storage;

		public:

			Controller(std::shared_ptr<Package> storage) :storage(storage) {};

			/**
			 * @brief Attempts to cancel the callable execution
			 * @return true if successfully canceled, false if already executed
			 * @pre Object must be in a valid state (storage != nullptr)
			 */
			bool cancel()
			{
				assert(storage);
				bool expected = false;
				auto& flag = std::get<1>(*storage);

				if (flag.compare_exchange_strong(expected, true))
				{
					auto& promise = std::get<0>(*storage);
					promise.set_exception(std::make_exception_ptr(std::runtime_error("Task canceled")));
					return true;
				}
				return false;
			}
		};

		/**
		 * @brief Constructs a cancelable callable object
		 * @param func Callable object to store
		 * @param args Arguments to bind to the callable
		 */
		template<class Func, typename std::enable_if<!is_generic_hc_cancel<typename std::decay<Func>::type>::value, int>::type = 0 >
		HeapCallable_Cancelable(Func&& func, Args &&...args) HSLL_ALLOW_THROW
			: storage(std::make_shared<Package>(std::promise<R>(), false, std::forward<Func>(func), std::forward<Args>(args)...)) {}

		/**
		 * @brief Executes the callable if not canceled
		 * @pre Object must be in a valid state (storage != nullptr)
		 */
		void operator()()
		{
			assert(storage);
			bool expected = false;
			auto& flag = std::get<1>(*storage);

			if (flag.compare_exchange_strong(expected, true))
				invoke();
		}

		Controller get_controller()
		{
			assert(storage);
			return Controller(storage);
		}

		/**
		 * @brief Retrieves the future associated with the promise
		 * @return std::future<R> Future object for the call result
		 * @pre Object must be in a valid state (storage != nullptr)
		 */
		std::future<R> get_future()
		{
			assert(storage);
			return std::get<0>(*storage).get_future();
		}

		HeapCallable_Cancelable(const HeapCallable_Cancelable& other) = delete;
		HeapCallable_Cancelable& operator=(const HeapCallable_Cancelable& other) = delete;
		HeapCallable_Cancelable(HeapCallable_Cancelable&& other) noexcept = default;
		HeapCallable_Cancelable& operator=(HeapCallable_Cancelable&& other) noexcept = default;
	};

	/**
	 * @brief Factory function to create HeapCallable objects
	 * @tparam F Type of callable object
	 * @tparam Args Types of arguments to bind
	 * @param func Callable target function
	 * @param args Arguments to bind to function call
	 * @return HeapCallable instance
	 */
	template <typename F, typename... Args>
	HeapCallable<F, Args...> make_callable(F&& func, Args &&...args) HSLL_ALLOW_THROW
	{
		return HeapCallable<F, Args...>(std::forward<F>(func), std::forward<Args>(args)...);
	}

	/**
	 * @brief Factory function to create HeapCallable_Async objects
	 * @tparam ResultType Type of return value
	 * @tparam F Type of callable object
	 * @tparam Args Types of arguments to bind
	 * @param func Callable target function
	 * @param args Arguments to bind to function call
	 * @return HeapCallable_Async instance
	 */
	template <typename ResultType, typename F, typename... Args>
	HeapCallable_Async<ResultType, F, Args...> make_callable_async(F&& func, Args &&...args) HSLL_ALLOW_THROW
	{
		return HeapCallable_Async<ResultType, F, Args...>
			(std::forward<F>(func), std::forward<Args>(args)...);
	}

	/**
	 * @brief Factory function to create HeapCallable_Cancelable objects
	 * @tparam ResultType Type of return value
	 * @tparam F Type of callable object
	 * @tparam Args Types of arguments to bind
	 * @param func Callable target function
	 * @param args Arguments to bind to function call
	 * @return HeapCallable_Async instance
	 */
	template <typename ResultType, typename F, typename... Args>
	HeapCallable_Cancelable<ResultType, F, Args...> make_callable_cancelable(F&& func, Args &&...args) HSLL_ALLOW_THROW
	{
		return HeapCallable_Cancelable<ResultType, F, Args...>
			(std::forward<F>(func), std::forward<Args>(args)...);
	}

	/**
	 * @brief Base interface for type-erased task objects
	 * @details Provides virtual methods for task execution and storage management
	 */
	struct TaskBase
	{
		virtual ~TaskBase() = default;
		virtual void execute() noexcept = 0;
		virtual void copyTo(void* memory) const noexcept = 0;
		virtual void moveTo(void* memory) noexcept = 0;
		virtual bool is_copyable() const noexcept = 0;
	};

	/**
	 * @brief Concrete task implementation storing function and arguments
	 * @details Stores decayed copies of function and arguments in a tuple
	 */
	template <class F, class... Args>
	struct TaskImpl : TaskBase
	{
		template <typename T, bool Copyable>
		struct CopyHelper;

		template <typename T>
		struct CopyHelper<T, true>
		{
			static void copyTo(const T* self, void* dst) noexcept
			{
				new (dst) T(*self);
			}
		};

		template <typename T>
		struct CopyHelper<T, false>
		{
			static void copyTo(const T*, void*) noexcept
			{
				printf("\nTaskImpl must be copy constructible for cloneTo()");
				std::abort();
			}
		};

		template <typename T, bool Movable>
		struct MoveHelper;

		template <typename T>
		struct MoveHelper<T, true>
		{
			static T&& apply(T& obj) noexcept
			{
				return std::move(obj);
			}
		};

		template <typename T>
		struct MoveHelper<T, false>
		{
			static T& apply(T& obj) noexcept
			{
				return obj;
			}
		};

		using Tuple = std::tuple<typename std::decay<F>::type, typename std::decay<Args>::type...>;
		Tuple storage;

		void tuple_move(void* dst)
		{
			move_impl(dst, typename make_index_sequence<std::tuple_size<Tuple>::value>::type{});
		}

		template <size_t... Is>
		void move_impl(void* dst, index_sequence<Is...>)
		{
			tmove(dst, std::get<Is>(storage)...);
		}

		template <typename... Ts>
		void tmove(void* dst, Ts &...args)
		{
			new (dst) TaskImpl(MoveHelper<Ts, is_move_constructible<Ts>::value>::apply(args)...);
		}

		template <class Func, class... Params,
			typename std::enable_if<!is_generic_ti<typename std::decay<Func>::type>::value, int>::type = 0>
		TaskImpl(Func&& func, Params &&...args)
			: storage(std::forward<Func>(func), std::forward<Params>(args)...) {}

		void execute() noexcept override
		{
			tuple_apply(storage);
		}

		void copyTo(void* dst) const noexcept override
		{
			CopyHelper<TaskImpl,
				are_all_copy_constructible<typename std::decay<F>::type,
				typename std::decay<Args>::type...>::value>::copyTo(this, dst);
		}

		void moveTo(void* dst) noexcept override
		{
			tuple_move(dst);
		}

		bool is_copyable() const noexcept override
		{
			return are_all_copy_constructible<
				typename std::decay<F>::type,
				typename std::decay<Args>::type...>::value;
		}
	};

	/**
	 * @brief Metafunction to compute the task implementation type and its size
	 * @tparam F Type of callable object
	 * @tparam Args Types of bound arguments
	 * @details Provides:
	 *   - `type`: Concrete TaskImpl type for given function and arguments
	 *   - `size`: Size in bytes of the TaskImpl type
	 */
	template <class F, class... Args>
	struct task_stack
	{
		using type = TaskImpl<F, Args...>;
		static constexpr unsigned int size = sizeof(type);
	};

#if __cplusplus >= 201402L
	template <class F, class... Args>
	static constexpr unsigned int task_stack_size = sizeof(task_stack<F, Args...>::size);
#endif

	/**
	 * @brief Stack-allocated task container with fixed-size storage
	 * @tparam TSIZE Size of internal storage buffer (default = 64)
	 * @tparam ALIGN Alignment requirement for storage (default = 8)
	 */
	template <unsigned int TSIZE = 64, unsigned int ALIGN = 8>
	class TaskStack
	{
		static_assert(TSIZE >= 24, "TSIZE must >= 24");
		static_assert(ALIGN >= alignof(void*), "Alignment must >= alignof(void*)");
		static_assert(TSIZE% ALIGN == 0, "TSIZE must be a multiple of ALIGN");
		alignas(ALIGN) char storage[TSIZE];

		/**
		 * @brief Helper template to conditionally create stack-allocated or heap-backed TaskStack
		 */
		template <bool Condition, typename F, typename... Args>
		struct Maker;

		template <typename F, typename... Args>
		struct Maker<true, F, Args...>
		{
			static TaskStack make(F&& func, Args &&...args)
			{
				return TaskStack(std::forward<F>(func), std::forward<Args>(args)...);
			}
		};

		template <typename F, typename... Args>
		struct Maker<false, F, Args...>
		{
			static TaskStack make(F&& func, Args &&...args)
			{
				return TaskStack(HeapCallable<F, Args...>(std::forward<F>(func), std::forward<Args>(args)...));
			}
		};

	public:
		/**
		 * @brief Metafunction to validate task compatibility with storage
		 * @tparam F Type of callable object
		 * @tparam Args Types of bound arguments
		 * @value true if task fits in storage and meets alignment requirements
		 */
		template <class F, class... Args>
		struct task_invalid
		{
			static constexpr bool value = sizeof(typename task_stack<F, Args...>::type) <= sizeof(TaskStack) &&
				alignof(typename task_stack<F, Args...>::type) <= ALIGN;
		};

#if __cplusplus >= 201402L
		template <class F, class... Args>
		static constexpr bool task_invalid_v = sizeof(typename task_stack<F, Args...>::type) <= TSIZE &&
			alignof(typename task_stack<F, Args...>::type) <= ALIGN;
#endif

		/**
		 * @brief Constructs task in internal storage
		 * @tparam F Type of callable object
		 * @tparam Args Types of bound arguments
		 * @param func Callable target function
		 * @param args Arguments to bind to function call
		 * @note Disables overload when F is a TaskStack (prevents nesting)
		 * @note Static assertion ensures storage size is sufficient
		 *
		 * Important usage note:
		 * - Argument value category (lvalue/rvalue) affects ONLY how
		 *   arguments are stored internally (copy vs move construction)
		 * - During execute(), arguments are ALWAYS passed as lvalues
		 * - Functions with rvalue reference parameters are NOT supported
		 *   Example: void bad_func(std::string&&) // Not allowed
		 */
		template <class F, class... Args,
			typename std::enable_if<!is_generic_ts<typename std::decay<F>::type>::value, int>::type = 0>
		TaskStack(F&& func, Args &&...args) HSLL_ALLOW_THROW
		{
			typedef typename task_stack<F, Args...>::type ImplType;
			static_assert(sizeof(ImplType) <= TSIZE, "TaskImpl size exceeds storage");
			static_assert(alignof(ImplType) <= ALIGN, "TaskImpl alignment exceeds storage alignment");
			new (storage) ImplType(std::forward<F>(func), std::forward<Args>(args)...);
		}

		/**
		 * @brief Factory method that automatically selects storage strategy
		 * @tparam F Type of callable object
		 * @tparam Args Types of bound arguments
		 * @param func Callable to store
		 * @param args Arguments to bind
		 * @return TaskStack with either:
		 *         - Directly stored task if it fits in stack buffer
		 *         - Heap-allocated fallback via HeapCallable otherwise
		 */
		template <class F, class... Args>
		static TaskStack make_auto(F&& func, Args &&...args) HSLL_ALLOW_THROW
		{
			return Maker<task_invalid<F, Args...>::value, F, Args...>::make(
				std::forward<F>(func),
				std::forward<Args>(args)...);
		}

		/**
		 * @brief Factory function to create a heap-allocated callable task
		 * @tparam F Type of callable object
		 * @tparam Args Types of bound arguments
		 * @param func Callable object to store
		 * @param args Arguments to bind
		 * @return TaskStack containing heap-stored callable (move-only)
		 */
		template <class F, class... Args>
		static TaskStack make_heap(F&& func, Args &&...args) HSLL_ALLOW_THROW
		{
			return TaskStack(HeapCallable<F, Args...>(std::forward<F>(func), std::forward<Args>(args)...));
		}

		void execute() noexcept
		{
			getBase()->execute();
		}

		bool is_copyable() const noexcept
		{
			return getBase()->is_copyable();
		}

		bool is_moveable() const noexcept
		{
			return true;
		}

		TaskStack(const TaskStack& other) noexcept
		{
			other.getBase()->copyTo(storage);
		}

		TaskStack(TaskStack&& other) noexcept
		{
			other.getBase()->moveTo(storage);
		}

		~TaskStack() noexcept
		{
			getBase()->~TaskBase();
		}

		TaskStack& operator=(const TaskStack& other) = delete;
		TaskStack& operator=(TaskStack&& other) = delete;

	private:
		TaskBase* getBase() noexcept
		{
			return (TaskBase*)storage;
		}

		const TaskBase* getBase() const noexcept
		{
			return (const TaskBase*)storage;
		}
	};
}

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>

namespace HSLL
{
	class ReadWriteLock {
	public:
		ReadWriteLock() {
			InitializeSRWLock(&srwlock_);
		}

		~ReadWriteLock() = default;

		void lock_read() {
			AcquireSRWLockShared(&srwlock_);
		}

		void unlock_read() {
			ReleaseSRWLockShared(&srwlock_);
		}

		void lock_write() {
			AcquireSRWLockExclusive(&srwlock_);
		}

		void unlock_write() {
			ReleaseSRWLockExclusive(&srwlock_);
		}

		ReadWriteLock(const ReadWriteLock&) = delete;
		ReadWriteLock& operator=(const ReadWriteLock&) = delete;

	private:
		SRWLOCK srwlock_;
	};

	class Semaphore
	{
	public:
		explicit Semaphore(unsigned int initial_count = 0)
		{
			m_sem = CreateSemaphoreW(nullptr, static_cast<LONG>(initial_count),
				std::numeric_limits<LONG>::max(), nullptr);
			if (!m_sem)
				throw std::system_error(GetLastError(), std::system_category());
		}

		~Semaphore() noexcept
		{
			if (m_sem) CloseHandle(m_sem);
		}

		void acquire()
		{
			DWORD result = WaitForSingleObject(m_sem, INFINITE);
			if (result != WAIT_OBJECT_0)
				throw std::system_error(GetLastError(), std::system_category());
		}

		template<typename Rep, typename Period>
		bool try_acquire_for(const std::chrono::duration<Rep, Period>& timeout)
		{
			auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
			if (ms.count() < 0) ms = std::chrono::milliseconds(0);

			DWORD wait_ms;
			if (ms.count() > MAX_WAIT_MS)
				wait_ms = MAX_WAIT_MS;
			else
				wait_ms = static_cast<DWORD>(ms.count());

			DWORD result = WaitForSingleObject(m_sem, wait_ms);
			if (result == WAIT_OBJECT_0)
				return true;
			else if (result == WAIT_TIMEOUT)
				return false;
			else
				throw std::system_error(GetLastError(), std::system_category());
		}

		void release(unsigned int count = 1)
		{
			if (!ReleaseSemaphore(m_sem, static_cast<LONG>(count), nullptr))
				throw std::system_error(GetLastError(), std::system_category());
		}

		Semaphore(const Semaphore&) = delete;
		Semaphore& operator=(const Semaphore&) = delete;
		Semaphore(Semaphore&&) = delete;
		Semaphore& operator=(Semaphore&&) = delete;

	private:
		HANDLE m_sem = nullptr;
		static constexpr DWORD MAX_WAIT_MS = 0xFFFFFFF;
	};
}

#elif defined(__linux__) || defined(__unix__) || \
      defined(__APPLE__) || defined(__FreeBSD__) || \
      defined(__OpenBSD__) || defined(__NetBSD__)

#include <semaphore.h>
#include <pthread.h>

namespace HSLL
{
	class ReadWriteLock {
	public:
		ReadWriteLock() {
			pthread_rwlock_init(&rwlock_, nullptr);
		}

		~ReadWriteLock() {
			pthread_rwlock_destroy(&rwlock_);
		}

		void lock_read() {
			pthread_rwlock_rdlock(&rwlock_);
		}

		void unlock_read() {
			pthread_rwlock_unlock(&rwlock_);
		}

		void lock_write() {
			pthread_rwlock_wrlock(&rwlock_);
		}

		void unlock_write() {
			pthread_rwlock_unlock(&rwlock_);
		}

		ReadWriteLock(const ReadWriteLock&) = delete;
		ReadWriteLock& operator=(const ReadWriteLock&) = delete;

	private:
		pthread_rwlock_t rwlock_;
	};

	class Semaphore
	{
	public:
		explicit Semaphore(unsigned int initial_count = 0)
		{
			if (sem_init(&m_sem, 0, initial_count) != 0)
				throw std::system_error(errno, std::system_category());
		}

		~Semaphore() noexcept
		{
			sem_destroy(&m_sem);
		}

		void acquire()
		{
			int ret;
			do {
				ret = sem_wait(&m_sem);
			} while (ret != 0 && errno == EINTR);

			if (ret != 0)
				throw std::system_error(errno, std::system_category());
		}

		template<typename Rep, typename Period>
		bool try_acquire_for(const std::chrono::duration<Rep, Period>& timeout)
		{
			auto abs_time = std::chrono::system_clock::now() + timeout;
			struct timespec ts;
			auto s = std::chrono::time_point_cast<std::chrono::seconds>(abs_time);
			auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(abs_time - s);

			ts.tv_sec = s.time_since_epoch().count();
			ts.tv_nsec = ns.count();

			while (true) {
				int result = sem_timedwait(&m_sem, &ts);
				if (result == 0)
					return true;
				else if (errno == EINTR)
					continue;
				else if (errno == ETIMEDOUT)
					return false;
				else
					throw std::system_error(errno, std::system_category());
			}
		}

		void release(unsigned int count = 1)
		{
			for (unsigned int i = 0; i < count; ++i) {
				if (sem_post(&m_sem) != 0) {
					throw std::system_error(errno, std::system_category());
				}
			}
		}

		Semaphore(const Semaphore&) = delete;
		Semaphore& operator=(const Semaphore&) = delete;
		Semaphore(Semaphore&&) = delete;
		Semaphore& operator=(Semaphore&&) = delete;

	private:
		sem_t m_sem;
	};
}


#else
#error "Unsupported platform"
#endif

namespace HSLL
{
	class ReadLockGuard {
	public:
		explicit ReadLockGuard(ReadWriteLock& lock) : lock_(lock) {
			lock_.lock_read();
		}

		~ReadLockGuard() {
			lock_.unlock_read();
		}

		ReadLockGuard(const ReadLockGuard&) = delete;
		ReadLockGuard& operator=(const ReadLockGuard&) = delete;

	private:
		ReadWriteLock& lock_;
	};

	class WriteLockGuard {
	public:
		explicit WriteLockGuard(ReadWriteLock& lock) : lock_(lock) {
			lock_.lock_write();
		}

		~WriteLockGuard() {
			lock_.unlock_write();
		}

		WriteLockGuard(const WriteLockGuard&) = delete;
		WriteLockGuard& operator=(const WriteLockGuard&) = delete;

	private:
		ReadWriteLock& lock_;
	};
}

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
		template <typename... Args>
		void emplace_impl(InsertAtTailTag, Args &&...args)
		{
			new (dataListTail) TYPE(std::forward<Args>(args)...);
			move_tail_next();
		}

		template <typename... Args>
		void emplace_impl(InsertAtHeadTag, Args &&...args)
		{
			move_head_prev();
			new (dataListHead) TYPE(std::forward<Args>(args)...);
		}

		template <class T>
		void enqueue_impl(InsertAtTailTag, T&& element)
		{
			new (dataListTail) TYPE(std::forward<T>(element));
			move_tail_next();
		}

		template <class T>
		void enqueue_impl(InsertAtHeadTag, T&& element)
		{
			move_head_prev();
			new (dataListHead) TYPE(std::forward<T>(element));
		}

		template <BULK_CMETHOD METHOD>
		void enqueue_bulk_impl(InsertAtTailTag, TYPE* elements, unsigned int toPush)
		{
			for (unsigned int i = 0; i < toPush; ++i)
			{
				bulk_construct<METHOD>(*dataListTail, elements[i]);
				move_tail_next();
			}
		}

		template <BULK_CMETHOD METHOD>
		void enqueue_bulk_impl(InsertAtHeadTag, TYPE* elements, unsigned int toPush)
		{
			for (unsigned int i = 0; i < toPush; ++i)
			{
				move_head_prev();
				bulk_construct<METHOD>(*dataListHead, elements[toPush - i - 1]);
			}
		}

		template <BULK_CMETHOD METHOD>
		void enqueue_bulk_impl(InsertAtTailTag, TYPE* part1, unsigned int count1, TYPE* part2, unsigned int count2)
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

		template <BULK_CMETHOD METHOD>
		void enqueue_bulk_impl(InsertAtHeadTag, TYPE* part1, unsigned int count1, TYPE* part2, unsigned int count2)
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
			size.fetch_add(1, std::memory_order_release);
			using InsertTag = typename std::conditional<POS == HEAD, InsertAtHeadTag, InsertAtTailTag>::type;
			emplace_impl(InsertTag(), std::forward<Args>(args)...);
			lock.unlock();
			notEmptyCond.notify_one();
		}

		template <INSERT_POS POS, class T>
		void enqueue_helper(std::unique_lock<std::mutex>& lock, T&& element)
		{
			size.fetch_add(1, std::memory_order_release);
			using InsertTag = typename std::conditional<POS == HEAD, InsertAtHeadTag, InsertAtTailTag>::type;
			enqueue_impl(InsertTag(), std::forward<T>(element));
			lock.unlock();
			notEmptyCond.notify_one();
		}

		template <BULK_CMETHOD METHOD, INSERT_POS POS>
		unsigned int enqueue_bulk_helper(std::unique_lock<std::mutex>& lock, TYPE* elements, unsigned int count)
		{
			unsigned int toPush = std::min(count, maxSize - size.load(std::memory_order_relaxed));
			size.fetch_add(toPush, std::memory_order_release);
			using InsertTag = typename std::conditional<POS == HEAD, InsertAtHeadTag, InsertAtTailTag>::type;
			enqueue_bulk_impl<METHOD>(InsertTag(), elements, toPush);
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
			unsigned int toPush = std::min(count1 + count2, maxSize - size.load(std::memory_order_relaxed));
			size.fetch_add(toPush, std::memory_order_release);
			using InsertTag = typename std::conditional<POS == HEAD, InsertAtHeadTag, InsertAtTailTag>::type;

			if (toPush > count1)
				enqueue_bulk_impl<METHOD>(InsertTag(), part1, count1, part2, toPush - count1);
			else
				enqueue_bulk_impl<METHOD>(InsertTag(), part1, toPush);

			lock.unlock();

			if (UNLIKELY(toPush == 1))
				notEmptyCond.notify_one();
			else
				notEmptyCond.notify_all();
			return toPush;
		}

		void dequeue_helper(std::unique_lock<std::mutex>& lock, TYPE& element)
		{
			size.fetch_sub(1, std::memory_order_release);
			move_element_dequeue(element, *dataListHead);
			move_head_next();
			lock.unlock();
			notFullCond.notify_one();
		}

		unsigned int dequeue_bulk_helper(std::unique_lock<std::mutex>& lock, TYPE* elements, unsigned int count)
		{
			unsigned int toPop = std::min(count, size.load(std::memory_order_relaxed));
			size.fetch_sub(toPop, std::memory_order_release);
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

	public:

		TPBlockQueue() : memoryBlock(nullptr), isStopped(0) {}

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

		template <INSERT_POS POS = TAIL, typename... Args>
		bool emplace(Args &&...args)
		{
			assert(memoryBlock);
			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(size.load(std::memory_order_relaxed) == maxSize))
				return false;

			emplace_helper<POS>(lock, std::forward<Args>(args)...);
			return true;
		}

		template <INSERT_POS POS = TAIL, typename... Args>
		typename std::enable_if<!is_generic_dr<typename std::tuple_element<0, std::tuple<Args...>>::type>::value, bool>::type
			wait_emplace(Args &&...args)
		{
			assert(memoryBlock);
			std::unique_lock<std::mutex> lock(dataMutex);

			notFullCond.wait(lock, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed) != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return false;

			emplace_helper<POS>(lock, std::forward<Args>(args)...);
			return true;
		}

		template <INSERT_POS POS = TAIL, class Rep, class Period, typename... Args>
		bool wait_emplace(const std::chrono::duration<Rep, Period>& timeout, Args &&...args)
		{
			assert(memoryBlock);
			std::unique_lock<std::mutex> lock(dataMutex);

			bool success = notFullCond.wait_for(lock, timeout, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed) != maxSize) || UNLIKELY(isStopped); });

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

			if (UNLIKELY(size.load(std::memory_order_relaxed) == maxSize))
				return false;

			enqueue_helper<POS>(lock, std::forward<T>(element));
			return true;
		}

		template <INSERT_POS POS = TAIL, class T>
		bool wait_enqueue(T&& element)
		{
			assert(memoryBlock);
			std::unique_lock<std::mutex> lock(dataMutex);

			notFullCond.wait(lock, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed) != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return false;

			enqueue_helper<POS>(lock, std::forward<T>(element));
			return true;
		}

		template <INSERT_POS POS = TAIL, class T, class Rep, class Period>
		bool wait_enqueue(T&& element, const std::chrono::duration<Rep, Period>& timeout)
		{
			assert(memoryBlock);
			std::unique_lock<std::mutex> lock(dataMutex);

			bool success = notFullCond.wait_for(lock, timeout, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed) != maxSize) || UNLIKELY(isStopped); });

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

			if (UNLIKELY(!(maxSize - size.load(std::memory_order_relaxed))))
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

			if (UNLIKELY(!(maxSize - size.load(std::memory_order_relaxed))))
				return 0;

			return enqueue_bulk_helper<METHOD, POS>(lock, part1, count1, part2, count2);
		}

		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL>
		unsigned int wait_enqueue_bulk(TYPE* elements, unsigned int count)
		{
			assert(memoryBlock);
			assert(elements && count);
			std::unique_lock<std::mutex> lock(dataMutex);

			notFullCond.wait(lock, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed) != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return 0;

			return enqueue_bulk_helper<METHOD, POS>(lock, elements, count);
		}

		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL, class Rep, class Period>
		unsigned int wait_enqueue_bulk(TYPE* elements, unsigned int count, const std::chrono::duration<Rep, Period>& timeout)
		{
			assert(memoryBlock);
			assert(elements && count);
			std::unique_lock<std::mutex> lock(dataMutex);

			bool success = notFullCond.wait_for(lock, timeout, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed) != maxSize) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || isStopped))
				return 0;

			return enqueue_bulk_helper<METHOD, POS>(lock, elements, count);
		}

		bool dequeue(TYPE& element)
		{
			assert(memoryBlock);
			std::unique_lock<std::mutex> lock(dataMutex);

			if (UNLIKELY(!size.load(std::memory_order_relaxed)))
				return false;

			dequeue_helper(lock, element);
			return true;
		}

		bool wait_dequeue(TYPE& element)
		{
			assert(memoryBlock);
			std::unique_lock<std::mutex> lock(dataMutex);

			notEmptyCond.wait(lock, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed)) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return false;

			dequeue_helper(lock, element);
			return true;
		}

		template <class Rep, class Period>
		bool wait_dequeue(TYPE& element, const std::chrono::duration<Rep, Period>& timeout)
		{
			assert(memoryBlock);
			std::unique_lock<std::mutex> lock(dataMutex);

			bool success = notEmptyCond.wait_for(lock, timeout, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed)) || UNLIKELY(isStopped); });

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

			if (UNLIKELY(!size.load(std::memory_order_relaxed)))
				return 0;

			return dequeue_bulk_helper(lock, elements, count);
		}

		unsigned int wait_dequeue_bulk(TYPE* elements, unsigned int count)
		{
			assert(memoryBlock);
			assert(elements && count);
			std::unique_lock<std::mutex> lock(dataMutex);

			notEmptyCond.wait(lock, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed)) || UNLIKELY(isStopped); });

			if (UNLIKELY(isStopped))
				return 0;

			return dequeue_bulk_helper(lock, elements, count);
		}

		template <class Rep, class Period>
		unsigned int wait_dequeue_bulk(TYPE* elements, unsigned int count, const std::chrono::duration<Rep, Period>& timeout)
		{
			assert(memoryBlock);
			assert(elements && count);
			std::unique_lock<std::mutex> lock(dataMutex);

			bool success = notEmptyCond.wait_for(lock, timeout, [this]
				{ return LIKELY(size.load(std::memory_order_relaxed)) || UNLIKELY(isStopped); });

			if (UNLIKELY(!success || isStopped))
				return 0;

			return dequeue_bulk_helper(lock, elements, count);
		}

		unsigned int get_size()
		{
			assert(memoryBlock);
			return size.load(std::memory_order_relaxed);
		}

		unsigned int get_exact_size()
		{
			assert(memoryBlock);
			return size.load(std::memory_order_acquire);
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
			assert(memoryBlock);

			{
				std::lock_guard<std::mutex> lock(dataMutex);
				isStopped = 1;
			}

			notEmptyCond.notify_all();
			notFullCond.notify_all();
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
			maxSize = 0;
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

#define HSLL_THREADPOOL_TIMEOUT_MILLISECONDS 5
#define HSLL_THREADPOOL_SHRINK_FACTOR 0.25
#define HSLL_THREADPOOL_EXPAND_FACTOR 0.75

static_assert(HSLL_THREADPOOL_TIMEOUT_MILLISECONDS > 0, "Invalid timeout value.");
static_assert(HSLL_THREADPOOL_SHRINK_FACTOR < HSLL_THREADPOOL_EXPAND_FACTOR&& HSLL_THREADPOOL_EXPAND_FACTOR < 1.0
	&& HSLL_THREADPOOL_SHRINK_FACTOR>0.0, "Invalid factors.");

namespace HSLL
{
	template <class T>
	class SingleStealer
	{
		template <class TYPE>
		friend class ThreadPool;
	private:

		unsigned int index;
		unsigned int queueLength;
		unsigned int* threadNum;
		unsigned int threshold;

		ReadWriteLock* rwLock;
		TPBlockQueue<T>* queues;
		TPBlockQueue<T>* ignore;

		SingleStealer(ReadWriteLock* rwLock, TPBlockQueue<T>* queues, TPBlockQueue<T>* ignore,
			unsigned int queueLength, unsigned int* threadNum)
		{
			this->index = 0;
			this->queueLength = queueLength;
			this->threadNum = threadNum;
			this->threshold = std::min((unsigned int)2, queueLength);
			this->rwLock = rwLock;
			this->queues = queues;
			this->ignore = ignore;
		}

		unsigned int steal(T& element)
		{
			ReadLockGuard lock(*rwLock);
			unsigned int num = *threadNum;
			for (int i = 0; i < num; ++i)
			{
				unsigned int now = (index + i) % num;
				TPBlockQueue<T>* queue = queues + now;
				if (queue->get_size() >= threshold && queue != ignore)
				{
					if (queue->dequeue(element))
					{
						index = now;
						return 1;
					}
				}
			}
			return 0;
		}
	};

	template <class T>
	class BulkStealer
	{
		template <class TYPE>
		friend class ThreadPool;

	private:

		unsigned int index;
		unsigned int batchSize;
		unsigned int queueLength;
		unsigned int* threadNum;
		unsigned int threshold;

		ReadWriteLock* rwLock;
		TPBlockQueue<T>* queues;
		TPBlockQueue<T>* ignore;

		BulkStealer(ReadWriteLock* rwLock, TPBlockQueue<T>* queues, TPBlockQueue<T>* ignore, unsigned int queueLength,
			unsigned int* threadNum, unsigned int batchSize)
		{
			this->index = 0;
			this->batchSize = batchSize;
			this->queueLength = queueLength;
			this->threadNum = threadNum;
			this->threshold = std::min(2 * batchSize, queueLength / 2);
			this->rwLock = rwLock;
			this->queues = queues;
			this->ignore = ignore;
		}

		unsigned int steal(T* elements)
		{
			ReadLockGuard lock(*rwLock);
			unsigned int num = *threadNum;
			for (int i = 0; i < num; ++i)
			{
				unsigned int now = (index + i) % num;
				TPBlockQueue<T>* queue = queues + now;
				if (queue->get_size() >= threshold && queue != ignore)
				{
					unsigned int count = queue->dequeue_bulk(elements, batchSize);
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

	/**
	 * @brief Thread pool implementation with multiple queues for task distribution
	 * @tparam T Type of task objects to be processed, must implement execute() method
	 */
	template <class T = TaskStack<>>
	class ThreadPool
	{
		static_assert(is_generic_ts<T>::value, "TYPE must be a TaskStack type");

	private:

		unsigned int threadNum;			  ///< Number of worker threads/queues
		unsigned int minThreadNum;
		unsigned int maxThreadNum;
		unsigned int batchSize;
		unsigned int queueLength;		  ///< Capacity of each internal queue
		std::chrono::milliseconds adjustInterval;

		T* containers;
		Semaphore exitSem;
		Semaphore* stopSem;
		Semaphore* startSem;
		ReadWriteLock rwLock;
		std::atomic<bool> exitFlag;
		std::atomic<bool> shutdownPolicy;			  ///< Thread pool shutdown policy: true for graceful shutdown	

		std::thread monitor;
		TPBlockQueue<T>* queues;		  ///< Per-worker task queues
		std::vector<std::thread> workers; ///< Worker thread collection
		std::atomic<unsigned int> index;  ///< Atomic counter for round-robin task distribution to worker queues

	public:

		/**
		 * @brief Constructs an uninitialized thread pool
		 */
		ThreadPool() : queues(nullptr) {}

		/**
		* @brief Initializes thread pool resources
		* @param capacity Capacity of each internal queue (>2)
		* @param minThreadNum Minimum number of worker threads (!=0)
		* @param maxThreadNum Maximum number of worker threads (>=minThreadNum)
		* @param batchSize Maximum tasks to process per batch (min 1)
		* @param adjustInterval Time interval for checking the load and adjusting the number of active threads
		* @return true if initialization succeeded, false otherwise
		*/
		bool init(unsigned int capacity, unsigned int minThreadNum,
			unsigned int maxThreadNum, unsigned int batchSize = 1,
			std::chrono::milliseconds adjustInterval = std::chrono::milliseconds(3000)) noexcept
		{
			assert(!queues);

			if (batchSize == 0 || minThreadNum == 0 || capacity< 2 || minThreadNum > maxThreadNum)
				return false;

			if (!initResourse(capacity, maxThreadNum, batchSize))
				return false;

			this->index = 0;
			this->exitFlag = false;
			this->shutdownPolicy = true;
			this->minThreadNum = minThreadNum;
			this->maxThreadNum = maxThreadNum;
			this->threadNum = maxThreadNum;
			this->batchSize = std::min(batchSize, capacity / 2);
			this->queueLength = capacity;
			this->adjustInterval = adjustInterval;
			workers.reserve(maxThreadNum);

			for (unsigned i = 0; i < maxThreadNum; ++i)
				workers.emplace_back(&ThreadPool::worker, this, i);

			if (maxThreadNum > 1)
				monitor = std::thread(&ThreadPool::load_monitor, this);

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
			assert(queues);

			if (maxThreadNum == 1)
				return queues->template emplace<POS>(std::forward<Args>(args)...);

			ReadLockGuard lock(rwLock);
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
			assert(queues);

			if (maxThreadNum == 1)
				return queues->template wait_emplace<POS>(std::forward<Args>(args)...);

			ReadLockGuard lock(rwLock);
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
			assert(queues);

			if (maxThreadNum == 1)
				return queues->template wait_emplace<POS>(timeout, std::forward<Args>(args)...);

			ReadLockGuard lock(rwLock);
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
			assert(queues);

			if (maxThreadNum == 1)
				return queues->template enqueue<POS>(std::forward<U>(task));

			ReadLockGuard lock(rwLock);
			return select_queue().template enqueue<POS>(std::forward<U>(task));
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
			assert(queues);

			if (maxThreadNum == 1)
				return queues->template wait_push<POS>(std::forward<U>(task));

			ReadLockGuard lock(rwLock);
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
			assert(queues);

			if (maxThreadNum == 1)
				return queues->template wait_push<POS>(std::forward<U>(task), timeout);

			ReadLockGuard lock(rwLock);
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
			assert(queues);

			if (maxThreadNum == 1)
				return queues->template enqueue_bulk<METHOD, POS>(tasks, count);

			ReadLockGuard lock(rwLock);
			return select_queue_for_bulk(std::max(1u, count / 2)).template enqueue_bulk<METHOD, POS>(tasks, count);
		}

		/**
		 * @brief Non-blocking bulk push for multiple tasks (dual-part version)
		 * @tparam METHOD Bulk construction method (COPY or MOVE, default COPY)
		 * @tparam POS Insertion position (HEAD or TAIL, default TAIL)
		 * @param part1 First array segment of tasks to enqueue
		 * @param count1 Number of tasks in first segment
		 * @param part2 Second array segment of tasks to enqueue
		 * @param count2 Number of tasks in second segment
		 * @return Actual number of tasks successfully enqueued (sum of both segments minus failures)
		 * @note Designed for ring buffers that benefit from batched two-part insertion.
		 */
		template <BULK_CMETHOD METHOD = COPY, INSERT_POS POS = TAIL>
		unsigned int enqueue_bulk(T* part1, unsigned int count1, T* part2, unsigned int count2) noexcept
		{
			assert(queues);

			if (maxThreadNum == 1)
				return queues->template enqueue_bulk<METHOD, POS>(part1, count1, part2, count2);

			ReadLockGuard lock(rwLock);
			return select_queue_for_bulk(std::max(1u, (count1 + count2) / 2)).template enqueue_bulk<METHOD, POS>(part1, count1, part2, count2);
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
			assert(queues);

			if (maxThreadNum == 1)
				return queues->template wait_pushBulk<METHOD, POS>(tasks, count);

			ReadLockGuard lock(rwLock);
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
			assert(queues);

			if (maxThreadNum == 1)
				return queues->template wait_pushBulk<METHOD, POS>(tasks, count, timeout);

			ReadLockGuard lock(rwLock);
			return select_queue().template wait_pushBulk<METHOD, POS>(tasks, count, timeout);
		}

		//Get the maximum occupied space of the thread pool.
		unsigned long long get_max_usage()
		{
			assert(queues);
			return  maxThreadNum * queues->get_bsize();
		}

		/**
		 * @brief Waits until all task queues are empty (all tasks have been taken from queues)
		 * @note This does NOT guarantee all tasks have completed execution - it only ensures:
		 *       1. All tasks have been dequeued by worker threads
		 *       2. May return while some tasks are still being processed by workers
		 * @details Continuously checks all active queues until they're empty.
		 *          Uses yield() between checks to avoid busy waiting.
		 */
		void join()
		{
			assert(queues);

			while (true)
			{
				bool flag = true;

				for (int i = 0; i < maxThreadNum; ++i)
				{
					if (queues[i].get_exact_size())
					{
						flag = false;
						break;
					}
				}

				if (flag)
					return;
				else
					std::this_thread::yield();
			}
		}

		/**
		 * @brief Waits until all task queues are empty (all tasks dequeued) or sleeps for specified intervals between checks.
		 * @note This does NOT guarantee all tasks have completed execution - it only ensures:
		 *       1. All tasks have been dequeued by worker threads
		 *       2. May return while some tasks are still being processed by workers
		 * @tparam Rep Arithmetic type representing tick count
		 * @tparam Period Type representing tick period
		 * @param interval Sleep duration between queue checks. Smaller values increase responsiveness
		 *                 but may use more CPU, larger values reduce CPU load but delay detection.
		 */
		template <class Rep, class Period>
		void join(const std::chrono::duration<Rep, Period>& interval)
		{
			assert(queues);

			while (true)
			{
				bool flag = true;
				for (int i = 0; i < maxThreadNum; ++i)
				{
					if (queues[i].get_exact_size())
					{
						flag = false;
						break;
					}
				}

				if (flag)
					return;
				else
					std::this_thread::sleep_for(interval);
			}
		}

		/**
		 * @brief Stops all workers and releases resources
		 * @param shutdownPolicy true for graceful shutdown (waiting for tasks to complete), false for immediate shutdown
		 */
		void exit(bool shutdownPolicy = true) noexcept
		{
			assert(queues);

			if (maxThreadNum > 1)
			{
				exitSem.release();
				monitor.join();
			}

			exitFlag = true;
			this->shutdownPolicy = shutdownPolicy;

			for (unsigned i = 0; i < workers.size(); ++i)
				startSem[i].release();

			for (unsigned i = 0; i < workers.size(); ++i)
				queues[i].stopWait();

			for (auto& worker : workers)
				worker.join();

			rleaseResourse();
		}

		~ThreadPool() noexcept
		{
			if (queues)
				exit(false);
		}

		ThreadPool(const ThreadPool&) = delete;
		ThreadPool& operator=(const ThreadPool&) = delete;
		ThreadPool(ThreadPool&&) = delete;
		ThreadPool& operator=(ThreadPool&&) = delete;

	private:

		unsigned int next_index() noexcept
		{
			return index.fetch_add(1, std::memory_order_relaxed) % threadNum;
		}

		TPBlockQueue<T>& select_queue() noexcept
		{
			unsigned int index = next_index();

			if (queues[index].get_exact_size() < queueLength)
				return queues[index];

			unsigned int half = threadNum / 2;
			return queues[(index + half) % threadNum];
		}

		TPBlockQueue<T>& select_queue_for_bulk(unsigned required) noexcept
		{
			unsigned int index = next_index();

			if (queues[index].get_exact_size() + required <= queueLength)
				return queues[index];

			unsigned int half = threadNum / 2;
			return queues[(index + half) % threadNum];
		}

		void load_monitor() noexcept
		{
			while (true)
			{
				if (exitSem.try_acquire_for(adjustInterval))
					return;

				unsigned int allSize = queueLength * threadNum;
				unsigned int totalSize = 0;

				for (int i = 0; i < threadNum; ++i)
					totalSize += queues[i].get_exact_size();

				if (totalSize < allSize * HSLL_THREADPOOL_SHRINK_FACTOR && threadNum > minThreadNum)
				{
					rwLock.lock_write();
					threadNum--;
					rwLock.unlock_write();
					queues[threadNum].stopWait();
					stopSem[threadNum].acquire();
					queues[threadNum].release();
				}
				else if (totalSize > allSize * HSLL_THREADPOOL_EXPAND_FACTOR && threadNum < maxThreadNum)
				{
					unsigned int newThreads = std::max(1u, (maxThreadNum - threadNum) / 2);
					unsigned int succeed = 0;
					for (int i = threadNum; i < threadNum + newThreads; ++i)
					{
						if (!queues[i].init(queueLength))
							break;

						startSem[i].release();
						succeed++;
					}

					if (succeed > 0)
					{
						rwLock.lock_write();
						threadNum += succeed;
						rwLock.unlock_write();
					}
				}
			}
		}

		void worker(unsigned int index) noexcept
		{
			if (batchSize == 1)
			{
				if (maxThreadNum == 1)
					process_single1(queues + index, index);
				else
					process_single2(queues + index, index);
			}
			else
			{
				if (maxThreadNum == 1)
					process_bulk1(queues + index, index);
				else
					process_bulk2(queues + index, index);
			}
		}

		static inline void execute_tasks(T* tasks, unsigned int count)
		{
			for (unsigned int i = 0; i < count; ++i)
			{
				tasks[i].execute();
				tasks[i].~T();
			}
		}

		void process_single1(TPBlockQueue<T>* queue, unsigned int index) noexcept
		{
			T* task = containers + index * batchSize;

			while (true)
			{
				while (true)
				{
					while (queue->dequeue(*task))
					{
						task->execute();
						task->~T();
					}

					if (queue->wait_dequeue(*task, std::chrono::milliseconds(HSLL_THREADPOOL_TIMEOUT_MILLISECONDS)))
					{
						task->execute();
						task->~T();
					}

					if (queue->is_Stopped())
						break;
				}

				bool  shutdownPolicy = this->shutdownPolicy.load();

				while (shutdownPolicy && queue->dequeue(*task))
				{
					task->execute();
					task->~T();
				}

				stopSem[index].release();
				startSem[index].acquire();

				if (exitFlag)
					break;
			}
		}

		void process_single2(TPBlockQueue<T>* queue, unsigned int index) noexcept
		{
			T* task = containers + index * batchSize;
			SingleStealer<T> stealer(&rwLock, queues, queue, queueLength, &threadNum);

			while (true)
			{
				while (true)
				{
					while (queue->dequeue(*task))
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
						if (queue->wait_dequeue(*task, std::chrono::milliseconds(HSLL_THREADPOOL_TIMEOUT_MILLISECONDS)))
						{
							task->execute();
							task->~T();
						}
					}

					if (queue->is_Stopped())
						break;
				}

				bool  shutdownPolicy = this->shutdownPolicy.load();

				while (shutdownPolicy && queue->dequeue(*task))
				{
					task->execute();
					task->~T();
				}

				stopSem[index].release();
				startSem[index].acquire();

				if (exitFlag)
					break;
			}
		}

		void process_bulk1(TPBlockQueue<T>* queue, unsigned int index) noexcept
		{
			T* tasks = containers + index * batchSize;
			unsigned int size_threshold = batchSize;
			unsigned int round_threshold = batchSize / 2;

			while (true)
			{
				unsigned int count;

				while (true)
				{
					while (true)
					{
						unsigned int round = 1;
						unsigned int size = queue->get_exact_size();

						while (size < size_threshold && round < round_threshold)
						{
							std::this_thread::yield();
							size = queue->get_exact_size();
							round++;
							std::this_thread::yield();
						}

						if (size && (count = queue->dequeue_bulk(tasks, size_threshold)))
							execute_tasks(tasks, count);
						else
							break;
					}

					count = queue->wait_dequeue_bulk(tasks, size_threshold, std::chrono::milliseconds(HSLL_THREADPOOL_TIMEOUT_MILLISECONDS));

					if (count)
						execute_tasks(tasks, count);

					if (queue->is_Stopped())
						break;
				}

				bool  shutdownPolicy = this->shutdownPolicy.load();

				while (shutdownPolicy && (count = queue->dequeue_bulk(tasks, size_threshold)))
					execute_tasks(tasks, count);

				stopSem[index].release();
				startSem[index].acquire();

				if (exitFlag)
					break;
			}
		}

		void process_bulk2(TPBlockQueue<T>* queue, unsigned int index) noexcept
		{
			T* tasks = containers + index * batchSize;
			unsigned int size_threshold = batchSize;
			unsigned int round_threshold = batchSize / 2;
			BulkStealer<T> stealer(&rwLock, queues, queue, queueLength, &threadNum, batchSize);

			while (true)
			{
				unsigned int count;

				while (true)
				{
					while (true)
					{
						unsigned int round = 1;
						unsigned int size = queue->get_exact_size();

						while (size < size_threshold && round < round_threshold)
						{
							std::this_thread::yield();
							size = queue->get_exact_size();
							round++;
							std::this_thread::yield();
						}

						if (size && (count = queue->dequeue_bulk(tasks, size_threshold)))
							execute_tasks(tasks, count);
						else
							break;
					}

					count = stealer.steal(tasks);
					if (count)
					{
						execute_tasks(tasks, count);
					}
					else
					{
						count = queue->wait_dequeue_bulk(tasks, size_threshold, std::chrono::milliseconds(HSLL_THREADPOOL_TIMEOUT_MILLISECONDS));

						if (count)
							execute_tasks(tasks, count);
					}

					if (queue->is_Stopped())
						break;
				}

				bool  shutdownPolicy = this->shutdownPolicy.load();

				while (shutdownPolicy && (count = queue->dequeue_bulk(tasks, size_threshold)))
					execute_tasks(tasks, count);

				stopSem[index].release();
				startSem[index].acquire();

				if (exitFlag)
					break;
			}
		}

		bool initResourse(unsigned int capacity, unsigned int maxThreadNum, unsigned int batchSize)
		{
			unsigned int succeed = 0;

			if (!(startSem = new(std::nothrow) Semaphore[2 * maxThreadNum]))
				goto clean_1;

			stopSem = startSem + maxThreadNum;

			if (!(containers = (T*)ALIGNED_MALLOC(sizeof(T) * batchSize * maxThreadNum, alignof(T))))
				goto clean_2;

			if (!(queues = (TPBlockQueue<T>*)ALIGNED_MALLOC(maxThreadNum * sizeof(TPBlockQueue<T>), 64)))
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

			ALIGNED_FREE(queues);
			queues = nullptr;

		clean_2:

			ALIGNED_FREE(containers);

		clean_1:

			delete[] startSem;

			return false;
		}

		void rleaseResourse()
		{
			for (unsigned i = 0; i < maxThreadNum; ++i)
				queues[i].~TPBlockQueue<T>();

			ALIGNED_FREE(queues);
			ALIGNED_FREE(containers);
			delete[] startSem;
			queues = nullptr;
			workers.clear();
			workers.shrink_to_fit();
		}
	};

	template <class T, unsigned int BATCH, INSERT_POS POS = TAIL>
	class BatchSubmitter
	{
		static_assert(is_generic_ts<T>::value, "TYPE must be a TaskStack type");
		static_assert(BATCH > 0, "BATCH > 0");
		alignas(alignof(T)) unsigned char buf[BATCH * sizeof(T)];

		T* elements;
		unsigned int size;
		unsigned int index;
		ThreadPool<T>* pool;

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
		BatchSubmitter(ThreadPool<T>* pool) : size(0), index(0), elements((T*)buf), pool(pool) {
			assert(pool);
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
		 * @tparam Args Types of arguments for task constructor
		 * @param args Arguments forwarded to task constructor
		 * @return true if task was added to buffer or submitted successfully,
		 *         false if submission failed due to full thread pool queues
		 * @details Automatically submits batch if buffer becomes full during emplace
		 */
		template <typename... Args>
		bool emplace(Args &&...args) noexcept
		{
			if (!check_and_submit())
				return false;

			new (elements + index) T(std::forward<Args>(args)...);
			index = (index + 1) % BATCH;
			size++;
			return true;
		}

		/**
		 * @brief Adds preconstructed task to batch buffer
		 * @tparam U Deduced task type
		 * @param task Task object to buffer
		 * @return true if task was added to buffer or submitted successfully,
		 *         false if submission failed due to full thread pool queues
		 * @details Automatically submits batch if buffer becomes full during add
		 */
		template <class U>
		bool add(U&& task) noexcept
		{
			if (!check_and_submit())
				return false;

			new (elements + index) T(std::forward<U>(task));
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
					submitted = pool->template enqueue<POS>(std::move(*(elements + start))) ? 1 : 0;
				else
					submitted = pool->template enqueue_bulk<MOVE, POS>(elements + start, len1);
			}
			else
			{
				submitted = pool->template enqueue_bulk<MOVE, POS>(
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

#endif