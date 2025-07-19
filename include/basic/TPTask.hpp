#ifndef HSLL_TPTASK
#define HSLL_TPTASK

#include<future>
#include<assert.h>

#define HSLL_ALLOW_THROW

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

#endif // !HSLL_TPTASK