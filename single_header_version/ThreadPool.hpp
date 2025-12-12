#ifndef HSLL_THREADPOOL
#define HSLL_THREADPOOL

#include <map>
#include <vector>
#include <future>
#include <thread>
#include <cstddef>
#include <cassert>

// The current function may throw exceptions, including std::bad_alloc.
#define HSLL_MAY_THROW

//For a valid object, the current function can only be called once.
#define HSLL_CALL_ONCE

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
				return nullptr;

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

//TPTaskStack
namespace HSLL
{
	namespace INNER
	{
		//extern
		template <unsigned int TSIZE, unsigned int ALIGN>
		class TaskStack;

		template <typename... Args>
		struct TaskImpl;

		template <typename F, typename... Args>
		class HeapCallable;

		template <typename F, typename... Args>
		class HeapCallable_Async;

		template <typename F, typename... Args>
		class HeapCallable_Cancelable;

		//helper1_sfinae
		template <typename T>
		struct is_TaskImpl : std::false_type {};

		template <typename... Args>
		struct is_TaskImpl<TaskImpl<Args...>> : std::true_type {};

		template <typename T>
		struct is_TaskStack : std::false_type {};

		template <unsigned int S, unsigned int A>
		struct is_TaskStack<TaskStack<S, A>> : std::true_type {};

		template <typename T>
		struct is_HeapCallable : std::false_type {};

		template <typename F, typename... Args>
		struct is_HeapCallable<HeapCallable<F, Args...>> : std::true_type {};

		template <typename T>
		struct is_HeapCallable_Async : std::false_type {};

		template <typename F, typename... Args>
		struct is_HeapCallable_Async<HeapCallable_Async<F, Args...>> : std::true_type {};

		template <typename T>
		struct is_HeapCallable_Cancelable : std::false_type {};

		template <typename F, typename... Args>
		struct is_HeapCallable_Cancelable<HeapCallable_Cancelable<F, Args...>> : std::true_type {};

		template <typename T>
		struct is_reference_wrapper : std::false_type {};

		template <typename T>
		struct is_reference_wrapper<std::reference_wrapper<T>> : std::true_type {};

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

		//helper4_function_traits
		template <typename T>
		struct function_traits;

		template <typename Ret, typename... Args>
		struct function_traits<Ret(*)(Args...)>
		{
			using result_type = Ret;
			static constexpr bool is_member_function = false;
		};

		template <typename Ret, typename... Args>
		struct function_traits<Ret(&)(Args...)>
		{
			using result_type = Ret;
			static constexpr bool is_member_function = false;
		};

		template <typename Class, typename Ret, typename... Args>
		struct function_traits<Ret(Class::*)(Args...)>
		{
			using result_type = Ret;
			static constexpr bool is_member_function = true;
		};

		template <typename Class, typename Ret, typename... Args>
		struct function_traits<Ret(Class::*)(Args...) const>
		{
			using result_type = Ret;
			static constexpr bool is_member_function = true;
		};

		template <typename Functor>
		struct function_traits
		{
		private:
			using call_type = function_traits<decltype(&Functor::operator())>;
		public:
			using result_type = typename call_type::result_type;
			static constexpr bool is_member_function = false;
		};

		template <typename Callable>
		using function_result_type = typename function_traits<typename std::decay<Callable>::type>::result_type;

		//helper5_invoke
		enum TASK_TUPLE_TYPE
		{
			TASK_TUPLE_TYPE_BASE,
			TASK_TUPLE_TYPE_NORMAL,
			TASK_TUPLE_TYPE_ASYNC,
			TASK_TUPLE_TYPE_CANCELABLE
		};

		template<TASK_TUPLE_TYPE TYPE>
		struct Invoker
		{
		};

		template<>
		struct Invoker<TASK_TUPLE_TYPE_BASE>
		{
			template <typename ResultType, typename Callable,
				typename std::enable_if<std::is_void<ResultType>::value, bool>::type = true,
				typename std::enable_if<!function_traits<Callable>::is_member_function, bool>::type = true,
				typename... Ts>
			static void invoke(Callable& callable, Ts &...args)
			{
				callable(args...);
			}

			template <typename ResultType, typename Callable,
				typename std::enable_if<!std::is_void<ResultType>::value, bool>::type = true,
				typename std::enable_if<!function_traits<Callable>::is_member_function, bool>::type = true,
				typename... Ts>
			static ResultType invoke(Callable& callable, Ts &...args)
			{
				return callable(args...);
			}

			template <typename ResultType, typename Callable, typename OBJ,
				typename std::enable_if<std::is_void<ResultType>::value, bool>::type = true,
				typename std::enable_if<function_traits<Callable>::is_member_function, bool>::type = true,
				typename std::enable_if<std::is_pointer<OBJ>::value, bool>::type = true,
				typename... Ts>
			static void invoke(Callable& callable, OBJ& obj, Ts&... args)
			{
				(obj->*callable)(args...);
			}

			template <typename ResultType, typename Callable, typename OBJ,
				typename std::enable_if<!std::is_void<ResultType>::value, bool>::type = true,
				typename std::enable_if<function_traits<Callable>::is_member_function, bool>::type = true,
				typename std::enable_if<std::is_pointer<OBJ>::value, bool>::type = true,
				typename... Ts>
			static ResultType invoke(Callable& callable, OBJ& obj, Ts&... args)
			{
				return (obj->*callable)(args...);
			}

			template <typename ResultType, typename Callable, typename OBJ,
				typename std::enable_if<std::is_void<ResultType>::value, bool>::type = true,
				typename std::enable_if<function_traits<Callable>::is_member_function, bool>::type = true,
				typename std::enable_if<!std::is_pointer<OBJ>::value, bool>::type = true,
				typename std::enable_if<!is_reference_wrapper<OBJ>::value, bool>::type = true,
				typename... Ts>
			static void invoke(Callable& callable, OBJ& obj, Ts&... args)
			{
				(obj.*callable)(args...);
			}

			template <typename ResultType, typename Callable, typename OBJ,
				typename std::enable_if<!std::is_void<ResultType>::value, bool>::type = true,
				typename std::enable_if<function_traits<Callable>::is_member_function, bool>::type = true,
				typename std::enable_if<!std::is_pointer<OBJ>::value, bool>::type = true,
				typename std::enable_if<!is_reference_wrapper<OBJ>::value, bool>::type = true,
				typename... Ts>
			static ResultType invoke(Callable& callable, OBJ& obj, Ts&... args)
			{
				return (obj.*callable)(args...);
			}

			template <typename ResultType, typename Callable, typename OBJ,
				typename std::enable_if<std::is_void<ResultType>::value, bool>::type = true,
				typename std::enable_if<function_traits<Callable>::is_member_function, bool>::type = true,
				typename std::enable_if<!std::is_pointer<OBJ>::value, bool>::type = true,
				typename std::enable_if<is_reference_wrapper<OBJ>::value, bool>::type = true,
				typename... Ts>
			static void invoke(Callable& callable, OBJ& obj, Ts&... args)
			{
				(obj.get().*callable)(args...);
			}

			template <typename ResultType, typename Callable, typename OBJ,
				typename std::enable_if<!std::is_void<ResultType>::value, bool>::type = true,
				typename std::enable_if<function_traits<Callable>::is_member_function, bool>::type = true,
				typename std::enable_if<!std::is_pointer<OBJ>::value, bool>::type = true,
				typename std::enable_if<is_reference_wrapper<OBJ>::value, bool>::type = true,
				typename... Ts>
			static ResultType invoke(Callable& callable, OBJ& obj, Ts&... args)
			{
				return (obj.get().*callable)(args...);
			}
		};

		template<>
		struct Invoker<TASK_TUPLE_TYPE_NORMAL>
		{
			template <typename ResultType, typename Callable, typename... Ts>
			static void invoke(Callable& callable, Ts &...args)
			{
				Invoker<TASK_TUPLE_TYPE_BASE>::invoke<ResultType>(callable, args...);
			}
		};

		template<>
		struct Invoker<TASK_TUPLE_TYPE_ASYNC>
		{
			template <typename ResultType, typename std::enable_if<std::is_void<ResultType>::value, bool>::type = true,
				typename Promise, typename Callable, typename... Ts>
			static void invoke(Promise& promise, Callable& callable, Ts &...args)
			{
				Invoker<TASK_TUPLE_TYPE_BASE>::invoke<ResultType>(callable, args...);
			}

			template <typename ResultType, typename std::enable_if<!std::is_void<ResultType>::value, bool>::type = true,
				typename Promise, typename Callable, typename... Ts>
			static ResultType invoke(Promise& promise, Callable& callable, Ts &...args)
			{
				return Invoker<TASK_TUPLE_TYPE_BASE>::invoke<ResultType>(callable, args...);
			}
		};

		template<>
		struct Invoker<TASK_TUPLE_TYPE_CANCELABLE>
		{
			template <typename ResultType, typename std::enable_if<std::is_void<ResultType>::value, bool>::type = true,
				typename Promise, typename Flag, typename Callable, typename... Ts>
			static void invoke(Promise& promise, Flag& flag, Callable& callable, Ts &...args)
			{
				Invoker<TASK_TUPLE_TYPE_BASE>::invoke<ResultType>(callable, args...);
			}

			template <typename ResultType, typename std::enable_if<!std::is_void<ResultType>::value, bool>::type = true,
				typename Promise, typename Flag, typename Callable, typename... Ts>
			static ResultType invoke(Promise& promise, Flag& flag, Callable& callable, Ts &...args)
			{
				return Invoker<TASK_TUPLE_TYPE_BASE>::invoke<ResultType>(callable, args...);
			}
		};

		//helper6_apply
		template <TASK_TUPLE_TYPE TYPE, typename ResultType, typename std::enable_if<std::is_void<ResultType>::value, bool>::type = true,
			typename Tuple, size_t... Is>
		void apply_impl(Tuple& tup, index_sequence<Is...>)
		{
			Invoker<TYPE>::template invoke<ResultType>(std::get<Is>(tup)...);
		}

		template <TASK_TUPLE_TYPE TYPE, typename ResultType, typename std::enable_if<!std::is_void<ResultType>::value, bool>::type = true,
			typename Tuple, size_t... Is>
		ResultType apply_impl(Tuple& tup, index_sequence<Is...>)
		{
			return Invoker<TYPE>::template invoke<ResultType>(std::get<Is>(tup)...);
		}

		template <TASK_TUPLE_TYPE TYPE = TASK_TUPLE_TYPE_NORMAL, typename ResultType = void,
			typename std::enable_if<std::is_void<ResultType>::value, bool>::type = true, typename Tuple>
		void tuple_apply(Tuple& tup)
		{
			apply_impl<TYPE, ResultType>(tup, typename make_index_sequence<std::tuple_size<Tuple>::value>::type{});
		}

		template <TASK_TUPLE_TYPE TYPE = TASK_TUPLE_TYPE_NORMAL, typename ResultType = void,
			typename std::enable_if<!std::is_void<ResultType>::value, bool>::type = true, typename Tuple>
		ResultType tuple_apply(Tuple& tup)
		{
			return apply_impl<TYPE, ResultType>(tup, typename make_index_sequence<std::tuple_size<Tuple>::value>::type{});
		}

		/**
		 * @class HeapCallable
		 * @brief Encapsulates a callable object and its arguments, storing them on the heap.
		 *        This class is move-only and non-assignable
		 * @tparam F Type of the callable object
		 * @tparam Args Types of the arguments bound to the callable
		 */
		template <typename F, typename... Args>
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
			template<typename Func, typename std::enable_if<!is_HeapCallable<typename std::decay<Func>::type>::value, bool>::type = true >
			HeapCallable(Func&& func, Args &&...args) HSLL_MAY_THROW
				: storage(new Package(std::forward<Func>(func), std::forward<Args>(args)...)) {
			}

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
		template <typename F, typename... Args>
		class HeapCallable_Async
		{
			using ResultType = function_result_type<F>;
			using Package = std::tuple<std::promise<ResultType>, typename std::decay<F>::type, typename std::decay<Args>::type...>;

		private:
			std::unique_ptr<Package> storage;

			template<typename T = ResultType, typename std::enable_if<std::is_void<T>::value, bool>::type = true>
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

			template<typename T = ResultType, typename std::enable_if<!std::is_void<T>::value, bool>::type = true>
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
			template<typename Func, typename std::enable_if<!is_HeapCallable_Async<typename std::decay<Func>::type>::value, bool>::type = true >
			HeapCallable_Async(Func&& func, Args &&...args) HSLL_MAY_THROW
				: storage(new Package(std::promise<ResultType>(), std::forward<Func>(func), std::forward<Args>(args)...)) {
			}

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
			std::future<ResultType> get_future() HSLL_CALL_ONCE
			{
				return std::get<0>(*storage).get_future();
			}
		};

		class CancelableFlag
		{
			std::atomic<bool> flag;

		public:

			CancelableFlag(bool ignore = false) : flag(false) {};

			/**
			 * @brief Requests cancellation of the associated task
			 * @return true if cancellation succeeded (state was active), false if already canceled/completed
			 * @note On success:
			 * - Sets promise exception with "Task canceled" error
			 * - Transitions state to canceled
			 */
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
			bool enter() HSLL_CALL_ONCE
			{
				bool expected = false;

				if (flag.compare_exchange_strong(expected, true))
					return true;

				return false;
			}
		};

		/**
		 * @class HeapCallable_Cancelable
		 * @brief Cancelable version of HeapCallable with atomic cancellation flag.
		 *        This class is move-only and non-assignable
		 * @tparam F Type of the callable object
		 * @tparam Args Types of the arguments bound to the callable
		 */
		template <typename F, typename... Args>
		class HeapCallable_Cancelable
		{
			using ResultType = function_result_type<F>;
			using Package = std::tuple<std::promise<ResultType>, CancelableFlag,
				typename std::decay<F>::type, typename std::decay<Args>::type...>;

		private:
			std::shared_ptr<Package> storage;

			template<typename T = ResultType, typename std::enable_if<std::is_void<T>::value, bool>::type = true>
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

			template<typename T = ResultType, typename std::enable_if<!std::is_void<T>::value, bool>::type = true>
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
					:storage(storage), future(std::get<0>(*storage).get_future()) {
				};

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
				typename std::enable_if<!std::is_void<U>::value, U>::type get() HSLL_CALL_ONCE
				{
					return future.get();
				}

				/**
				 * @brief Synchronizes with task completion (void specialization)
				 * @throws Propagates any exception stored in the promise
				 * @throws std::runtime_error("Task canceled") if canceled
				 */
				template <typename U = ResultType>
				typename std::enable_if<std::is_void<U>::value>::type get() HSLL_CALL_ONCE
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
				template <typename Rep, typename Period>
				std::future_status wait_for(const std::chrono::duration<Rep, Period>& timeout_duration) const
				{
					return future.wait_for(timeout_duration);
				}

				/**
				 * @brief Blocks until specified time point
				 * @return Status of future after waiting
				 */
				template <typename Clock, typename Duration>
				std::future_status wait_until(const std::chrono::time_point<Clock, Duration>& timeout_time) const
				{
					return future.wait_until(timeout_time);
				}
			};

			/**
			 * @brief Constructs a cancelable callable object
			 * @param func Callable object to store
			 * @param args Arguments to bind to the callable
			 */
			template<typename Func, typename std::enable_if<!is_HeapCallable_Cancelable<typename std::decay<Func>::type>::value, bool>::type = true >
			HeapCallable_Cancelable(Func&& func, Args &&...args) HSLL_MAY_THROW
				: storage(std::make_shared<Package>(std::promise<ResultType>(), false, std::forward<Func>(func), std::forward<Args>(args)...)) {
			}

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

			Controller get_controller() HSLL_CALL_ONCE
			{
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
		HeapCallable<F, Args...> make_callable(F&& func, Args &&...args) HSLL_MAY_THROW
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
		HeapCallable_Async<F, Args...> make_callable_async(F&& func, Args &&...args) HSLL_MAY_THROW
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
		HeapCallable_Cancelable<F, Args...> make_callable_cancelable(F&& func, Args &&...args) HSLL_MAY_THROW
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
			virtual void execute() = 0;
			virtual void moveTo(void* memory) = 0;
		};

		/**
		 * @brief Concrete task implementation storing function and arguments
		 * @details Stores decayed copies of function and arguments in a tuple
		 */
		template <typename... Args>
		struct TaskImpl : TaskBase
		{
			using Tuple = std::tuple<typename std::decay<Args>::type...>;
			Tuple storage;

			template <typename T>
			static auto move_or_copy(T& obj)
				-> typename std::enable_if<std::is_move_constructible<T>::value, T&&>::type
			{
				return std::move(obj);
			}

			template <typename T>
			static auto move_or_copy(T& obj)
				-> typename std::enable_if<!std::is_move_constructible<T>::value, T&>::type
			{
				return obj;
			}

			template <size_t... Is>
			void move_impl(void* dst, index_sequence<Is...>)
			{
				move_conditional(dst, std::get<Is>(storage)...);
			}

			template <typename... Ts>
			void move_conditional(void* dst, Ts &...ts)
			{
				new (dst) TaskImpl(move_or_copy(ts)...);
			}

			template <typename Func, typename... Ts,
				typename std::enable_if<!is_TaskImpl<typename std::decay<Func>::type>::value, bool>::type = true>
			TaskImpl(Func&& func, Ts &&...ts)
				: storage(std::forward<Func>(func), std::forward<Ts>(ts)...) {
			}


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

			void execute() override
			{
				invoke<sizeof...(Args) == 1>();
			}

			void moveTo(void* dst) override
			{
				move_impl(dst, typename make_index_sequence<std::tuple_size<Tuple>::value>::type{});
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
		template <typename F, typename... Args>
		struct TaskImplTraits
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


			template <bool Condition, typename F, typename... Args>
			typename std::enable_if<Condition, void>::type construct(F&& func, Args &&...args)//normal
			{
				using ImplType = typename TaskImplTraits<F, Args...>::type;
				new (storage) ImplType(std::forward<F>(func), std::forward<Args>(args)...);
			}

			template <bool Condition, typename F, typename... Args>
			typename std::enable_if<!Condition, void>::type construct(F&& func, Args &&...args)//HeapCallable
			{
				using ImplType = typename TaskImplTraits<HeapCallable<F, Args...>>::type;
				new (storage) ImplType(HeapCallable<F, Args...>(std::forward<F>(func), std::forward<Args>(args)...));
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
			template <typename F, typename... Args>
			struct is_stored_on_stack
			{
				typedef typename TaskImplTraits<F, Args...>::type ImplType;
				static constexpr bool value = (sizeof(ImplType) <= TSIZE && alignof(ImplType) <= ALIGN);
			};

			/**
			 * @brief Constructs a task in internal storage
			 *
			 * Creates a task by storing the callable object and bound arguments. The storage location is determined automatically:
			 *   - If the total size of the task implementation <= TSIZE and its alignment <= ALIGN:
			 *        Stores directly in the internal stack buffer
			 *   - Otherwise:
			 *        Allocates the task on the heap and stores a pointer in the buffer (using HeapCallable wrapper)
			 *
			 * @tparam F Type of callable object (automatically deduced)
			 * @tparam Args Types of bound arguments (automatically deduced)
			 * @param func Callable target function (function object/lambda/etc)
			 * @param args Arguments to bind to the function call
			 *
			 * @note Important usage considerations:
			 * 1. Arguments are stored as decayed types. Use `std::ref` to preserve references.
			 * 2. Stored argument types need not match the function signature exactly, but must be convertible.
			 *    Example: `void func(std::atomic<bool> , float)` can be called with `(bool, double)` arguments.
			 * 3. Beware of implicit conversions that may cause precision loss or narrowing (e.g., double->float,
			 *    long long->int). Explicitly specify argument types (e.g., `5.0f`, `6ULL`) to avoid unintended conversions.

			 */
			template <typename F, typename... Args,
				typename std::enable_if<!is_TaskStack<typename std::decay<F>::type>::value, bool>::type = true>
			TaskStack(F&& func, Args &&...args) HSLL_MAY_THROW
			{
				using ImplType = typename TaskImplTraits<F, Args...>::type;
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

			TaskStack(TaskStack&& other) noexcept
			{
				other.getBase()->moveTo(storage);
			}

			TaskStack& operator=(TaskStack&& other) noexcept
			{
				if (this != &other)
				{
					getBase()->~TaskBase();
					other.getBase()->moveTo(storage);
				}
				return *this;
			}

			~TaskStack()
			{
				getBase()->~TaskBase();
			}

			TaskStack(const TaskStack&) = delete;
			TaskStack& operator=(const TaskStack&) = delete;

		private:
			TaskBase* getBase()
			{
				return (TaskBase*)storage;
			}

			const TaskBase* getBase() const
			{
				return (const TaskBase*)storage;
			}
		};
	}

	using INNER::HeapCallable;
	using INNER::HeapCallable_Async;
	using INNER::HeapCallable_Cancelable;
	using INNER::make_callable;
	using INNER::make_callable_async;
	using INNER::make_callable_cancelable;
	using INNER::TaskImplTraits;
	using INNER::TaskStack;
}

//TPSRWLock
namespace HSLL
{
	namespace INNER
	{
		constexpr intptr_t HSLL_SPINREADWRITELOCK_MAXSLOTS = 32;
		constexpr intptr_t HSLL_SPINREADWRITELOCK_MAXREADER = (sizeof(intptr_t) == 4 ? (1 << 30) : (1LL << 62));

		static_assert(HSLL_SPINREADWRITELOCK_MAXSLOTS > 0, "HSLL_SPINREADWRITELOCK_MAXSLOTS must be > 0");
		static_assert(HSLL_SPINREADWRITELOCK_MAXREADER > 0 && HSLL_SPINREADWRITELOCK_MAXREADER <= (sizeof(intptr_t) == 4 ? (1 << 30) : (1LL << 62)),
			"HSLL_SPINREADWRITELOCK_MAXREADER must be > 0 and <= (2^30 for 32-bit, 2^62 for 64-bit)");

		/**
		* @brief Efficient spin lock based on atomic variables, suitable for scenarios where reads significantly outnumber writes
		*/
		class SpinReadWriteLock
		{
		private:

			class alignas(64) InnerLock
			{
			private:
				std::atomic<intptr_t> count;

			public:

				InnerLock() noexcept :count(0) {}

				void lock_read() noexcept
				{
					intptr_t old = count.fetch_add(1, std::memory_order_acquire);

					while (old < 0)
					{
						count.fetch_sub(1, std::memory_order_relaxed);

						std::this_thread::yield();

						while (count.load(std::memory_order_relaxed) < 0)
							std::this_thread::yield();

						old = count.fetch_add(1, std::memory_order_acquire);
					}
				}

				void unlock_read() noexcept
				{
					count.fetch_sub(1, std::memory_order_relaxed);
				}

				bool mark_write() noexcept
				{
					return !count.fetch_sub(HSLL_SPINREADWRITELOCK_MAXREADER, std::memory_order_relaxed);
				}

				void unmark_write(bool ready) noexcept
				{
					if (ready)
						count.store(0, std::memory_order_relaxed);
					else
						count.fetch_add(HSLL_SPINREADWRITELOCK_MAXREADER, std::memory_order_relaxed);
				}

				void unlock_write() noexcept
				{
					count.store(0, std::memory_order_release);
				}

				bool is_write_ready() noexcept
				{
					return count.load(std::memory_order_relaxed) == -HSLL_SPINREADWRITELOCK_MAXREADER;
				}
			};

			class LocalReadLock
			{
				InnerLock& localLock;

			public:

				explicit LocalReadLock(InnerLock& localLock) noexcept :localLock(localLock) {}

				void lock_read() noexcept
				{
					localLock.lock_read();
				}

				void unlock_read() noexcept
				{
					localLock.unlock_read();
				}
			};

			std::atomic<bool> flag;
			InnerLock rwLocks[HSLL_SPINREADWRITELOCK_MAXSLOTS];

			thread_local static intptr_t localIndex;
			static std::atomic<intptr_t> globalIndex;

			inline LocalReadLock get_local_lock() noexcept
			{
				intptr_t index = localIndex;

				if (index == -1)
					index = localIndex = globalIndex.fetch_add(1, std::memory_order_relaxed) % HSLL_SPINREADWRITELOCK_MAXSLOTS;

				return LocalReadLock(rwLocks[index]);
			}

			bool try_mark_write(bool flagArray[HSLL_SPINREADWRITELOCK_MAXSLOTS]) noexcept
			{
				bool old = true;

				if (!flag.compare_exchange_strong(old, false, std::memory_order_acquire, std::memory_order_relaxed))
					return false;

				for (intptr_t i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i)
					flagArray[i] = rwLocks[i].mark_write();

				return true;
			}

			bool try_mark_write_check_before(bool flagArray[HSLL_SPINREADWRITELOCK_MAXSLOTS]) noexcept
			{
				if (!flag.load(std::memory_order_relaxed))
					return false;

				return try_mark_write(flagArray);
			}

			void mark_write(bool flagArray[HSLL_SPINREADWRITELOCK_MAXSLOTS]) noexcept
			{
				if (try_mark_write(flagArray))
					return;

				std::this_thread::yield();

				while (!try_mark_write_check_before(flagArray))
					std::this_thread::yield();
			}

			void unmark_write(bool flagArray[HSLL_SPINREADWRITELOCK_MAXSLOTS]) noexcept
			{
				for (intptr_t i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i)
					rwLocks[i].unmark_write(flagArray[i]);

				flag.store(true, std::memory_order_relaxed);
			}

			intptr_t ready_count(intptr_t startIndex, bool flagArray[HSLL_SPINREADWRITELOCK_MAXSLOTS]) noexcept
			{
				intptr_t index = HSLL_SPINREADWRITELOCK_MAXSLOTS;

				for (intptr_t i = startIndex; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i)
				{
					if (flagArray[i])
						continue;

					flagArray[i] = rwLocks[i].is_write_ready();

					if (!flagArray[i] && i < index)
						index = i;
				}

				return index;
			}

		public:

			SpinReadWriteLock() noexcept :flag(true) {}

			void lock_read() noexcept
			{
				get_local_lock().lock_read();
			}

			void unlock_read() noexcept
			{
				get_local_lock().unlock_read();
			}

			void lock_write() noexcept
			{
				bool flagArray[HSLL_SPINREADWRITELOCK_MAXSLOTS];

				mark_write(flagArray);

				intptr_t nextCheckIndex = 0;

				while ((nextCheckIndex = ready_count(nextCheckIndex, flagArray)) != HSLL_SPINREADWRITELOCK_MAXSLOTS)
					std::this_thread::yield();
			}

			void unlock_write() noexcept
			{
				for (intptr_t i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i)
					rwLocks[i].unlock_write();

				flag.store(true, std::memory_order_release);
			}

			SpinReadWriteLock(const SpinReadWriteLock&) = delete;
			SpinReadWriteLock& operator=(const SpinReadWriteLock&) = delete;
		};

		std::atomic<intptr_t> SpinReadWriteLock::globalIndex{ 0 };
		thread_local intptr_t SpinReadWriteLock::localIndex{ -1 };

		class ReadLockGuard
		{
		private:

			SpinReadWriteLock& lock;

		public:

			explicit ReadLockGuard(SpinReadWriteLock& lock) noexcept : lock(lock)
			{
				lock.lock_read();
			}

			~ReadLockGuard() noexcept
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

			explicit WriteLockGuard(SpinReadWriteLock& lock) noexcept : lock(lock)
			{
				lock.lock_write();
			}

			~WriteLockGuard() noexcept
			{
				lock.unlock_write();
			}

			WriteLockGuard(const WriteLockGuard&) = delete;
			WriteLockGuard& operator=(const WriteLockGuard&) = delete;
		};
	}
}

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>

namespace HSLL
{
	namespace INNER
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

			void release()
			{
				if (!ReleaseSemaphore(m_sem, 1, nullptr))
					throw std::system_error(GetLastError(), std::system_category());
			}

			Semaphore(const Semaphore&) = delete;
			Semaphore& operator=(const Semaphore&) = delete;
			Semaphore(Semaphore&&) = delete;
			Semaphore& operator=(Semaphore&&) = delete;

		private:
			HANDLE m_sem = nullptr;
			static constexpr DWORD MAX_WAIT_MS = INFINITE - 1;
		};
	}
}

#elif defined(__APPLE__)
#include <dispatch/dispatch.h>

namespace HSLL
{
	namespace INNER
	{
		class Semaphore
		{
		public:
			explicit Semaphore(unsigned int initial_count = 0)
			{
				m_sem = dispatch_semaphore_create(static_cast<long>(initial_count));
				if (!m_sem)
					throw std::system_error(errno, std::system_category());
			}

			~Semaphore() noexcept
			{
				dispatch_release(m_sem);
			}

			void acquire()
			{
				dispatch_semaphore_wait(m_sem, DISPATCH_TIME_FOREVER);
			}

			template<typename Rep, typename Period>
			bool try_acquire_for(const std::chrono::duration<Rep, Period>& timeout)
			{
				auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout);
				return dispatch_semaphore_wait(m_sem,
					dispatch_time(DISPATCH_TIME_NOW, ns.count())) == 0;
			}

			void release()
			{
				dispatch_semaphore_signal(m_sem);
			}

			Semaphore(const Semaphore&) = delete;
			Semaphore& operator=(const Semaphore&) = delete;
			Semaphore(Semaphore&&) = delete;
			Semaphore& operator=(Semaphore&&) = delete;

		private:
			dispatch_semaphore_t m_sem;
		};
	}
}

#elif defined(__linux__) || defined(__FreeBSD__) || \
      defined(__OpenBSD__) || defined(__NetBSD__)

#include <semaphore.h>
#include <time.h>

namespace HSLL
{
	namespace INNER
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
				auto since_epoch = abs_time.time_since_epoch();

				auto secs = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
				auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch - secs);

				struct timespec ts;
				ts.tv_sec = secs.count();
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

			void release()
			{
				if (sem_post(&m_sem) != 0)
					throw std::system_error(errno, std::system_category());
			}

			Semaphore(const Semaphore&) = delete;
			Semaphore& operator=(const Semaphore&) = delete;
			Semaphore(Semaphore&&) = delete;
			Semaphore& operator=(Semaphore&&) = delete;

		private:
			sem_t m_sem;
		};
	}
}

#else // Generic fallback for other platforms
#include <mutex>
#include <condition_variable>

namespace HSLL
{
	namespace INNER
	{
		class Semaphore
		{
		public:
			explicit Semaphore(unsigned int initial_count = 0)
				: m_count(initial_count) {
			}

			void acquire()
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_cv.wait(lock, [this] { return m_count > 0; });
				--m_count;
			}

			template<typename Rep, typename Period>
			bool try_acquire_for(const std::chrono::duration<Rep, Period>& timeout)
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				if (!m_cv.wait_for(lock, timeout, [this] { return m_count > 0; }))
					return false;
				--m_count;
				return true;
			}

			void release()
			{
				{
					std::lock_guard<std::mutex> lock(m_mutex);
					++m_count;
				}
				m_cv.notify_one();
			}

			Semaphore(const Semaphore&) = delete;
			Semaphore& operator=(const Semaphore&) = delete;
			Semaphore(Semaphore&&) = delete;
			Semaphore& operator=(Semaphore&&) = delete;

		private:
			std::mutex m_mutex;
			std::condition_variable m_cv;
			unsigned int m_count = 0;
		};
	}
}

#endif

//TPBlockQueue
namespace HSLL
{
	namespace INNER
	{
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
		template <typename TYPE>
		class alignas(64) TPBlockQueue
		{
			// Memory management
			bool stopped;		///< Flag for stopping all operations
			void* memoryBlock;	///< Raw memory block for element storage

			// Queue state tracking
			unsigned int maxSpin;
			unsigned int capacity;	///< Capacity of the queue
			unsigned int totalsize; ///< Total allocated memory size
			unsigned int size;		///< Current number of elements in queue

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
				if (HSLL_UNLIKELY((uintptr_t)dataListTail == border))
					dataListTail = (TYPE*)(uintptr_t)memoryBlock;
			}

			void move_head_next()
			{
				dataListHead = (TYPE*)((char*)dataListHead + sizeof(TYPE));
				if (HSLL_UNLIKELY((uintptr_t)dataListHead == border))
					dataListHead = (TYPE*)(uintptr_t)memoryBlock;
			}

			// Reserve for head push
			void move_head_prev()
			{
				dataListHead = (TYPE*)((char*)dataListHead - sizeof(TYPE));
				if (HSLL_UNLIKELY((uintptr_t)dataListHead < (uintptr_t)memoryBlock))
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

			template <INSERT_POS POS>
			typename std::enable_if<POS == TAIL>::type enqueue_bulk_impl(TYPE* elements, unsigned int toPush)
			{
				for (unsigned int i = 0; i < toPush; ++i)
				{
					new (dataListTail) TYPE(std::move(elements[i]));
					move_tail_next();
				}
			}

			template <INSERT_POS POS>
			typename std::enable_if<POS == HEAD>::type enqueue_bulk_impl(TYPE* elements, unsigned int toPush)
			{
				for (unsigned int i = 0; i < toPush; ++i)
				{
					move_head_prev();
					new (dataListHead) TYPE(std::move(elements[toPush - i - 1]));
				}
			}

			template <INSERT_POS POS>
			typename std::enable_if<POS == TAIL>::type enqueue_bulk_impl(TYPE* part1, unsigned int count1, TYPE* part2, unsigned int count2)
			{
				for (unsigned int i = 0; i < count1; ++i)
				{
					new (dataListTail) TYPE(std::move(part1[i]));
					move_tail_next();
				}

				for (unsigned int i = 0; i < count2; ++i)
				{
					new (dataListTail) TYPE(std::move(part2[i]));
					move_tail_next();
				}
			}

			template <INSERT_POS POS>
			typename std::enable_if<POS == HEAD>::type enqueue_bulk_impl(TYPE* part1, unsigned int count1, TYPE* part2, unsigned int count2)
			{
				for (unsigned int i = 0; i < count1; ++i)
				{
					move_head_prev();
					new (dataListHead) TYPE(std::move(part1[count1 - i - 1]));
				}

				for (unsigned int i = 0; i < count2; ++i)
				{
					move_head_prev();
					new (dataListHead) TYPE(std::move(part2[count2 - i - 1]));
				}
			}

			template <INSERT_POS POS, typename... Args>
			void emplace_helper(std::unique_lock<std::mutex>& lock, Args &&...args)
			{
				size++;
				emplace_impl<POS>(std::forward<Args>(args)...);
				lock.unlock();
				notEmptyCond.notify_one();
			}

			template <INSERT_POS POS>
			unsigned int enqueue_bulk_helper(std::unique_lock<std::mutex>& lock, TYPE* elements, unsigned int count)
			{
				unsigned int toPush = std::min(count, capacity - size);
				size += toPush;
				enqueue_bulk_impl<POS>(elements, toPush);
				lock.unlock();

				if (HSLL_UNLIKELY(toPush == 1))
					notEmptyCond.notify_one();
				else
					notEmptyCond.notify_all();
				return toPush;
			}

			template <INSERT_POS POS>
			unsigned int enqueue_bulk_helper(std::unique_lock<std::mutex>& lock,
				TYPE* part1, unsigned int count1, TYPE* part2, unsigned int count2)
			{
				unsigned int toPush = std::min(count1 + count2, capacity - size);
				size += toPush;

				if (toPush > count1)
					enqueue_bulk_impl<POS>(part1, count1, part2, toPush - count1);
				else
					enqueue_bulk_impl<POS>(part1, toPush);

				lock.unlock();

				if (HSLL_UNLIKELY(toPush == 1))
					notEmptyCond.notify_one();
				else
					notEmptyCond.notify_all();
				return toPush;
			}

			void dequeue_helper(std::unique_lock<std::mutex>& lock, TYPE& element)
			{
				size -= 1;
				move_element(element, *dataListHead);
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
					move_element(elements[i], *dataListHead);
					move_head_next();
				}

				lock.unlock();

				if (HSLL_UNLIKELY(toPop == 1))
					notFullCond.notify_one();
				else
					notFullCond.notify_all();
				return toPop;
			}

			void move_element(TYPE& dst, TYPE& src)
			{
				new (&dst) TYPE(std::move(src));
				src.~TYPE();
			}

			void wait_element()
			{
				for (unsigned int i = 0; i < maxSpin; ++i)
				{
					if (size)
						return;
				}

				std::this_thread::yield();
				return;
			}

		public:

			TPBlockQueue() : memoryBlock(nullptr), stopped(false) {}

			bool init(unsigned int capacity, unsigned int maxSpin = 5000)
			{
				assert(!memoryBlock);

				if (!capacity)
					return false;

				totalsize = sizeof(TYPE) * capacity;
				memoryBlock = HSLL_ALIGNED_MALLOC(totalsize, alignof(TYPE));

				if (!memoryBlock)
					return false;

				this->size = 0;
				this->maxSpin = maxSpin;
				this->capacity = capacity;
				this->dataListHead = (TYPE*)memoryBlock;
				this->dataListTail = (TYPE*)memoryBlock;
				this->border = (uintptr_t)memoryBlock + totalsize;
				return true;
			}

			template <INSERT_POS POS = TAIL, typename... Args>
			bool emplace(Args &&...args)
			{
				assert(memoryBlock);
				std::unique_lock<std::mutex> lock(dataMutex);

				if (HSLL_UNLIKELY(size == capacity))
					return false;

				emplace_helper<POS>(lock, std::forward<Args>(args)...);
				return true;
			}

			template <INSERT_POS POS = TAIL, typename... Args>
			bool wait_emplace(Args &&...args)
			{
				assert(memoryBlock);

				{
					std::unique_lock<std::mutex> lock(dataMutex);

					if (HSLL_LIKELY(size != capacity))
					{
						emplace_helper<POS>(lock, std::forward<Args>(args)...);
						return true;
					}
				}

				wait_element();
				std::unique_lock<std::mutex> lock(dataMutex);

				notFullCond.wait(lock, [this] {
					return HSLL_LIKELY(size != capacity) || HSLL_UNLIKELY(stopped); });

				if (HSLL_UNLIKELY(stopped))
					return false;

				emplace_helper<POS>(lock, std::forward<Args>(args)...);
				return true;
			}

			template <INSERT_POS POS = TAIL, typename Rep, typename Period, typename... Args>
			bool wait_emplace_for(const std::chrono::duration<Rep, Period>& timeout, Args &&...args)
			{
				return wait_emplace_until<POS>(std::chrono::steady_clock::now() + timeout, std::forward<Args>(args)...);
			}

			template <INSERT_POS POS = TAIL, typename Clock, typename Duration, typename... Args>
			bool wait_emplace_until(const std::chrono::time_point<Clock, Duration>& abs, Args &&...args)
			{
				assert(memoryBlock);

				{
					std::unique_lock<std::mutex> lock(dataMutex);

					if (HSLL_LIKELY(size != capacity))
					{
						emplace_helper<POS>(lock, std::forward<Args>(args)...);
						return true;
					}
				}

				wait_element();
				std::unique_lock<std::mutex> lock(dataMutex);

				bool success = notFullCond.wait_until(lock, abs, [this]
					{ return HSLL_LIKELY(size != capacity) || HSLL_UNLIKELY(stopped); });

				if (HSLL_UNLIKELY(!success || stopped))
					return false;

				emplace_helper<POS>(lock, std::forward<Args>(args)...);
				return true;
			}

			template <INSERT_POS POS = TAIL>
			unsigned int enqueue_bulk(TYPE* elements, unsigned int count)
			{
				assert(memoryBlock);
				assert(elements && count);
				std::unique_lock<std::mutex> lock(dataMutex);

				if (HSLL_UNLIKELY(!(capacity - size)))
					return 0;

				return enqueue_bulk_helper<POS>(lock, elements, count);
			}

			template <INSERT_POS POS = TAIL>
			unsigned int enqueue_bulk(TYPE* part1, unsigned int count1, TYPE* part2, unsigned int count2)
			{
				assert(memoryBlock);
				assert(part1 && count1);

				if (!part2 || !count2)
					return enqueue_bulk<POS>(part1, count1);

				std::unique_lock<std::mutex> lock(dataMutex);

				if (HSLL_UNLIKELY(!(capacity - size)))
					return 0;

				return enqueue_bulk_helper<POS>(lock, part1, count1, part2, count2);
			}

			template <INSERT_POS POS = TAIL>
			unsigned int wait_enqueue_bulk(TYPE* elements, unsigned int count)
			{
				assert(memoryBlock);
				assert(elements && count);

				{
					std::unique_lock<std::mutex> lock(dataMutex);

					if (HSLL_LIKELY(size != capacity))
						return enqueue_bulk_helper<POS>(lock, elements, count);
				}

				wait_element();
				std::unique_lock<std::mutex> lock(dataMutex);

				notFullCond.wait(lock, [this]
					{ return HSLL_LIKELY(size != capacity) || HSLL_UNLIKELY(stopped); });

				if (HSLL_UNLIKELY(stopped))
					return 0;

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
				assert(memoryBlock);
				assert(elements && count);

				{
					std::unique_lock<std::mutex> lock(dataMutex);

					if (HSLL_LIKELY(size != capacity))
						return enqueue_bulk_helper<POS>(lock, elements, count);
				}

				wait_element();
				std::unique_lock<std::mutex> lock(dataMutex);

				bool success = notFullCond.wait_until(lock, abs, [this] {
					return HSLL_LIKELY(size != capacity) || HSLL_UNLIKELY(stopped);
					});

				if (HSLL_UNLIKELY(!success || stopped))
					return 0;

				return enqueue_bulk_helper<POS>(lock, elements, count);
			}

			bool dequeue(TYPE& element)
			{
				assert(memoryBlock);
				std::unique_lock<std::mutex> lock(dataMutex);

				if (HSLL_UNLIKELY(!size))
					return false;

				dequeue_helper(lock, element);
				return true;
			}

			bool wait_dequeue(TYPE& element)
			{
				assert(memoryBlock);

				{
					std::unique_lock<std::mutex> lock(dataMutex);

					if (HSLL_LIKELY(size))
					{
						dequeue_helper(lock, element);
						return true;
					}
				}

				wait_element();
				std::unique_lock<std::mutex> lock(dataMutex);

				notEmptyCond.wait(lock, [this]
					{ return HSLL_LIKELY(size) || HSLL_UNLIKELY(stopped); });

				if (HSLL_UNLIKELY(stopped))
					return false;

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
				assert(memoryBlock);

				{
					std::unique_lock<std::mutex> lock(dataMutex);

					if (HSLL_LIKELY(size))
					{
						dequeue_helper(lock, element);
						return true;
					}
				}

				wait_element();
				std::unique_lock<std::mutex> lock(dataMutex);

				bool success = notEmptyCond.wait_until(lock, abs, [this]
					{ return HSLL_LIKELY(size) || HSLL_UNLIKELY(stopped); });

				if (HSLL_UNLIKELY(!success || stopped))
					return false;

				dequeue_helper(lock, element);
				return true;
			}

			unsigned int dequeue_bulk(TYPE* elements, unsigned int count)
			{
				assert(memoryBlock);
				assert(elements && count);
				std::unique_lock<std::mutex> lock(dataMutex);

				if (HSLL_UNLIKELY(!size))
					return 0;

				return dequeue_bulk_helper(lock, elements, count);
			}

			unsigned int wait_dequeue_bulk(TYPE* elements, unsigned int count)
			{
				assert(memoryBlock);
				assert(elements && count);

				{
					std::unique_lock<std::mutex> lock(dataMutex);

					if (HSLL_LIKELY(size))
						return dequeue_bulk_helper(lock, elements, count);
				}

				wait_element();
				std::unique_lock<std::mutex> lock(dataMutex);

				notEmptyCond.wait(lock, [this]
					{ return HSLL_LIKELY(size) || HSLL_UNLIKELY(stopped); });

				if (HSLL_UNLIKELY(stopped))
					return 0;

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
				assert(memoryBlock);
				assert(elements && count);

				{
					std::unique_lock<std::mutex> lock(dataMutex);

					if (HSLL_LIKELY(size))
						return dequeue_bulk_helper(lock, elements, count);
				}

				wait_element();
				std::unique_lock<std::mutex> lock(dataMutex);

				bool success = notEmptyCond.wait_until(lock, abs, [this]
					{ return HSLL_LIKELY(size) || HSLL_UNLIKELY(stopped); });

				if (HSLL_UNLIKELY(!success || stopped))
					return 0;

				return dequeue_bulk_helper(lock, elements, count);
			}

			unsigned int get_size_weak()
			{
				assert(memoryBlock);
				return size;
			}

			unsigned int get_size_strong()
			{
				assert(memoryBlock);
				std::lock_guard<std::mutex> lock(dataMutex);
				return size;
			}

			unsigned int is_stopped_weak()
			{
				assert(memoryBlock);
				return stopped;
			}

			unsigned int is_stopped_strong()
			{
				assert(memoryBlock);
				std::lock_guard<std::mutex> lock(dataMutex);
				return stopped;
			}

			void disableWait()
			{
				assert(memoryBlock);

				{
					std::lock_guard<std::mutex> lock(dataMutex);
					stopped = true;
				}

				notFullCond.notify_all();
				notEmptyCond.notify_all();
			}

			void enableWait()
			{
				assert(memoryBlock);
				std::lock_guard<std::mutex> lock(dataMutex);
				stopped = false;
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

				HSLL_ALIGNED_FREE(memoryBlock);
				stopped = false;
				memoryBlock = nullptr;
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

	using INNER::INSERT_POS;
}

//TPGroupAllocator
namespace HSLL
{
	namespace INNER
	{
		constexpr float HSLL_QUEUE_FULL_FACTOR_MAIN = 0.999f;
		constexpr float HSLL_QUEUE_FULL_FACTOR_OTHER = 0.995f;

		static_assert(HSLL_QUEUE_FULL_FACTOR_MAIN > 0 && HSLL_QUEUE_FULL_FACTOR_MAIN <= 1, "Invalid factors.");
		static_assert(HSLL_QUEUE_FULL_FACTOR_OTHER > 0 && HSLL_QUEUE_FULL_FACTOR_OTHER <= 1, "Invalid factors.");

		template<typename T>
		class RoundRobinGroup
		{
			template<typename TYPE>
			friend class TPGroupAllocator;

			unsigned int nowCount;
			unsigned int nowIndex;
			unsigned int taskThreshold;
			unsigned int mainFullThreshold;
			unsigned int otherFullThreshold;
			std::vector<TPBlockQueue<T>*>* assignedQueues;

			void advance_index() noexcept
			{
				if (nowCount >= taskThreshold)
				{
					nowIndex = (nowIndex + 1) % assignedQueues->size();
					nowCount = 0;
				}
			}

		public:

			void resetAndInit(std::vector<TPBlockQueue<T>*>* queues, unsigned int capacity, unsigned int threshold) noexcept
			{
				nowCount = 0;
				nowIndex = 0;
				this->assignedQueues = queues;
				this->taskThreshold = threshold;
				this->mainFullThreshold = std::max(2u, (unsigned int)(capacity * HSLL_QUEUE_FULL_FACTOR_MAIN));
				this->otherFullThreshold = std::max(2u, (unsigned int)(capacity * HSLL_QUEUE_FULL_FACTOR_OTHER));
			}

			TPBlockQueue<T>* current_queue() noexcept
			{
				TPBlockQueue<T>* queue = (*assignedQueues)[nowIndex];

				if (queue->get_size_weak() <= mainFullThreshold)
					return queue;
				else
					return nullptr;
			}

			TPBlockQueue<T>* available_queue() noexcept
			{
				TPBlockQueue<T>* candidateQueue;

				for (int i = 0; i < assignedQueues->size() - 1; ++i)
				{
					nowIndex = (nowIndex + 1) % assignedQueues->size();
					candidateQueue = (*assignedQueues)[nowIndex];

					if (candidateQueue->get_size_weak() <= otherFullThreshold)
					{
						nowCount = 0;
						return candidateQueue;
					}
				}

				return nullptr;
			}

			void record(unsigned int count) noexcept
			{
				if (assignedQueues->size() == 1)
					return;

				if (count)
				{
					nowCount += count;
					advance_index();
				}
				else
				{
					nowIndex = (nowIndex + 1) % assignedQueues->size();
					nowCount = 0;
					return;
				}
			}
		};

		template<typename T>
		class TPGroupAllocator
		{
			unsigned int capacity;
			unsigned int queueCount;
			unsigned int fullThreshold;
			unsigned int moveThreshold;

			TPBlockQueue<T>* queues;
			std::vector<std::vector<TPBlockQueue<T>*>> threadSlots;
			std::map<std::thread::id, RoundRobinGroup<T>> threadGroups;

			void manage_thread_entry(bool addThread, std::thread::id threadId) noexcept
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
			}

			void rebuild_slot_assignments() noexcept
			{
				if (threadSlots.size())
				{
					for (int i = 0; i < threadSlots.size(); ++i)
						threadSlots[i].clear();

					distribute_queues_to_threads((unsigned int)threadSlots.size());
					reinitialize_groups();
				}
			}

			void reinitialize_groups() noexcept
			{
				if (threadSlots.size())
				{
					unsigned int slotIndex = 0;
					for (auto& group : threadGroups)
					{
						group.second.resetAndInit(&threadSlots[slotIndex], capacity, moveThreshold);
						slotIndex++;
					}
				}
			}

			static unsigned int calculate_balanced_thread_count(unsigned int queueCount, unsigned int threadCount) noexcept
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

			void populate_slot(bool forwardOrder, std::vector<TPBlockQueue<T>*>& slot) noexcept
			{
				if (forwardOrder)
				{
					for (unsigned int k = 0; k < queueCount; ++k)
						slot.emplace_back(queues + k);
				}
				else
				{
					for (unsigned int k = queueCount; k > 0; --k)
						slot.emplace_back(queues + k - 1);
				}
			}

			void handle_remainder_case(unsigned int threadCount) noexcept
			{
				bool fillDirection = false;
				unsigned int balancedCount = calculate_balanced_thread_count(queueCount, threadCount - 1);
				distribute_queues_to_threads(balancedCount);

				for (unsigned int i = 0; i < threadCount - balancedCount; ++i)
				{
					populate_slot(fillDirection, threadSlots[balancedCount + i]);
					fillDirection = !fillDirection;
				}
			}

			void distribute_queues_to_threads(unsigned int threadCount) noexcept
			{
				if (!threadCount)
					return;

				if (threadCount <= queueCount)
				{
					unsigned int queuesPerThread = queueCount / threadCount;
					unsigned int remainder = queueCount % threadCount;

					if (remainder)
					{
						handle_remainder_case(threadCount);
					}
					else
					{
						for (unsigned int i = 0; i < threadCount; ++i)
							for (unsigned int k = 0; k < queuesPerThread; ++k)
								threadSlots[i].emplace_back(queues + i * queuesPerThread + k);
					}
				}
				else
				{
					unsigned int threadsPerQueue = threadCount / queueCount;
					unsigned int remainder = threadCount % queueCount;
					if (remainder)
					{
						handle_remainder_case(threadCount);
					}
					else
					{
						for (unsigned int i = 0; i < threadCount; ++i)
							threadSlots[i].emplace_back(queues + i / threadsPerQueue);
					}
				}

				return;
			}

		public:

			void reset() noexcept
			{
				std::vector<std::vector<TPBlockQueue<T>*>>().swap(threadSlots);
				std::map<std::thread::id, RoundRobinGroup<T>>().swap(threadGroups);
			}

			void initialize(TPBlockQueue<T>* queues, unsigned int queueCount, unsigned int capacity, unsigned int threshold) noexcept
			{
				this->queues = queues;
				this->capacity = capacity;
				this->queueCount = queueCount;
				this->moveThreshold = threshold;
				this->fullThreshold = std::max(2u, (unsigned int)(capacity * HSLL_QUEUE_FULL_FACTOR_MAIN));
			}

			RoundRobinGroup<T>* find(std::thread::id threadId) noexcept
			{
				auto it = threadGroups.find(threadId);

				if (it != threadGroups.end())
					return &(it->second);

				return nullptr;
			}

			TPBlockQueue<T>* available_queue(RoundRobinGroup<T>* group) noexcept
			{
				unsigned int size = (unsigned int)group->assignedQueues->size();

				if (size == queueCount)
					return nullptr;

				std::vector <TPBlockQueue<T>*>& assignedQueues = *group->assignedQueues;
				long long start = (assignedQueues[0] - queues + size) % queueCount;

				for (unsigned int i = 0; i < queueCount - size; ++i)
				{
					TPBlockQueue<T>* queue = queues + (start + i) % queueCount;

					if (queue->get_size_weak() <= fullThreshold)
						return queue;
				}

				return nullptr;
			}

			void register_thread(std::thread::id threadId) noexcept
			{
				manage_thread_entry(true, threadId);
			}

			void unregister_thread(std::thread::id threadId) noexcept
			{
				manage_thread_entry(false, threadId);
			}

			void update(unsigned int newQueueCount) noexcept
			{
				if (this->queueCount != newQueueCount)
				{
					this->queueCount = newQueueCount;
					rebuild_slot_assignments();
				}
			}
		};
	}
}


//ThreadPool
namespace HSLL
{
	namespace INNER
	{
		constexpr unsigned int HSLL_THREADPOOL_LOCK_TIMEOUT_MILLISECONDS = 50u;
		constexpr unsigned int HSLL_THREADPOOL_DEQUEUE_TIMEOUT_MILLISECONDS = 1u;

		static_assert(HSLL_THREADPOOL_LOCK_TIMEOUT_MILLISECONDS > 0, "Invalid lock timeout value.");
		static_assert(HSLL_THREADPOOL_DEQUEUE_TIMEOUT_MILLISECONDS > 0, "Invalid dequeue timeout value.");

		template <typename T>
		class SingleStealer
		{
			template <typename TYPE>
			friend class ThreadPool;
		private:

			unsigned int index;
			unsigned int capacity;
			unsigned int threshold;
			unsigned int threadNum;

			TPBlockQueue<T>* queues;
			TPBlockQueue<T>* ignore;

			SingleStealer(TPBlockQueue<T>* queues, TPBlockQueue<T>* ignore,
				unsigned int capacity, unsigned int threadNum)
			{
				this->index = 0;
				this->capacity = capacity;
				this->threadNum = threadNum;
				this->threshold = std::min(capacity / 2, threadNum / 2);
				this->queues = queues;
				this->ignore = ignore;
			}

			unsigned int steal(T& element)
			{
				return steal_inner(element);
			}

			bool steal_inner(T& element)
			{
				unsigned int num = threadNum;

				for (unsigned int i = 0; i < num; ++i)
				{
					unsigned int now = (index + i) % num;
					TPBlockQueue<T>* queue = queues + now;

					if (queue != ignore && queue->get_size_weak() >= threshold)
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

		template <typename T>
		class BulkStealer
		{
			template <typename TYPE>
			friend class ThreadPool;

		private:

			unsigned int index;
			unsigned int capacity;
			unsigned int batchSize;
			unsigned int threshold;
			unsigned int threadNum;

			TPBlockQueue<T>* queues;
			TPBlockQueue<T>* ignore;

			BulkStealer(TPBlockQueue<T>* queues, TPBlockQueue<T>* ignore, unsigned int capacity,
				unsigned int threadNum, unsigned int batchSize)
			{
				this->index = 0;
				this->batchSize = batchSize;
				this->capacity = capacity;
				this->threadNum = threadNum;
				this->threshold = std::min(capacity / 2, batchSize * threadNum / 2);
				this->queues = queues;
				this->ignore = ignore;
			}

			unsigned int steal(T* elements)
			{
				return steal_inner(elements);
			}

			unsigned int steal_inner(T* elements)
			{
				unsigned int count;
				unsigned int num = threadNum;

				for (unsigned int i = 0; i < num; ++i)
				{
					unsigned int now = (index + i) % num;
					TPBlockQueue<T>* queue = queues + now;

					if (queue != ignore && queue->get_size_weak() >= threshold)
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

		/**
		 * @brief Thread pool implementation with multiple queues for task distribution
		 */
		template <typename T = TaskStack<>>
		class ThreadPool
		{
			static_assert(is_TaskStack<T>::value, "TYPE must be a TaskStack type");

			template <typename TYPE, unsigned int, INSERT_POS POS>
			friend class BatchSubmitter;

		private:

			unsigned int capacity;
			unsigned int batchSize;
			unsigned int threadNum;
			unsigned int mainFullThreshold;
			unsigned int otherFullThreshold;

			T* containers;
			Semaphore* stoppedSem;
			Semaphore* restartSem;
			SpinReadWriteLock rwLock;
			std::atomic<bool> shutdownFlag;
			std::atomic<bool> gracefulShutdown;

			TPBlockQueue<T>* queues;
			std::atomic<unsigned int> index;
			std::vector<std::thread> workers;
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
			bool init(unsigned int capacity, unsigned int threadNum, unsigned int batchSize = 1) noexcept
			{
				assert(!queues);

				if (!batchSize || !threadNum || capacity < 2)
					return false;

				if (!initResourse(capacity, threadNum, batchSize))
					return false;

				this->capacity = capacity;
				this->batchSize = std::min(batchSize, capacity / 2);
				this->threadNum = threadNum;
				this->mainFullThreshold = std::max(2u, (unsigned int)(capacity * HSLL_QUEUE_FULL_FACTOR_MAIN));
				this->otherFullThreshold = std::max(2u, (unsigned int)(capacity * HSLL_QUEUE_FULL_FACTOR_OTHER));
				this->shutdownFlag = false;
				this->gracefulShutdown = true;
				this->index = 0;

				workers.reserve(threadNum);
				groupAllocator.initialize(queues, threadNum, capacity, (unsigned int)(capacity * 0.05 > 1 ? capacity * 0.05 : 1));

				for (unsigned i = 0; i < threadNum; ++i)
					workers.emplace_back(&ThreadPool::worker, this, i);

				return true;
			}

#define HSLL_ENQUEUE_HELPER(exp1,exp2)							\
																\
		assert(queues);											\
																\
		if (threadNum == 1)										\
			return exp1;										\
																\
		ReadLockGuard lock(rwLock);								\
																\
		unsigned int size;										\
		TPBlockQueue<T>* queue;									\
		std::thread::id id = std::this_thread::get_id();		\
		RoundRobinGroup<T>* group = groupAllocator.find(id);	\
																\
		if(!group)												\
		{														\
			queue = select_queue();								\
																\
			if (queue)											\
				return exp2;									\
																\
			return 0;											\
		}														\
																\
		queue = group->current_queue();							\
																\
		if (queue)												\
			size = exp2;										\
		else													\
			size = 0;											\
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
				group->record(size);							\
			}													\
			else												\
			{													\
				queue = groupAllocator.available_queue(group);	\
																\
				if(queue)										\
				return exp2;									\
			}													\
		}														\
																\
		return size;											

			/**
			 * @brief Non-blocking task submission to the thread pool
			 * @tparam POS Insertion position (HEAD or TAIL, default: TAIL)
			 * @param args Arguments forwarded to task constructor
			 * @return true if task was successfully added, false otherwise
			 * @note
			 * Supports two argument structures:
			 * 1. TaskStack object (must be passed by rvalue reference, using move semantics)
			 * 2. Callable object (function pointer/lambda/functor...) + bound arguments
			 */
			template <INSERT_POS POS = TAIL, typename... Args>
			bool submit(Args &&...args) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(queues-> template emplace<POS>(std::forward<Args>(args)...)),
					(queue-> template emplace<POS>(std::forward<Args>(args)...))
				)
			}

			/**
			 * @brief Blocking task submission with indefinite wait
			 * @tparam POS Insertion position (HEAD or TAIL, default: TAIL)
			 * @param args Arguments forwarded to task constructor
			 * @return true if task was added successfully, false if thread pool was stopped
			 * @note
			 * Supports two argument structures:
			 * 1. TaskStack object (must be passed by rvalue reference, using move semantics)
			 * 2. Callable object (function pointer/lambda/functor...) + bound arguments
			 */
			template <INSERT_POS POS = TAIL, typename... Args>
			bool wait_submit(Args &&...args) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(queues-> template wait_emplace<POS>(std::forward<Args>(args)...)),
					(queue-> template wait_emplace<POS>(std::forward<Args>(args)...))
				)
			}

			/**
			 * @brief Blocking task submission with timeout
			 * @tparam POS Insertion position (HEAD or TAIL, default: TAIL)
			 * @param timeout Maximum duration to wait for space
			 * @param args Arguments forwarded to task constructor
			 * @return true if task was added successfully, false on timeout or thread pool stop
			 * @note
			 * Supports two argument structures:
			 * 1. TaskStack object (must be passed by rvalue reference, using move semantics)
			 * 2. Callable object (function pointer/lambda/functor...) + bound arguments
			 */
			template <INSERT_POS POS = TAIL, typename Rep, typename Period, typename... Args>
			bool wait_submit_for(const std::chrono::duration<Rep, Period>& timeout, Args &&...args) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(queues-> template wait_emplace_for<POS>(timeout, std::forward<Args>(args)...)),
					(queue-> template wait_emplace_for<POS>(timeout, std::forward<Args>(args)...))
				)
			}

			/**
			 * @brief Blocking task submission with absolute timeout
			 * @tparam POS Insertion position (HEAD or TAIL, default: TAIL)
			 * @param abs Absolute timeout point
			 * @param args Arguments forwarded to task constructor
			 * @return true if task was added successfully, false on timeout or thread pool stop
			 * @note
			 * Supports two argument structures:
			 * 1. TaskStack object (must be passed by rvalue reference, using move semantics)
			 * 2. Callable object (function pointer/lambda/functor...) + bound arguments
			 */
			template <INSERT_POS POS = TAIL, typename Clock, typename Duration, typename... Args>
			bool wait_submit_until(const std::chrono::time_point<Clock, Duration>& abs, Args &&...args)
			{
				HSLL_ENQUEUE_HELPER(
					(queues-> template wait_emplace_until<POS>(abs, std::forward<Args>(args)...)),
					(queue-> template wait_emplace_until<POS>(abs, std::forward<Args>(args)...))
				)
			}

			/**
			 * @brief Non-blocking bulk task submission (using move semantics)
			 * @tparam POS Insertion position (HEAD or TAIL, default: TAIL)
			 * @param tasks Array of tasks to enqueue
			 * @param count Number of tasks in array (must be > 0)
			 * @return Actual number of tasks added (may be less than count)
			 */
			template <INSERT_POS POS = TAIL>
			unsigned int submit_bulk(T* tasks, unsigned int count) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(queues-> template enqueue_bulk<POS>(tasks, count)),
					(queue-> template enqueue_bulk<POS>(tasks, count))
				)
			}

			/**
			 * @brief Blocking bulk submission with indefinite wait (using move semantics)
			 * @tparam POS Insertion position (HEAD or TAIL, default: TAIL)
			 * @param tasks Array of tasks to add
			 * @param count Number of tasks to add (must be > 0)
			 * @return Actual number of tasks added (may be less than count)
			 */
			template <INSERT_POS POS = TAIL>
			unsigned int wait_submit_bulk(T* tasks, unsigned int count) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(queues-> template wait_enqueue_bulk<POS>(tasks, count)),
					(queue-> template wait_enqueue_bulk<POS>(tasks, count))
				)
			}

			/**
			 * @brief Blocking bulk submission with timeout (using move semantics)
			 * @tparam POS Insertion position (HEAD or TAIL, default: TAIL)
			 * @param timeout Maximum duration to wait for space
			 * @param tasks Array of tasks to add
			 * @param count Number of tasks to add (must be > 0)
			 * @return Actual number of tasks added (may be less than count)
			 */
			template <INSERT_POS POS = TAIL, typename Rep, typename Period>
			unsigned int wait_submit_bulk_for(const std::chrono::duration<Rep, Period>& timeout, T* tasks, unsigned int count) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(queues-> template wait_enqueue_bulk_for<POS>(timeout, tasks, count)),
					(queue-> template wait_enqueue_bulk_for<POS>(timeout, tasks, count))
				)
			}

			/**
			 * @brief Blocking bulk submission with absolute timeout (using move semantics)
			 * @tparam POS Insertion position (HEAD or TAIL, default: TAIL)
			 * @param abs Absolute timeout point
			 * @param tasks Array of tasks to add
			 * @param count Number of tasks to add (must be > 0)
			 * @return Actual number of tasks added (may be less than count)
			 */
			template <INSERT_POS POS = TAIL, typename Clock, typename Duration>
			unsigned int wait_submit_bulk_until(const std::chrono::time_point<Clock, Duration>& abs, T* tasks, unsigned int count)
			{
				HSLL_ENQUEUE_HELPER(
					(queues-> template wait_enqueue_bulk_until<POS>(abs, tasks, count)),
					(queue-> template wait_enqueue_bulk_until<POS>(abs, tasks, count))
				)
			}

			/**
			 * @brief Waits for all tasks to complete.
			 * @note
			 *  1. During the join operation, adding any new tasks is prohibited.
			 *  2. This function is not thread-safe.
			 *	3. This function does not clean up resources. After the call, the queue can be used normally.
			 */
			void drain() noexcept
			{
				assert(queues);

				for (unsigned int i = 0; i < threadNum; ++i)
				{
					restartSem[i].release();
					queues[i].disableWait();
				}

				for (unsigned int i = 0; i < threadNum; ++i)
				{
					stoppedSem[i].acquire();
					queues[i].enableWait();
				}
			}

			/**
			 * @brief Stops all workers and releases resources.
			 * @param graceful If true, performs a graceful shutdown (waits for tasks to complete);
			 *                if false, forces an immediate shutdown.
			 * @note This function is not thread-safe.
			 * @note After calling this function, the thread pool can be reused by calling init again.
			 */
			void shutdown(bool graceful = true) noexcept
			{
				assert(queues);

				this->shutdownFlag = true;
				this->gracefulShutdown = graceful;

				{
					for (unsigned i = 0; i < workers.size(); ++i)
						restartSem[i].release();

					for (unsigned i = 0; i < threadNum; ++i)
						queues[i].disableWait();

					for (auto& worker : workers)
						worker.join();
				}

				releaseResourse();
			}

			/**
			 * @brief Registers the current thread. Registered threads participate in queue grouping and obtain a dedicated queue group.
			 * @note
			 *   1. The registered thread must be a producer thread
			 *   2. Production capacity between registered threads should not vary significantly
			 *   3. If the thread pool will continue to be used after this thread exits, you MUST unregister
			 *      the thread before exit to allow queue reallocation
			 */
			void register_this_thread() noexcept
			{
				assert(queues);
				std::thread::id id = std::this_thread::get_id();
				WriteLockGuard lock(rwLock);
				groupAllocator.register_thread(id);
			}

			/**
			 * @brief Unregisters the current thread. It will no longer participate in queue grouping.
			 */
			void unregister_this_thread() noexcept
			{
				assert(queues);
				std::thread::id id = std::this_thread::get_id();
				WriteLockGuard lock(rwLock);
				groupAllocator.unregister_thread(id);
			}

			~ThreadPool() noexcept
			{
				if (queues)
					shutdown(false);
			}

			ThreadPool(const ThreadPool&) = delete;
			ThreadPool& operator=(const ThreadPool&) = delete;
			ThreadPool(ThreadPool&&) = delete;
			ThreadPool& operator=(ThreadPool&&) = delete;

		private:

			template <INSERT_POS POS = TAIL>
			unsigned int submit_bulk(T* part1, unsigned int count1, T* part2, unsigned int count2) noexcept
			{
				HSLL_ENQUEUE_HELPER(
					(queues-> template enqueue_bulk<POS>(part1, count1, part2, count2)),
					(queue-> template enqueue_bulk<POS>(part1, count1, part2, count2))
				)
			}

			unsigned int next_index() noexcept
			{
				return index.fetch_add(1, std::memory_order_relaxed) % threadNum;
			}

			TPBlockQueue<T>* select_queue() noexcept
			{
				unsigned int now = next_index();
				TPBlockQueue<T>* queue = queues + now;

				if (queue->get_size_weak() <= mainFullThreshold)
					return queue;

				for (unsigned int i = 1; i <= threadNum - 1; ++i)
				{
					queue = queues + ((now + i) % threadNum);

					if (queue->get_size_weak() <= otherFullThreshold)
						return queue;
				}

				return nullptr;
			}

			void worker(unsigned int index) noexcept
			{
				if (batchSize == 1)
					process_single(queues + index, index);
				else
					process_bulk(queues + index, index);
			}

			static void execute_tasks(T* tasks, unsigned int count)
			{
				for (unsigned int i = 0; i < count; ++i)
				{
					tasks[i].execute();
					tasks[i].~T();
				}
			}

			void process_single(TPBlockQueue<T>* queue, unsigned int index) noexcept
			{
				bool enableSteal = (threadNum != 1);
				T* task = containers + index * batchSize;
				SingleStealer<T> stealer(queues, queue, capacity, threadNum);

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

						if (queue->wait_dequeue_for(std::chrono::milliseconds(HSLL_THREADPOOL_DEQUEUE_TIMEOUT_MILLISECONDS), *task))
						{
							task->execute();
							task->~T();
						}

					cheak:

						if (queue->is_stopped_weak())
							break;
					}

					if (gracefulShutdown)
					{
						while (queue->dequeue(*task))
						{
							task->execute();
							task->~T();
						}
					}

					stoppedSem[index].release();
					restartSem[index].acquire();

					if (shutdownFlag)
						break;
				}
			}

			void process_bulk(TPBlockQueue<T>* queue, unsigned int index) noexcept
			{
				bool enableSteal = (threadNum != 1);
				T* tasks = containers + index * batchSize;
				BulkStealer<T> stealer(queues, queue, capacity, threadNum, batchSize);

				while (true)
				{
					unsigned int count;

					while (true)
					{
						while (true)
						{
							unsigned int size = queue->get_size_weak();
							unsigned int round = batchSize;

							while (round && size < batchSize)
							{
								std::this_thread::yield();
								size = queue->get_size_weak();
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

						if (count = queue->wait_dequeue_bulk_for(std::chrono::milliseconds(HSLL_THREADPOOL_DEQUEUE_TIMEOUT_MILLISECONDS),
							tasks, batchSize))
							execute_tasks(tasks, count);

					cheak:

						if (queue->is_stopped_weak())
							break;
					}

					if (gracefulShutdown)
					{
						while (count = queue->dequeue_bulk(tasks, batchSize))
							execute_tasks(tasks, count);
					}

					stoppedSem[index].release();
					restartSem[index].acquire();

					if (shutdownFlag)
						break;
				}
			}

			bool initResourse(unsigned int capacity, unsigned int maxThreadNum, unsigned int batchSize) noexcept
			{
				unsigned int succeed = 0;

				if (!(restartSem = new(std::nothrow) Semaphore[2 * maxThreadNum]))
					goto clean_1;

				stoppedSem = restartSem + maxThreadNum;

				if (!(containers = (T*)HSLL_ALIGNED_MALLOC(sizeof(T) * batchSize * maxThreadNum, alignof(T))))
					goto clean_2;

				if (!(queues = (TPBlockQueue<T>*)HSLL_ALIGNED_MALLOC(maxThreadNum * sizeof(TPBlockQueue<T>), alignof(TPBlockQueue<T>))))
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

				HSLL_ALIGNED_FREE(queues);
				queues = nullptr;

			clean_2:

				HSLL_ALIGNED_FREE(containers);

			clean_1:

				delete[] restartSem;

				return false;
			}

			void releaseResourse() noexcept
			{
				for (unsigned i = 0; i < threadNum; ++i)
					queues[i].~TPBlockQueue<T>();

				HSLL_ALIGNED_FREE(queues);
				HSLL_ALIGNED_FREE(containers);
				delete[] restartSem;
				queues = nullptr;
				workers.clear();
				workers.shrink_to_fit();
				groupAllocator.reset();
			}
		};

		template <typename T, unsigned int BATCH, INSERT_POS POS = TAIL>
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
			BatchSubmitter(ThreadPool<T>& pool) noexcept : size(0), index(0), elements((T*)buf), pool(pool) {
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
			 * @param args Arguments forwarded to task constructor
			 * @return true if task was added to buffer (or submitted successfully when buffer full),
			 *         false if buffer was full and submission failed (task not added)
			 * @note
			 * Supports two argument structures:
			 * 1. TaskStack object (must be passed by rvalue reference, using move semantics)
			 * 2. Callable object (function pointer/lambda/functor...) + bound arguments
			 *
			 * @details
			 *   - If buffer not full: adds task to buffer
			 *   - If buffer full:
			 *       1. First attempts to submit full batch
			 *       2. Only if submission succeeds, adds new task to buffer
			 *   - Returns false only when submission of full batch fails
			 */
			template <typename... Args>
			bool add(Args &&...args) noexcept
			{
				if (!check_and_submit())
					return false;

				new (elements + index) T(std::forward<Args>(args)...);
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
						submitted = pool.template submit<POS>(std::move(*(elements + start))) ? 1 : 0;
					else
						submitted = pool.template submit_bulk<POS>(elements + start, len1);
				}
				else
				{
					submitted = pool.template submit_bulk<POS>(
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

	using INNER::ThreadPool;
	using INNER::BatchSubmitter;
}

#endif