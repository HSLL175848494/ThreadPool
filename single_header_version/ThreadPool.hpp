#ifndef HSLL_THREADPOOL
#define HSLL_THREADPOOL

#include <set>
#include <map>
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

	template <class... Args>
	struct TaskImpl;

	template <class F, class... Args>
	class HeapCallable;

	template <class F, class... Args>
	class HeapCallable_Async;

	template <class F, class... Args>
	class HeapCallable_Cancelable;

	//helper1_sfinae
	template <typename T>
	struct is_TaskImpl : std::false_type {};

	template <class... Args>
	struct is_TaskImpl<TaskImpl<Args...>> : std::true_type {};

	template <typename T>
	struct is_TaskStack : std::false_type {};

	template <unsigned int S, unsigned int A>
	struct is_TaskStack<TaskStack<S, A>> : std::true_type {};

	template <typename T>
	struct is_HeapCallable : std::false_type {};

	template <class F, class... Args>
	struct is_HeapCallable<HeapCallable<F, Args...>> : std::true_type {};

	template <typename T>
	struct is_HeapCallable_Async : std::false_type {};

	template <class F, class... Args>
	struct is_HeapCallable_Async<HeapCallable_Async<F, Args...>> : std::true_type {};

	template <typename T>
	struct is_HeapCallable_Cancelable : std::false_type {};

	template <class F, class... Args>
	struct is_HeapCallable_Cancelable<HeapCallable_Cancelable<F, Args...>> : std::true_type {};

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

	//helper4_all_true
	template <bool...>
	struct bool_pack;

	template <bool... Bs>
	using all_true = std::is_same<bool_pack<true, Bs...>, bool_pack<Bs..., true>>;

	template <typename... Ts>
	using are_all_copy_constructible = all_true<is_copy_constructible<Ts>::value...>;

	//helper5_invoke
	enum TASK_TUPLE_TYPE
	{
		TASK_TUPLE_TYPE_NORMAL,
		TASK_TUPLE_TYPE_ASYNC,
		TASK_TUPLE_TYPE_CANCELABLE
	};

	template<TASK_TUPLE_TYPE TYPE>
	struct Invoker {};

	template<>
	struct Invoker<TASK_TUPLE_TYPE_NORMAL>
	{
		template <class ResultType, typename Callable, typename... Ts>
		static void invoke(Callable& callable, Ts &...args)
		{
			callable(args...);
		}
	};

	template<>
	struct Invoker<TASK_TUPLE_TYPE_ASYNC>
	{
		template <class ResultType, typename std::enable_if<std::is_void<ResultType>::value, bool>::type = true,
			typename Promise, typename Callable, typename... Ts>
		static void invoke(Promise& promise, Callable& callable, Ts &...args)
		{
			callable(args...);
		}

		template <class ResultType, typename std::enable_if<!std::is_void<ResultType>::value, bool>::type = true,
			typename Promise, typename Callable, typename... Ts>
		static ResultType invoke(Promise& promise, Callable& callable, Ts &...args)
		{
			return callable(args...);
		}
	};

	template<>
	struct Invoker<TASK_TUPLE_TYPE_CANCELABLE>
	{
		template <class ResultType, typename std::enable_if<std::is_void<ResultType>::value, bool>::type = true,
			typename Promise, typename Flag, typename Callable, typename... Ts>
		static void invoke(Promise& promise, Flag& flag, Callable& callable, Ts &...args)
		{
			callable(args...);
		}

		template <class ResultType, typename std::enable_if<!std::is_void<ResultType>::value, bool>::type = true,
			typename Promise, typename Flag, typename Callable, typename... Ts>
		static ResultType invoke(Promise& promise, Flag& flag, Callable& callable, Ts &...args)
		{
			return callable(args...);
		}
	};

	//helper6_apply
	template <TASK_TUPLE_TYPE TYPE, class ResultType, typename std::enable_if<std::is_void<ResultType>::value, bool>::type = true,
		typename Tuple, size_t... Is>
	void apply_impl(Tuple& tup, index_sequence<Is...>)
	{
		Invoker<TYPE>::template invoke<ResultType>(std::get<Is>(tup)...);
	}

	template <TASK_TUPLE_TYPE TYPE, class ResultType, typename std::enable_if<!std::is_void<ResultType>::value, bool>::type = true,
		typename Tuple, size_t... Is>
	ResultType apply_impl(Tuple& tup, index_sequence<Is...>)
	{
		return Invoker<TYPE>::template invoke<ResultType>(std::get<Is>(tup)...);
	}

	template <TASK_TUPLE_TYPE TYPE = TASK_TUPLE_TYPE_NORMAL, class ResultType = void,
		typename std::enable_if<std::is_void<ResultType>::value, bool>::type = true, typename Tuple>
	void tuple_apply(Tuple& tup)
	{
		apply_impl<TYPE, ResultType>(tup, typename make_index_sequence<std::tuple_size<Tuple>::value>::type{});
	}

	template <TASK_TUPLE_TYPE TYPE = TASK_TUPLE_TYPE_NORMAL, class ResultType = void,
		typename std::enable_if<!std::is_void<ResultType>::value, bool>::type = true, typename Tuple>
	ResultType tuple_apply(Tuple& tup)
	{
		return apply_impl<TYPE, ResultType>(tup, typename make_index_sequence<std::tuple_size<Tuple>::value>::type{});
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

	//helper8_result_infer
	template <class F, class... Args>
	struct result_type_infer
	{
		using type = decltype(
			std::declval<typename std::decay<F>::type>()
			(std::declval<typename std::decay<Args>::type&>()...));
	};

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
		template<class Func, typename std::enable_if<!is_HeapCallable<typename std::decay<Func>::type>::value, int>::type = 0 >
		HeapCallable(Func&& func, Args &&...args) HSLL_ALLOW_THROW
			: storage(HSLL::make_unique<Package>(std::forward<Func>(func), std::forward<Args>(args)...)) {}

		/**
		 * @brief Invokes the stored callable with bound arguments
		 * @pre Object must be in a valid state
		 */
		void operator()()
		{
			tuple_apply(*storage);
		}
	};

	/**
	 * @class HeapCallable_Async
	 * @brief Asynchronous version of HeapCallable that provides a future for the result.
	 *        This class is move-only and non-assignable
	 * @tparam F Type of the callable object
	 * @tparam Args Types of the arguments bound to the callable
	 */
	template <class F, class... Args>
	class HeapCallable_Async
	{
		using ResultType = typename result_type_infer<F, Args...>::type;
		using Package = std::tuple<std::promise<ResultType>, typename std::decay<F>::type, typename std::decay<Args>::type...>;

	protected:
		std::unique_ptr<Package> storage;

		template<class T = ResultType, typename std::enable_if<std::is_void<T>::value, bool>::type = true>
		void invoke()
		{
			auto& promise = std::get<0>(*storage);
			try
			{
				tuple_apply<TASK_TUPLE_TYPE_ASYNC, ResultType>(*storage);
				promise.set_value();
			}
			catch (...)
			{
				promise.set_exception(std::current_exception());
			}
		}

		template<class T = ResultType, typename std::enable_if<!std::is_void<T>::value, bool>::type = true>
		void invoke()
		{
			auto& promise = std::get<0>(*storage);
			try
			{
				promise.set_value(tuple_apply<TASK_TUPLE_TYPE_ASYNC, ResultType>(*storage));
			}
			catch (...)
			{
				promise.set_exception(std::current_exception());
			}
		}

	public:
		/**
		 * @brief Constructs an async callable object
		 * @param func Callable object to store
		 * @param args Arguments to bind to the callable
		 */
		template<class Func, typename std::enable_if<!is_HeapCallable_Async<typename std::decay<Func>::type>::value, int>::type = 0 >
		HeapCallable_Async(Func&& func, Args &&...args) HSLL_ALLOW_THROW
			: storage(HSLL::make_unique<Package>(std::promise<ResultType>(), std::forward<Func>(func), std::forward<Args>(args)...)) {}

		/**
		 * @brief Executes the callable and sets promise value/exception
		 * @pre Object must be in a valid state
		 */
		void operator()()
		{
			invoke();
		}

		/**
		 * @brief Retrieves the future associated with the promise
		 * @return std::future<ResultType> Future object for the call result
		 * @pre Object must be in a valid state
		 */
		std::future<ResultType> get_future()
		{
			return std::get<0>(*storage).get_future();
		}
	};

	class CancelableFlag
	{
		std::atomic<bool> flag;

	public:

		CancelableFlag(bool ignore = false) : flag(false) {};

		bool cancel()
		{
			bool expected = false;

			if (flag.compare_exchange_strong(expected, true))
				return true;

			return false;
		}

		/**
		 * @brief Enters a critical section making the task non-cancelable
		 * @return true Successfully entered critical section
		 * @return false Entry failed (task was already canceled, or critical section was already entered successfully)
		 */
		bool enter()
		{
			bool expected = false;

			if (flag.compare_exchange_strong(expected, true))
				return true;

			return false;
		}

		CancelableFlag& operator=(CancelableFlag&& other)
		{
			if (this != &other)
				flag = other.flag.load();

			return *this;
		}

		CancelableFlag(CancelableFlag&& other) :flag(other.flag.load()) {}
	};

	/**
	 * @class Cancelable
	 * @brief Manages cancellation state and result propagation for asynchronous tasks.
	 * @tparam ResultType Return type of the associated asynchronous task
	 */
	template<class ResultType>
	class Cancelable
	{
	private:

		CancelableFlag flag;
		std::promise<ResultType> promise;
		std::future<ResultType> future;

	public:

		Cancelable() :future(promise.get_future()) {}

		/**
		 * @brief Requests cancellation of the associated task
		 * @return true if cancellation succeeded (state was active), false if already canceled/completed
		 * @note On success:
		 * - Sets promise exception with "Task canceled" error
		 * - Transitions state to canceled
		 */
		bool cancel()
		{
			bool result;

			if (result = flag.cancel())
				promise.set_exception(std::make_exception_ptr(std::runtime_error("Task canceled")));

			return result;
		}

		/**
		 * @brief Retrieves task result (blocking)
		 * @return Result value for non-void specializations
		 * @throws Propagates any exception stored in the promise
		 * @throws std::runtime_error("Task canceled") if canceled
		 */
		template <typename U = ResultType>
		typename std::enable_if<!std::is_void<U>::value, U>::type get()
		{
			return future.get();
		}

		/**
		 * @brief Synchronizes with task completion (void specialization)
		 * @throws Propagates any exception stored in the promise
		 * @throws std::runtime_error("Task canceled") if canceled
		 */
		template <typename U = ResultType>
		typename std::enable_if<std::is_void<U>::value>::type get()
		{
			future.get();
		}

		/**
		 * @brief Blocks until result becomes available
		 */
		void wait() const
		{
			future.wait();
		}

		/**
		 * @brief Blocks with timeout duration
		 * @return Status of future after waiting
		 */
		template <class Rep, class Period>
		std::future_status wait_for(const std::chrono::duration<Rep, Period>& timeout_duration) const
		{
			return future.wait_for(timeout_duration);
		}

		/**
		 * @brief Blocks until specified time point
		 * @return Status of future after waiting
		 */
		template <class Clock, class Duration>
		std::future_status wait_until(const std::chrono::time_point<Clock, Duration>& timeout_time) const
		{
			return future.wait_until(timeout_time);
		}

		/**
		 * @brief Enters a critical section making the task non-cancelable
		 * @return true Successfully entered critical section
		 * @return false Entry failed (task was already canceled, or critical section was already entered successfully)
		 */
		bool enter()
		{
			return flag.enter();
		}

		// Result setters (critical section only) -----------------------------------

		/**
		 * @brief Sets void result value
		 * @pre Must be in critical section (via successful enter())
		 * @throws std::future_error if result already set or not in critical section
		 */
		template <class T = ResultType, typename std::enable_if<std::is_void<T>::value, bool>::type = true>
		void set_value()
		{
			promise.set_value();
		}

		/**
		 * @brief Sets non-void result value
		 * @param value Result to store in promise
		 * @pre Must be in critical section (via successful enter())
		 * @throws std::future_error if result already set or not in critical section
		 */
		template <class U, class T = ResultType, typename std::enable_if<!std::is_void<T>::value, bool>::type = true>
		void set_value(U&& value)
		{
			promise.set_value(std::forward<T>(value));
		}

		/**
		 * @brief Stores exception in promise
		 * @param e Exception pointer to store
		 * @pre Must be in critical section (via successful enter())
		 * @throws std::future_error if exception already set or not in critical section
		 */
		void set_exception(std::exception_ptr e)
		{
			promise.set_exception(e);
		}
	};

	/**
	 * @class HeapCallable_Cancelable
	 * @brief Cancelable version of HeapCallable with atomic cancellation flag.
	 *        This class is move-only and non-assignable
	 * @tparam F Type of the callable object
	 * @tparam Args Types of the arguments bound to the callable
	 */
	template <class F, class... Args>
	class HeapCallable_Cancelable
	{
		using ResultType = typename result_type_infer<F, Args...>::type;
		using Package = std::tuple<std::promise<ResultType>, CancelableFlag,
			typename std::decay<F>::type, typename std::decay<Args>::type...>;

	private:
		std::shared_ptr<Package> storage;

		template<class T = ResultType, typename std::enable_if<std::is_void<T>::value, bool>::type = true>
		void invoke()
		{
			auto& promise = std::get<0>(*storage);
			try
			{
				tuple_apply<TASK_TUPLE_TYPE_CANCELABLE, ResultType>(*storage);
				promise.set_value();
			}
			catch (...)
			{
				promise.set_exception(std::current_exception());
			}
		}

		template<class T = ResultType, typename std::enable_if<!std::is_void<T>::value, bool>::type = true>
		void invoke()
		{
			auto& promise = std::get<0>(*storage);
			try
			{
				promise.set_value(tuple_apply<TASK_TUPLE_TYPE_CANCELABLE, ResultType>(*storage));
			}
			catch (...)
			{
				promise.set_exception(std::current_exception());
			}
		}

	public:

		struct Controller
		{
		private:

			std::future<ResultType> future;
			std::shared_ptr<Package> storage;

		public:

			Controller(std::shared_ptr<Package> storage)
				:storage(storage), future(std::get<0>(*storage).get_future()) {};

			/**
			 * @brief Requests cancellation of the associated task
			 * @return true if cancellation succeeded (state was active), false if already canceled/completed
			 * @note On success:
			 * - Sets promise exception with "Task canceled" error
			 * - Transitions state to canceled
			 */
			bool cancel()
			{
				bool result;

				if (result = std::get<1>(*storage).cancel())
					std::get<0>(*storage).set_exception(std::make_exception_ptr(std::runtime_error("Task canceled")));

				return result;
			}

			/**
			 * @brief Retrieves task result (blocking)
			 * @return Result value for non-void specializations
			 * @throws Propagates any exception stored in the promise
			 * @throws std::runtime_error("Task canceled") if canceled
			 */
			template <typename U = ResultType>
			typename std::enable_if<!std::is_void<U>::value, U>::type get()
			{
				return future.get();
			}

			/**
			 * @brief Synchronizes with task completion (void specialization)
			 * @throws Propagates any exception stored in the promise
			 * @throws std::runtime_error("Task canceled") if canceled
			 */
			template <typename U = ResultType>
			typename std::enable_if<std::is_void<U>::value>::type get()
			{
				future.get();
			}

			/**
			 * @brief Blocks until result becomes available
			 */
			void wait() const
			{
				future.wait();
			}

			/**
			 * @brief Blocks with timeout duration
			 * @return Status of future after waiting
			 */
			template <class Rep, class Period>
			std::future_status wait_for(const std::chrono::duration<Rep, Period>& timeout_duration) const
			{
				return future.wait_for(timeout_duration);
			}
		};

		/**
		 * @brief Constructs a cancelable callable object
		 * @param func Callable object to store
		 * @param args Arguments to bind to the callable
		 */
		template<class Func, typename std::enable_if<!is_HeapCallable_Cancelable<typename std::decay<Func>::type>::value, int>::type = 0 >
		HeapCallable_Cancelable(Func&& func, Args &&...args) HSLL_ALLOW_THROW
			: storage(std::make_shared<Package>(std::promise<ResultType>(), false, std::forward<Func>(func), std::forward<Args>(args)...)) {}

		/**
		 * @brief Executes the callable if not canceled
		 * @pre Object must be in a valid state
		 */
		void operator()()
		{
			auto& flag = std::get<1>(*storage);

			if (flag.enter())
				invoke();
		}

		Controller get_controller()
		{
			assert(storage);
			return Controller(storage);
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
	 * @tparam F Type of callable object
	 * @tparam Args Types of arguments to bind
	 * @param func Callable target function
	 * @param args Arguments to bind to function call
	 * @return HeapCallable_Async instance
	 */
	template <typename F, typename... Args>
	HeapCallable_Async<F, Args...> make_callable_async(F&& func, Args &&...args) HSLL_ALLOW_THROW
	{
		return HeapCallable_Async<F, Args...>(std::forward<F>(func), std::forward<Args>(args)...);
	}

	/**
	 * @brief Factory function to create HeapCallable_Cancelable objects
	 * @tparam F Type of callable object
	 * @tparam Args Types of arguments to bind
	 * @param func Callable target function
	 * @param args Arguments to bind to function call
	 * @return HeapCallable_Cancelable instance
	 */
	template <typename F, typename... Args>
	HeapCallable_Cancelable<F, Args...> make_callable_cancelable(F&& func, Args &&...args) HSLL_ALLOW_THROW
	{
		return HeapCallable_Cancelable<F, Args...>
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
	template <class... Args>
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

		using Tuple = std::tuple<typename std::decay<Args>::type...>;
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
			typename std::enable_if<!is_TaskImpl<typename std::decay<Func>::type>::value, int>::type = 0>
		TaskImpl(Func&& func, Params &&...args)
			: storage(std::forward<Func>(func), std::forward<Params>(args)...) {}


		template <bool Condition>
		typename std::enable_if<!Condition, void>::type invoke()
		{
			tuple_apply(storage);
		}

		template <bool Condition>
		typename std::enable_if<Condition, void>::type invoke()
		{
			std::get<0>(storage)();
		}

		void execute() noexcept override
		{
			invoke<sizeof...(Args) == 1>();
		}

		void copyTo(void* dst) const noexcept override
		{
			CopyHelper<TaskImpl,
				are_all_copy_constructible<typename std::decay<Args>::type...>::value>::copyTo(this, dst);
		}

		void moveTo(void* dst) noexcept override
		{
			tuple_move(dst);
		}

		bool is_copyable() const noexcept override
		{
			return are_all_copy_constructible<typename std::decay<Args>::type...>::value;
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

	/**
	 * @brief Stack-allocated task container with fixed-size storage
	 * @tparam TSIZE Size of internal storage buffer (default = 64)
	 * @tparam ALIGN Alignment requirement for storage (default = 8)
	 */
	template <unsigned int TSIZE = 64, unsigned int ALIGN = 8>
	class TaskStack
	{
		static_assert(TSIZE >= 24, "TSIZE must be at least 24: sizeof(HeapCallable_Cancelable<F, Args...>) in x64 platform");
		static_assert(ALIGN >= alignof(void*), "Alignment must >= alignof(void*)");
		static_assert(TSIZE% ALIGN == 0, "TSIZE must be a multiple of ALIGN");
		alignas(ALIGN) char storage[TSIZE];


		template <bool Condition, typename Any, typename... Params>
		typename std::enable_if<Condition, void>::type
			construct(Any&& func, Params &&...params)//normal
		{
			using ImplType = typename task_stack<Any, Params...>::type;
			new (storage) ImplType(std::forward<Any>(func), std::forward<Params>(params)...);
		}

		template <bool Condition, typename Any, typename... Params>
		typename std::enable_if<!Condition, void>::type
			construct(Any&& func, Params &&...params)//HeapCallable
		{
			using ImplType = typename task_stack<HeapCallable<Any, Params...>>::type;
			new (storage) ImplType(HeapCallable<Any, Params...>(std::forward<Any>(func), std::forward<Params>(params)...));
		}

	public:

		/**
		 * @brief Compile-time type trait to check if a task is stored on the stack
		 * This type trait determines whether a task constructed with the given callable and arguments
		 * will be stored within the internal stack buffer (fixed-size storage) of the TaskStack container.
		 * The static boolean member `value` is:
		 *   - `true`: Task fits in internal buffer
		 *   - `false`: Task requires heap storage
		 * @tparam F Type of the callable object
		 * @tparam Args... Types of the bound arguments
		 */
		template <class F, class... Args>
		struct is_stored_on_stack
		{
			typedef typename task_stack<F, Args...>::type ImplType;
			static constexpr bool value = (sizeof(ImplType) <= TSIZE && alignof(ImplType) <= ALIGN);
		};

		/**
		 * @brief Constructs task in internal storage
		 * Constructs a task by storing the callable and bound arguments in the internal buffer if possible.
		 *
		 * The storage location is determined by:
		 *   - If the task's total size <= TSIZE AND alignment <= ALIGN:
		 *        Directly constructs the task in the internal stack buffer
		 *   - Else:
		 *        Allocates the task on the heap and stores a pointer in the internal buffer
		 *        (using HeapCallable wrapper)

		 * For task properties after construction:
		 *   - Copyability can be checked with is_copyable()
		 *        (depends on whether the underlying callable is copyable)
		 *   - Movability is always supported (is_moveable() always returns true)
		 *
		 * @tparam F Type of callable object
		 * @tparam Args Types of bound arguments
		 * @param func Callable target function
		 * @param args Arguments to bind to function call
		 */
		template <class F, class... Args,
			typename std::enable_if<!is_TaskStack<typename std::decay<F>::type>::value, int>::type = 0>
		TaskStack(F&& func, Args &&...args) HSLL_ALLOW_THROW
		{
			using ImplType = typename task_stack<F, Args...>::type;
			constexpr bool can_store = sizeof(ImplType) <= TSIZE && alignof(ImplType) <= ALIGN;
			construct<can_store>(std::forward<F>(func), std::forward<Args>(args)...);
		}

		/**
		 * @brief Executes the stored task
		 */
		void execute() noexcept
		{
			getBase()->execute();
		}

		/*
		 * @brief Checks if the stored task is copyable
		*/
		bool is_copyable() const noexcept
		{
			return getBase()->is_copyable();
		}

		/*
		 * @brief Checks if the stored task is moveable
		 * @return Always returns true
		*/
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

namespace HSLL
{
	constexpr long long HSLL_SPINREADWRITELOCK_MAXREADER = (1LL << 62);

	/**
	 * @brief Efficient spin lock based on atomic variables, suitable for scenarios where reads significantly outnumber writes
	 */
	class SpinReadWriteLock
	{
	private:
		std::atomic<long long> count;

	public:

		SpinReadWriteLock() :count(0) {}


		void lock_read()
		{
			long long old = count.fetch_add(1, std::memory_order_acquire);

			while (old < 0)
			{
				count.fetch_sub(1, std::memory_order_relaxed);

				while (count.load(std::memory_order_relaxed) < 0)
					std::this_thread::yield();

				old = count.fetch_add(1, std::memory_order_acquire);
			}
		}

		void unlock_read()
		{
			count.fetch_sub(1, std::memory_order_relaxed);
		}

		void lock_write()
		{
			long long old = count.load(std::memory_order_relaxed);

			while (true)
			{
				if (old < 0)
				{
					std::this_thread::yield();
					old = count.load(std::memory_order_relaxed);
				}
				else if (count.compare_exchange_weak(old, old - HSLL_SPINREADWRITELOCK_MAXREADER, std::memory_order_acquire, std::memory_order_relaxed))
				{
					break;
				}
			}

			while (count.load(std::memory_order_relaxed) != -HSLL_SPINREADWRITELOCK_MAXREADER);

			std::atomic_thread_fence(std::memory_order_acquire);
		}

		void unlock_write()
		{
			count.fetch_add(HSLL_SPINREADWRITELOCK_MAXREADER, std::memory_order_release);
		}

		SpinReadWriteLock(const SpinReadWriteLock&) = delete;
		SpinReadWriteLock& operator=(const SpinReadWriteLock&) = delete;
	};

	class ReadLockGuard
	{
	private:

		SpinReadWriteLock& lock;

	public:

		explicit ReadLockGuard(SpinReadWriteLock& lock) : lock(lock)
		{
			lock.lock_read();
		}

		~ReadLockGuard()
		{
			lock.unlock_read();
		}

		ReadLockGuard(const ReadLockGuard&) = delete;
		ReadLockGuard& operator=(const ReadLockGuard&) = delete;
	};

	class WriteLockGuard
	{
	private:

		SpinReadWriteLock& lock;

	public:

		explicit WriteLockGuard(SpinReadWriteLock& lock) : lock(lock)
		{
			lock.lock_write();
		}

		~WriteLockGuard()
		{
			lock.unlock_write();
		}

		WriteLockGuard(const WriteLockGuard&) = delete;
		WriteLockGuard& operator=(const WriteLockGuard&) = delete;
	};
}


#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>

namespace HSLL
{
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

namespace HSLL
{
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
#error "Unsupported platform: no Semaphore implementation available"
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

namespace HSLL
{
	template<class T>
	class RoundRobinGroup
	{
		unsigned int currentIndex;
		unsigned int taskCount;
		unsigned int capacityThreshold;
		unsigned int taskThreshold;
		std::vector<TPBlockQueue<T>*>* assignedQueues;

		void advance_index()
		{
			if (taskCount >= taskThreshold)
			{
				currentIndex = (currentIndex + 1) % assignedQueues->size();
				taskCount = 0;
			}
		}

	public:

		void resetAndInit(std::vector<TPBlockQueue<T>*>* queues, unsigned int capacity, unsigned int threshold)
		{
			currentIndex = 0;
			taskCount = 0;
			this->assignedQueues = queues;
			this->capacityThreshold = capacity * 0.995;
			this->taskThreshold = threshold;
		}

		TPBlockQueue<T>* current_queue()
		{
			return (*assignedQueues)[currentIndex];
		}

		TPBlockQueue<T>* available_queue()
		{
			TPBlockQueue<T>* candidateQueue;

			for (int i = 0; i < assignedQueues->size() - 1; ++i)
			{
				currentIndex = (currentIndex + 1) % assignedQueues->size();
				candidateQueue = (*assignedQueues)[currentIndex];

				if (candidateQueue->get_size() <= capacityThreshold)
				{
					taskCount = 0;
					return candidateQueue;
				}
			}

			return nullptr;
		}

		void record(unsigned int taskSize)
		{
			if (taskSize)
			{
				taskCount += taskSize;
				advance_index();
			}
			else
			{
				currentIndex = (currentIndex + 1) % assignedQueues->size();
				taskCount = 0;
				return;
			}
		}
	};

	template<class T>
	class TPGroupAllocator
	{
		unsigned int totalQueues;
		unsigned int taskThreshold;
		unsigned int queueCapacity;
		TPBlockQueue<T>* queueArray;
		std::vector<std::vector<TPBlockQueue<T>*>> threadSlots;
		std::map<std::thread::id, RoundRobinGroup<T>> threadGroups;

		void manage_thread_entry(bool addThread, std::thread::id threadId)
		{
			if (addThread)
			{
				if (threadGroups.find(threadId) == threadGroups.end())
					threadGroups.insert({ threadId, RoundRobinGroup<T>() });
				else
					return;
			}
			else
			{
				auto group = threadGroups.find(threadId);

				if (group != threadGroups.end())
					threadGroups.erase(group);
				else
					return;
			}

			if (addThread)
				threadSlots.emplace_back(std::vector<TPBlockQueue<T>*>());
			else
				threadSlots.pop_back();

			rebuild_slot_assignments();
			reinitialize_groups();
		}

		void rebuild_slot_assignments()
		{
			if (threadSlots.size())
			{
				for (int i = 0; i < threadSlots.size(); ++i)
					threadSlots[i].clear();

				distribute_queues_to_threads(threadSlots.size());
			}
		}

		void reinitialize_groups()
		{
			if (threadSlots.size())
			{
				unsigned int slotIndex = 0;
				for (auto& group : threadGroups)
				{
					group.second.resetAndInit(&threadSlots[slotIndex], queueCapacity, taskThreshold);
					slotIndex++;
				}
			}
		}

		static unsigned int calculate_balanced_thread_count(unsigned int queueCount, unsigned int threadCount)
		{
			if (threadCount > queueCount)
			{
				while (threadCount > queueCount)
				{
					if (!(threadCount % queueCount))
						break;

					threadCount--;
				}
			}
			else
			{
				while (threadCount)
				{
					if (!(queueCount % threadCount))
						break;

					threadCount--;
				}
			}

			return threadCount;
		}

		void populate_slot(bool forwardOrder, std::vector<TPBlockQueue<T>*>& slot)
		{
			if (forwardOrder)
			{
				for (unsigned int k = 0; k < totalQueues; k++)
					slot.emplace_back(queueArray + k);
			}
			else
			{
				for (unsigned int k = totalQueues; k > 0; k--)
					slot.emplace_back(queueArray + k - 1);
			}
		}

		void handle_remainder_case(unsigned int threadCount)
		{
			bool fillDirection = false;
			unsigned int balancedCount = calculate_balanced_thread_count(totalQueues, threadCount - 1);
			distribute_queues_to_threads(balancedCount);

			for (unsigned int i = 0; i < threadCount - balancedCount; ++i)
			{
				populate_slot(fillDirection, threadSlots[balancedCount + i]);
				fillDirection = !fillDirection;
			}
		}

		void distribute_queues_to_threads(unsigned int threadCount)
		{
			if (!threadCount)
				return;

			if (threadCount <= totalQueues)
			{
				unsigned int queuesPerThread = totalQueues / threadCount;
				unsigned int remainder = totalQueues % threadCount;

				if (remainder)
				{
					handle_remainder_case(threadCount);
				}
				else
				{
					for (unsigned int i = 0; i < threadCount; ++i)
						for (unsigned int k = 0; k < queuesPerThread; k++)
							threadSlots[i].emplace_back(queueArray + i * queuesPerThread + k);
				}
			}
			else
			{
				unsigned int threadsPerQueue = threadCount / totalQueues;
				unsigned int remainder = threadCount % totalQueues;
				if (remainder)
				{
					handle_remainder_case(threadCount);
				}
				else
				{
					for (unsigned int i = 0; i < threadCount; i++)
						threadSlots[i].emplace_back(queueArray + i / threadsPerQueue);
				}
			}

			return;
		}

	public:

		void reset()
		{
			std::vector<std::vector<TPBlockQueue<T>*>>().swap(threadSlots);
			std::map<std::thread::id, RoundRobinGroup<T>>().swap(threadGroups);
		}

		void initialize(TPBlockQueue<T>* queues, unsigned int queueCount, unsigned int capacity, unsigned int threshold)
		{
			this->queueArray = queues;
			this->totalQueues = queueCount;
			this->queueCapacity = capacity;
			this->taskThreshold = threshold;
		}

		RoundRobinGroup<T>* find(std::thread::id threadId)
		{
			auto it = threadGroups.find(threadId);

			if (it != threadGroups.end())
				return &(it->second);

			return nullptr;
		}

		void register_thread(std::thread::id threadId)
		{
			manage_thread_entry(true, threadId);
		}

		void unregister_thread(std::thread::id threadId)
		{
			manage_thread_entry(false, threadId);
		}

		void update(unsigned int newQueueCount)
		{
			if (this->totalQueues != newQueueCount)
			{
				this->totalQueues = newQueueCount;
				rebuild_slot_assignments();
			}
		}
	};
}

#define HSLL_THREADPOOL_TIMEOUT_MILLISECONDS 1
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

		bool monitor;
		unsigned int index;
		unsigned int capacity;
		unsigned int threshold;
		unsigned int* threadNum;

		TPBlockQueue<T>* queues;
		TPBlockQueue<T>* ignore;
		SpinReadWriteLock* rwLock;

		SingleStealer(SpinReadWriteLock* rwLock, TPBlockQueue<T>* queues, TPBlockQueue<T>* ignore,
			unsigned int capacity, unsigned int* threadNum, unsigned int maxThreadum, bool monitor)
		{
			this->index = 0;
			this->capacity = capacity;
			this->threadNum = threadNum;
			this->threshold = std::min(capacity / 2, maxThreadum / 2);
			this->rwLock = rwLock;
			this->queues = queues;
			this->ignore = ignore;
			this->monitor = monitor;
		}

		unsigned int steal(T& element)
		{
			if (monitor)
			{
				ReadLockGuard lock(*rwLock);
				return steal_inner(element);
			}
			else
			{
				return steal_inner(element);
			}
		}

		bool steal_inner(T& element)
		{
			unsigned int num = *threadNum;

			for (unsigned int i = 0; i < num; ++i)
			{
				unsigned int now = (index + i) % num;
				TPBlockQueue<T>* queue = queues + now;

				if (queue != ignore && queue->get_size() >= threshold)
				{
					if (queue->dequeue(element))
					{
						index = now;
						return true;
					}
				}
			}
			return false;
		}
	};

	template <class T>
	class BulkStealer
	{
		template <class TYPE>
		friend class ThreadPool;

	private:

		bool monitor;
		unsigned int index;
		unsigned int capacity;
		unsigned int batchSize;
		unsigned int threshold;
		unsigned int* threadNum;

		TPBlockQueue<T>* queues;
		TPBlockQueue<T>* ignore;
		SpinReadWriteLock* rwLock;

		BulkStealer(SpinReadWriteLock* rwLock, TPBlockQueue<T>* queues, TPBlockQueue<T>* ignore, unsigned int capacity,
			unsigned int* threadNum, unsigned int maxThreadum, unsigned int batchSize, bool monitor)
		{
			this->index = 0;
			this->batchSize = batchSize;
			this->capacity = capacity;
			this->threadNum = threadNum;
			this->threshold = std::min(capacity / 2, batchSize * maxThreadum / 2);
			this->rwLock = rwLock;
			this->queues = queues;
			this->ignore = ignore;
			this->monitor = monitor;
		}

		unsigned int steal(T* elements)
		{
			if (monitor)
			{
				ReadLockGuard lock(*rwLock);
				return steal_inner(elements);
			}
			else
			{
				return steal_inner(elements);
			}
		}

		unsigned int steal_inner(T* elements)
		{
			unsigned int count;
			unsigned int num = *threadNum;

			for (unsigned int i = 0; i < num; ++i)
			{
				unsigned int now = (index + i) % num;
				TPBlockQueue<T>* queue = queues + now;

				if (queue != ignore && queue->get_size() >= threshold)
				{
					if (count = queue->dequeue_bulk(elements, batchSize))
					{
						if (count == batchSize)
							index = now;
						else
							index = (now + 1) % num;

						return count;
					}
				}
			}
			return 0;
		}
	};

	class ThreadPoolRegister
	{
		template <class TYPE>
		friend class ThreadPool;

		struct Key
		{
			void* poolAddr;
			std::thread::id id;

			bool operator<(const Key& other) const noexcept
			{
				if (poolAddr != other.poolAddr)
					return poolAddr < other.poolAddr;

				return id < other.id;
			}
		};

		thread_local static std::set<Key> threadpool_register;


		static bool is_registered(void* poolAddr, std::thread::id id)
		{
			return threadpool_register.find({ poolAddr, id }) != threadpool_register.end();
		}

		static void register_this_thread(void* poolAddr, std::thread::id id)
		{
			if (threadpool_register.find({ poolAddr, id }) == threadpool_register.end())
				threadpool_register.insert({ poolAddr, id });
		}

		static void unregister_this_thread(void* poolAddr, std::thread::id id)
		{
			const auto it = threadpool_register.find({ poolAddr, id });
			if (it != threadpool_register.end())
				threadpool_register.erase(it);
		}
	};

	thread_local std::set<ThreadPoolRegister::Key> ThreadPoolRegister::threadpool_register;

	/**
	 * @brief Thread pool implementation with multiple queues for task distribution
	 */
	template <class T = TaskStack<>>
	class ThreadPool
	{
		static_assert(is_TaskStack<T>::value, "TYPE must be a TaskStack type");

	private:

		unsigned int capacity;
		unsigned int batchSize;
		unsigned int threadNum;
		unsigned int minThreadNum;
		unsigned int maxThreadNum;

		bool enableMonitor;
		Semaphore monitorSem;
		std::atomic<bool> adjustFlag;
		std::chrono::milliseconds adjustMillis;

		T* containers;
		Semaphore* stoppedSem;
		Semaphore* restartSem;
		SpinReadWriteLock rwLock;
		std::atomic<bool> exitFlag;
		std::atomic<bool> shutdownPolicy;

		std::thread monitor;
		TPBlockQueue<T>* queues;
		std::vector<std::thread> workers;
		std::atomic<unsigned int> index;
		TPGroupAllocator<T> groupAllocator;

	public:

		ThreadPool() : queues(nullptr) {}

		/**
		 * @brief Initializes thread pool with fixed number of threads (no dynamic scaling)
		 * @param capacity Capacity of each internal task queue (must be >= 2)
		 * @param threadNum Fixed number of worker threads (must be != 0)
		 * @param batchSize Maximum number of tasks processed per batch (must be != 0)
		 * @return true  Initialization successful
		 * @return false Initialization failed (invalid parameters or resource allocation failure)
		 */
		bool init(unsigned int capacity, unsigned int threadNum, unsigned int batchSize) noexcept
		{
			assert(!queues);

			if (!batchSize || !threadNum || capacity < 2)
				return false;

			if (!initResourse(capacity, threadNum, batchSize))
				return false;

			this->index = 0;
			this->exitFlag = false;
			this->adjustFlag = false;
			this->enableMonitor = false;
			this->shutdownPolicy = true;
			this->minThreadNum = threadNum;
			this->maxThreadNum = threadNum;
			this->threadNum = maxThreadNum;
			this->batchSize = std::min(batchSize, capacity / 2);
			this->capacity = capacity;
			this->adjustMillis = adjustMillis;
			workers.reserve(maxThreadNum);
			groupAllocator.init(queues, maxThreadNum, capacity, capacity * 0.01 > 1 ? capacity * 0.01 : 1);

			for (unsigned i = 0; i < maxThreadNum; ++i)
				workers.emplace_back(&ThreadPool::worker, this, i);

			return true;
		}

		/**
		* @brief Initializes thread pool resources (Dynamic scaling)
		* @param capacity Capacity of each internal queue (must be >= 2)
		* @param minThreadNum Minimum number of worker threads (must be != 0)
		* @param maxThreadNum Maximum number of worker threads (must be >=minThreadNum)
		* @param batchSize Maximum tasks to process per batch (must be != 0)
		* @param adjustMillis Time interval for checking the load and adjusting the number of active threads(must be != 0)
		* @return true  Initialization successful
		* @return false Initialization failed (invalid parameters or resource allocation failure)
		*/
		bool init(unsigned int capacity, unsigned int minThreadNum, unsigned int maxThreadNum,
			unsigned int batchSize, unsigned int adjustMillis = 2500) noexcept
		{
			assert(!queues);

			if (!batchSize || !minThreadNum || capacity< 2 || minThreadNum > maxThreadNum || !adjustMillis)
				return false;

			if (!initResourse(capacity, maxThreadNum, batchSize))
				return false;

			this->index = 0;
			this->exitFlag = false;
			this->adjustFlag = false;
			this->enableMonitor = (minThreadNum != maxThreadNum) ? true : false;
			this->shutdownPolicy = true;
			this->minThreadNum = minThreadNum;
			this->maxThreadNum = maxThreadNum;
			this->threadNum = maxThreadNum;
			this->batchSize = std::min(batchSize, capacity / 2);
			this->capacity = capacity;
			this->adjustMillis = std::chrono::milliseconds(adjustMillis);
			workers.reserve(maxThreadNum);
			groupAllocator.initialize(queues, maxThreadNum, capacity, capacity * 0.05 > 1 ? capacity * 0.05 : 1);

			for (unsigned i = 0; i < maxThreadNum; ++i)
				workers.emplace_back(&ThreadPool::worker, this, i);

			if (enableMonitor)
				monitor = std::thread(&ThreadPool::load_monitor, this);

			return true;
		}

#define HSLL_ENQUEUE_HELPER(exp1,exp2,exp3)							\
																	\
		assert(queues);												\
																	\
		if (maxThreadNum == 1)										\
			return exp1;											\
																	\
		std::thread::id id = std::this_thread::get_id();			\
																	\
		if (ThreadPoolRegister::is_registered(this, id))			\
		{															\
			ReadLockGuard lock(rwLock);								\
																	\
			if (threadNum == 1)										\
				return exp1;										\
																	\
			RoundRobinGroup<T>* group = groupAllocator.find(id);	\
			TPBlockQueue<T>* queue = group->current_queue();			\
			unsigned int size = exp2;								\
																	\
			if (size)												\
			{														\
				group->record(size);								\
				return size;										\
			}														\
			else													\
			{														\
				if ((queue = group->available_queue()))				\
				{													\
					size = exp2;									\
																	\
					if (size)										\
					{												\
						group->record(size);						\
						return size;								\
					}												\
					else											\
					{												\
						group->record(0);							\
					}												\
				}													\
			}														\
																	\
			return size;											\
		}															\
		else														\
		{															\
			if (enableMonitor)										\
			{														\
				ReadLockGuard lock(rwLock);							\
				return exp3;										\
			}														\
			else													\
			{														\
				return exp3;										\
			}														\
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
			HSLL_ENQUEUE_HELPER(
				(queues-> template emplace<POS>(std::forward<Args>(args)...)),
				(queue-> template emplace<POS>(std::forward<Args>(args)...)),
				(select_queue()-> template emplace<POS>(std::forward<Args>(args)...))
			)
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
			HSLL_ENQUEUE_HELPER(
				(queues-> template wait_emplace<POS>(std::forward<Args>(args)...)),
				(queue-> template wait_emplace<POS>(std::forward<Args>(args)...)),
				(select_queue()-> template wait_emplace<POS>(std::forward<Args>(args)...))
			)
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
			HSLL_ENQUEUE_HELPER(
				(queues-> template wait_emplace<POS>(timeout, std::forward<Args>(args)...)),
				(queue-> template wait_emplace<POS>(timeout, std::forward<Args>(args)...)),
				(select_queue()-> template wait_emplace<POS>(timeout, std::forward<Args>(args)...))
			)
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
			HSLL_ENQUEUE_HELPER(
				(queues-> template enqueue<POS>(std::forward<U>(task))),
				(queue-> template enqueue<POS>(std::forward<U>(task))),
				(select_queue()-> template enqueue<POS>(std::forward<U>(task)))
			)
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
			HSLL_ENQUEUE_HELPER(
				(queues-> template wait_push<POS>(std::forward<U>(task))),
				(queue-> template wait_push<POS>(std::forward<U>(task))),
				(select_queue()-> template wait_push<POS>(std::forward<U>(task)))
			)
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
			HSLL_ENQUEUE_HELPER(
				(queues-> template wait_push<POS>(std::forward<U>(task), timeout)),
				(queue-> template wait_push<POS>(std::forward<U>(task), timeout)),
				(select_queue()-> template wait_push<POS>(std::forward<U>(task), timeout))
			)
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
			HSLL_ENQUEUE_HELPER(
				(queues-> template enqueue_bulk<METHOD, POS>(tasks, count)),
				(queue-> template enqueue_bulk<METHOD, POS>(tasks, count)),
				(select_queue()-> template enqueue_bulk<METHOD, POS>(tasks, count))
			)
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
			HSLL_ENQUEUE_HELPER(
				(queues-> template enqueue_bulk<METHOD, POS>(part1, count1, part2, count2)),
				(queue-> template enqueue_bulk<METHOD, POS>(part1, count1, part2, count2)),
				(select_queue()-> template enqueue_bulk<METHOD, POS>(part1, count1, part2, count2))
			)
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
			HSLL_ENQUEUE_HELPER(
				(queues-> template wait_pushBulk<METHOD, POS>(tasks, count)),
				(queue-> template wait_pushBulk<METHOD, POS>(tasks, count)),
				(select_queue()-> template wait_pushBulk<METHOD, POS>(tasks, count))
			)
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
			HSLL_ENQUEUE_HELPER(
				(queues-> template wait_pushBulk<METHOD, POS>(tasks, count, timeout)),
				(queue-> template wait_pushBulk<METHOD, POS>(tasks, count, timeout)),
				(select_queue()-> template wait_pushBulk<METHOD, POS>(tasks, count, timeout))
			)
		}

		/**
		* @brief Get the maximum occupied space of the thread pool.
		*/
		unsigned long long get_max_usage()
		{
			assert(queues);
			return  maxThreadNum * queues->get_bsize();
		}

		/**
		 * @brief Waits for all tasks to complete.
		 * @note
		 *  1. During the join operation, adding any new tasks is prohibited.
		 *  2. This function is not thread-safe.
		 *	3. This function does not clean up resources. After the call, the queue can be used normally.
		 */
		void drain()
		{
			assert(queues);

			if (enableMonitor)
			{
				adjustFlag = true;
				monitorSem.release();

				while (adjustFlag)
					std::this_thread::yield();
			}

			for (int i = 0; i < threadNum; ++i)
			{
				restartSem[i].release();
				queues[i].stopWait();
			}

			for (int i = 0; i < threadNum; ++i)
			{
				stoppedSem[i].acquire();
				queues[i].enableWait();
			}

			if (enableMonitor)
				monitorSem.release();
		}

		/**
		 * @brief Stops all workers and releases resources.
		 * @param shutdownPolicy If true, performs a graceful shutdown (waits for tasks to complete);
		 *                       if false, forces an immediate shutdown.
		 * @note This function is not thread-safe.
		 * @note After calling this function, the thread pool can be reused by calling init again.
		 */
		void exit(bool shutdownPolicy = true) noexcept
		{
			assert(queues);

			if (enableMonitor)
			{
				monitorSem.release();
				monitor.join();
			}

			exitFlag = true;
			this->shutdownPolicy = shutdownPolicy;

			{
				for (unsigned i = 0; i < workers.size(); ++i)
					restartSem[i].release();

				for (unsigned i = 0; i < workers.size(); ++i)
					queues[i].stopWait();

				for (auto& worker : workers)
					worker.join();
			}

			rleaseResourse();
		}

		void register_this_thread()
		{
			std::thread::id id = std::this_thread::get_id();
			ThreadPoolRegister::register_this_thread(this, id);
			WriteLockGuard lock(rwLock);
			groupAllocator.register_thread(id);
		}

		void unregister_this_thread()
		{
			std::thread::id id = std::this_thread::get_id();
			ThreadPoolRegister::unregister_this_thread(this, id);
			WriteLockGuard lock(rwLock);
			groupAllocator.unregister_thread(id);
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

		TPBlockQueue<T>* select_queue() noexcept
		{
			return queues + next_index();
		}

		void load_monitor() noexcept
		{
			unsigned int count = 0;

			while (true)
			{
				if (monitorSem.try_acquire_for(adjustMillis))
				{
					if (adjustFlag)
					{
						adjustFlag = false;
						monitorSem.acquire();
					}
					else
					{
						return;
					}
				}

				unsigned int allSize = capacity * threadNum;
				unsigned int totalSize = 0;

				for (int i = 0; i < threadNum; ++i)
					totalSize += queues[i].get_size();

				if (totalSize < allSize * HSLL_THREADPOOL_SHRINK_FACTOR && threadNum > minThreadNum)
				{
					count++;

					if (count >= 3)
					{
						rwLock.lock_write();
						threadNum--;
						groupAllocator.update(threadNum);
						rwLock.unlock_write();
						queues[threadNum].stopWait();
						stoppedSem[threadNum].acquire();
						queues[threadNum].release();
						count = 0;
					}
				}
				else
				{
					if (totalSize > allSize * HSLL_THREADPOOL_EXPAND_FACTOR && threadNum < maxThreadNum)
					{
						unsigned int newThreads = std::max(1u, (maxThreadNum - threadNum) / 2);
						unsigned int succeed = 0;
						for (int i = threadNum; i < threadNum + newThreads; ++i)
						{
							if (!queues[i].init(capacity))
								break;

							restartSem[i].release();
							succeed++;
						}

						if (succeed > 0)
						{
							rwLock.lock_write();
							threadNum += succeed;
							groupAllocator.update(threadNum);
							rwLock.unlock_write();
						}
					}

					count = 0;
				}
			}
		}

		void worker(unsigned int index) noexcept
		{
			if (batchSize == 1)
				process_single(queues + index, index);
			else
				process_bulk(queues + index, index);
		}

		static inline void execute_tasks(T* tasks, unsigned int count)
		{
			for (unsigned int i = 0; i < count; ++i)
			{
				tasks[i].execute();
				tasks[i].~T();
			}
		}

		void process_single(TPBlockQueue<T>* queue, unsigned int index) noexcept
		{
			bool enableSteal = (maxThreadNum != 1);
			T* task = containers + index * batchSize;
			SingleStealer<T> stealer(&rwLock, queues, queue, capacity, &threadNum, maxThreadNum, enableMonitor);

			while (true)
			{
				while (true)
				{
					while (queue->dequeue(*task))
					{
						task->execute();
						task->~T();
					}

					if (enableSteal && stealer.steal(*task))
					{
						task->execute();
						task->~T();
						goto cheak;
					}

					if (queue->wait_dequeue(*task, std::chrono::milliseconds(HSLL_THREADPOOL_TIMEOUT_MILLISECONDS)))
					{
						task->execute();
						task->~T();
					}

				cheak:

					if (queue->is_Stopped())
						break;
				}

				if (shutdownPolicy)
				{
					while (queue->dequeue(*task))
					{
						task->execute();
						task->~T();
					}
				}

				stoppedSem[index].release();
				restartSem[index].acquire();

				if (exitFlag)
					break;
			}
		}

		void process_bulk(TPBlockQueue<T>* queue, unsigned int index) noexcept
		{
			bool enableSteal = (maxThreadNum != 1);
			T* tasks = containers + index * batchSize;
			BulkStealer<T> stealer(&rwLock, queues, queue, capacity, &threadNum, maxThreadNum, batchSize, enableMonitor);

			while (true)
			{
				unsigned int count;

				while (true)
				{
					while (true)
					{
						unsigned int size = queue->get_size();
						unsigned int round = batchSize;

						while (round && size < batchSize)
						{
							std::this_thread::yield();
							size = queue->get_size();
							round--;
						}

						if (size && (count = queue->dequeue_bulk(tasks, batchSize)))
							execute_tasks(tasks, count);
						else
							break;
					}

					if (enableSteal && (count = stealer.steal(tasks)))
					{
						execute_tasks(tasks, count);
						goto cheak;
					}

					if (count = queue->wait_dequeue_bulk(tasks, batchSize,
						std::chrono::milliseconds(HSLL_THREADPOOL_TIMEOUT_MILLISECONDS)))
						execute_tasks(tasks, count);

				cheak:

					if (queue->is_Stopped())
						break;
				}

				if (shutdownPolicy)
				{
					while (count = queue->dequeue_bulk(tasks, batchSize))
						execute_tasks(tasks, count);
				}

				stoppedSem[index].release();
				restartSem[index].acquire();

				if (exitFlag)
					break;
			}
		}

		bool initResourse(unsigned int capacity, unsigned int maxThreadNum, unsigned int batchSize)
		{
			unsigned int succeed = 0;

			if (!(restartSem = new(std::nothrow) Semaphore[2 * maxThreadNum]))
				goto clean_1;

			stoppedSem = restartSem + maxThreadNum;

			if (!(containers = (T*)ALIGNED_MALLOC(sizeof(T) * batchSize * maxThreadNum, alignof(T))))
				goto clean_2;

			if (!(queues = (TPBlockQueue<T>*)ALIGNED_MALLOC(maxThreadNum * sizeof(TPBlockQueue<T>), alignof(TPBlockQueue<T>))))
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

			delete[] restartSem;

			return false;
		}

		void rleaseResourse()
		{
			for (unsigned i = 0; i < maxThreadNum; ++i)
				queues[i].~TPBlockQueue<T>();

			ALIGNED_FREE(queues);
			ALIGNED_FREE(containers);
			delete[] restartSem;
			queues = nullptr;
			workers.clear();
			workers.shrink_to_fit();
			groupAllocator.reset();
		}
	};

	template <class T, unsigned int BATCH, INSERT_POS POS = TAIL>
	class BatchSubmitter
	{
		static_assert(is_TaskStack<T>::value, "T must be a TaskStack type");
		static_assert(BATCH > 0, "BATCH > 0");
		alignas(alignof(T)) unsigned char buf[BATCH * sizeof(T)];

		T* elements;
		unsigned int size;
		unsigned int index;
		ThreadPool<T>& pool;

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
		BatchSubmitter(ThreadPool<T>& pool) : size(0), index(0), elements((T*)buf), pool(pool) {
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
					submitted = pool.template enqueue<POS>(std::move(*(elements + start))) ? 1 : 0;
				else
					submitted = pool.template enqueue_bulk<MOVE, POS>(elements + start, len1);
			}
			else
			{
				submitted = pool.template enqueue_bulk<MOVE, POS>(
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