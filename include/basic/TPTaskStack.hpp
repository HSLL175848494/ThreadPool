#ifndef HSLL_TPTASKSTACK
#define HSLL_TPTASKSTACK

// The current function may throw exceptions, including std::bad_alloc.
#define HSLL_MAY_THROW

//For a valid object, the current function can only be called once.
#define HSLL_CALL_ONCE

namespace HSLL
{
	namespace INNER
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

		//helper5_function_traits
		template <typename T>
		struct function_traits;

		template <typename Ret, typename... Args>
		struct function_traits<Ret(*)(Args...)>
		{
			using return_type = Ret;
			using args_tuple = std::tuple<typename std::decay<Args>::type...>;
			static constexpr bool is_member_function = false;
		};

		template <typename Ret, typename... Args>
		struct function_traits<Ret(&)(Args...)>
		{
			using return_type = Ret;
			using args_tuple = std::tuple<typename std::decay<Args>::type...>;
			static constexpr bool is_member_function = false;
		};

		template <typename Class, typename Ret, typename... Args>
		struct function_traits<Ret(Class::*)(Args...)>
		{
			using return_type = Ret;
			using args_tuple = std::tuple<typename std::decay<Args>::type...>;
			static constexpr bool is_member_function = true;
		};

		template <typename Class, typename Ret, typename... Args>
		struct function_traits<Ret(Class::*)(Args...) const>
		{
			using return_type = Ret;
			using args_tuple = std::tuple<typename std::decay<Args>::type...>;
			static constexpr bool is_member_function = true;
		};

		template <typename Functor>
		struct function_traits
		{
		private:
			using call_type = function_traits<decltype(&Functor::operator())>;
		public:
			using return_type = typename call_type::return_type;
			using args_tuple = typename call_type::args_tuple;
			static constexpr bool is_member_function = false;
		};

		template <typename Callable>
		using function_args = typename function_traits<typename std::decay<Callable>::type>::args_tuple;

		template <typename Callable>
		using function_rtype = typename function_traits<typename std::decay<Callable>::type>::return_type;

		//helper6_invoke
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
			template <class ResultType, typename Callable,
				typename std::enable_if<std::is_void<ResultType>::value, bool>::type = true,
				typename std::enable_if<!function_traits<Callable>::is_member_function, bool>::type = true,
				typename... Ts>
			static void invoke(Callable& callable, Ts &...args)
			{
				callable(args...);
			}

			template <class ResultType, typename Callable,
				typename std::enable_if<!std::is_void<ResultType>::value, bool>::type = true,
				typename std::enable_if<!function_traits<Callable>::is_member_function, bool>::type = true,
				typename... Ts>
			static ResultType invoke(Callable& callable, Ts &...args)
			{
				return callable(args...);
			}

			template <class ResultType, typename Callable, typename OBJ,
				typename std::enable_if<std::is_void<ResultType>::value, bool>::type = true,
				typename std::enable_if<function_traits<Callable>::is_member_function, bool>::type = true,
				typename std::enable_if<std::is_pointer<OBJ>::value, bool>::type = true,
				typename... Ts>
			static void invoke(Callable& callable, OBJ& obj, Ts&... args)
			{
				(obj->*callable)(args...);
			}

			template <class ResultType, typename Callable, typename OBJ,
				typename std::enable_if<!std::is_void<ResultType>::value, bool>::type = true,
				typename std::enable_if<function_traits<Callable>::is_member_function, bool>::type = true,
				typename std::enable_if<std::is_pointer<OBJ>::value, bool>::type = true,
				typename... Ts>
			static ResultType invoke(Callable& callable, OBJ& obj, Ts&... args)
			{
				return (obj->*callable)(args...);
			}

			template <class ResultType, typename Callable, typename OBJ,
				typename std::enable_if<std::is_void<ResultType>::value, bool>::type = true,
				typename std::enable_if<function_traits<Callable>::is_member_function, bool>::type = true,
				typename std::enable_if<!std::is_pointer<OBJ>::value, bool>::type = true,
				typename std::enable_if<!is_reference_wrapper<OBJ>::value, bool>::type = true,
				typename... Ts>
			static void invoke(Callable& callable, OBJ& obj, Ts&... args)
			{
				(obj.*callable)(args...);
			}

			template <class ResultType, typename Callable, typename OBJ,
				typename std::enable_if<!std::is_void<ResultType>::value, bool>::type = true,
				typename std::enable_if<function_traits<Callable>::is_member_function, bool>::type = true,
				typename std::enable_if<!std::is_pointer<OBJ>::value, bool>::type = true,
				typename std::enable_if<!is_reference_wrapper<OBJ>::value, bool>::type = true,
				typename... Ts>
			static ResultType invoke(Callable& callable, OBJ& obj, Ts&... args)
			{
				return (obj.*callable)(args...);
			}

			template <class ResultType, typename Callable, typename OBJ,
				typename std::enable_if<std::is_void<ResultType>::value, bool>::type = true,
				typename std::enable_if<function_traits<Callable>::is_member_function, bool>::type = true,
				typename std::enable_if<!std::is_pointer<OBJ>::value, bool>::type = true,
				typename std::enable_if<is_reference_wrapper<OBJ>::value, bool>::type = true,
				typename... Ts>
			static void invoke(Callable& callable, OBJ& obj, Ts&... args)
			{
				(obj.get().*callable)(args...);
			}

			template <class ResultType, typename Callable, typename OBJ,
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
			template <class ResultType, typename Callable, typename... Ts>
			static void invoke(Callable& callable, Ts &...args)
			{
				Invoker<TASK_TUPLE_TYPE_BASE>::invoke<ResultType>(callable, args...);
			}
		};

		template<>
		struct Invoker<TASK_TUPLE_TYPE_ASYNC>
		{
			template <class ResultType, typename std::enable_if<std::is_void<ResultType>::value, bool>::type = true,
				typename Promise, typename Callable, typename... Ts>
			static void invoke(Promise& promise, Callable& callable, Ts &...args)
			{
				Invoker<TASK_TUPLE_TYPE_BASE>::invoke<ResultType>(callable, args...);
			}

			template <class ResultType, typename std::enable_if<!std::is_void<ResultType>::value, bool>::type = true,
				typename Promise, typename Callable, typename... Ts>
			static ResultType invoke(Promise& promise, Callable& callable, Ts &...args)
			{
				return Invoker<TASK_TUPLE_TYPE_BASE>::invoke<ResultType>(callable, args...);
			}
		};

		template<>
		struct Invoker<TASK_TUPLE_TYPE_CANCELABLE>
		{
			template <class ResultType, typename std::enable_if<std::is_void<ResultType>::value, bool>::type = true,
				typename Promise, typename Flag, typename Callable, typename... Ts>
			static void invoke(Promise& promise, Flag& flag, Callable& callable, Ts &...args)
			{
				Invoker<TASK_TUPLE_TYPE_BASE>::invoke<ResultType>(callable, args...);
			}

			template <class ResultType, typename std::enable_if<!std::is_void<ResultType>::value, bool>::type = true,
				typename Promise, typename Flag, typename Callable, typename... Ts>
			static ResultType invoke(Promise& promise, Flag& flag, Callable& callable, Ts &...args)
			{
				return Invoker<TASK_TUPLE_TYPE_BASE>::invoke<ResultType>(callable, args...);
			}
		};

		//helper7_apply
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

		//helper8_make_unique
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
			template<class Func, typename std::enable_if<!is_HeapCallable<typename std::decay<Func>::type>::value, int>::type = 0 >
			HeapCallable(Func&& func, Args &&...args) HSLL_MAY_THROW
				: storage(HSLL::INNER::make_unique<Package>(std::forward<Func>(func), std::forward<Args>(args)...)) {}

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
			using ReturnType = function_rtype<F>;
			using Package = std::tuple<std::promise<ReturnType>, typename std::decay<F>::type, typename std::decay<Args>::type...>;

		protected:
			std::unique_ptr<Package> storage;

			template<class T = ReturnType, typename std::enable_if<std::is_void<T>::value, bool>::type = true>
			void invoke()
			{
				auto& promise = std::get<0>(*storage);
				try
				{
					tuple_apply<TASK_TUPLE_TYPE_ASYNC, ReturnType>(*storage);
					promise.set_value();
				}
				catch (...)
				{
					promise.set_exception(std::current_exception());
				}
			}

			template<class T = ReturnType, typename std::enable_if<!std::is_void<T>::value, bool>::type = true>
			void invoke()
			{
				auto& promise = std::get<0>(*storage);
				try
				{
					promise.set_value(tuple_apply<TASK_TUPLE_TYPE_ASYNC, ReturnType>(*storage));
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
			HeapCallable_Async(Func&& func, Args &&...args) HSLL_MAY_THROW
				: storage(HSLL::INNER::make_unique<Package>(std::promise<ReturnType>(), std::forward<Func>(func), std::forward<Args>(args)...)) {}

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
			std::future<ReturnType> get_future() HSLL_CALL_ONCE
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
			void wait()  const
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
			void set_value() HSLL_CALL_ONCE
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
			void set_value(U&& value) HSLL_CALL_ONCE
			{
				promise.set_value(std::forward<T>(value));
			}

			/**
			 * @brief Stores exception in promise
			 * @param e Exception pointer to store
			 * @pre Must be in critical section (via successful enter())
			 * @throws std::future_error if exception already set or not in critical section
			 */
			void set_exception(std::exception_ptr e) HSLL_CALL_ONCE
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
			using ReturnType = function_rtype<F>;
			using Package = std::tuple<std::promise<ReturnType>, CancelableFlag,
				typename std::decay<F>::type, typename std::decay<Args>::type...>;

		private:
			std::shared_ptr<Package> storage;

			template<class T = ReturnType, typename std::enable_if<std::is_void<T>::value, bool>::type = true>
			void invoke()
			{
				auto& promise = std::get<0>(*storage);
				try
				{
					tuple_apply<TASK_TUPLE_TYPE_CANCELABLE, ReturnType>(*storage);
					promise.set_value();
				}
				catch (...)
				{
					promise.set_exception(std::current_exception());
				}
			}

			template<class T = ReturnType, typename std::enable_if<!std::is_void<T>::value, bool>::type = true>
			void invoke()
			{
				auto& promise = std::get<0>(*storage);
				try
				{
					promise.set_value(tuple_apply<TASK_TUPLE_TYPE_CANCELABLE, ReturnType>(*storage));
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

				std::future<ReturnType> future;
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
				template <typename U = ReturnType>
				typename std::enable_if<!std::is_void<U>::value, U>::type get() HSLL_CALL_ONCE
				{
					return future.get();
				}

				/**
				 * @brief Synchronizes with task completion (void specialization)
				 * @throws Propagates any exception stored in the promise
				 * @throws std::runtime_error("Task canceled") if canceled
				 */
				template <typename U = ReturnType>
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
			};

			/**
			 * @brief Constructs a cancelable callable object
			 * @param func Callable object to store
			 * @param args Arguments to bind to the callable
			 */
			template<class Func, typename std::enable_if<!is_HeapCallable_Cancelable<typename std::decay<Func>::type>::value, int>::type = 0 >
			HeapCallable_Cancelable(Func&& func, Args &&...args) HSLL_MAY_THROW
				: storage(std::make_shared<Package>(std::promise<ReturnType>(), false, std::forward<Func>(func), std::forward<Args>(args)...)) {}

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
		template <class... Args>
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
				new (dst) TaskImpl(move_or_copy(args)...);
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

			void execute() override
			{
				invoke<sizeof...(Args) == 1>();
			}

			void moveTo(void* dst) override
			{
				tuple_move(dst);
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


			template <bool Condition, typename Any, typename... Params>
			typename std::enable_if<Condition, void>::type
				construct(Any&& func, Params &&...params)//normal
			{
				using ImplType = typename TaskImplTraits<Any, Params...>::type;
				new (storage) ImplType(std::forward<Any>(func), std::forward<Params>(params)...);
			}

			template <bool Condition, typename Any, typename... Params>
			typename std::enable_if<!Condition, void>::type
				construct(Any&& func, Params &&...params)//HeapCallable
			{
				using ImplType = typename TaskImplTraits<HeapCallable<Any, Params...>>::type;
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
			template <class F, class... Args,
				typename std::enable_if<!is_TaskStack<typename std::decay<F>::type>::value, int>::type = 0>
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

	using INNER::Cancelable;
	using INNER::HeapCallable;
	using INNER::HeapCallable_Async;
	using INNER::HeapCallable_Cancelable;
	using INNER::make_callable;
	using INNER::make_callable_async;
	using INNER::make_callable_cancelable;
	using INNER::TaskImplTraits;
	using INNER::TaskStack;
}

#endif // !HSLL_TPTASKSTACK