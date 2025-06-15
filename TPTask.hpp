#ifndef HSLL_TPTASK
#define HSLL_TPTASK

#include <new>      
#include <tuple>      
#include <memory>
#include <cstdio>  
#include <cstdlib>    
#include <cstddef>   
#include <utility>   
#include <type_traits>  

#define HSLL_ALLOW_THROW

namespace HSLL
{

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

		template  <typename U, typename = decltype(U{ std::declval<U&>() }) >
		static constexpr  std::true_type test_copy(bool);

		template <typename>
		static constexpr  std::false_type test_copy(...);

	public:

		static constexpr bool value = decltype(test_copy<T>(true))::value;
	};

	template <unsigned int TSIZE, unsigned int ALIGN>
	class TaskStack;

	template <class F, class... Args>
	class HeapCallable;

	template <unsigned int TSIZE, unsigned int ALIGN>
	class TaskStack;

	template <typename T>
	struct is_generic_hc : std::false_type
	{
	};

	template <class T, class... Params>
	struct is_generic_hc<HeapCallable<T, Params...>> : std::true_type
	{
	};

	template <typename T>
	struct is_generic_ts : std::false_type
	{
	};

	template <unsigned int S, unsigned int A>
	struct is_generic_ts<TaskStack<S, A>> : std::true_type
	{
	};

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
	struct make_index_sequence {
		using type = typename make_index_sequence_impl<N>::type;
	};

	template <bool...>
	struct bool_pack;

	template <bool... Bs>
	using all_true = std::is_same<bool_pack<true, Bs...>, bool_pack<Bs..., true>>;

	template <typename... Ts>
	using are_all_move_constructible = all_true<is_move_constructible<Ts>::value...>;

	template <typename... Ts>
	using are_all_copy_constructible = all_true<is_copy_constructible<Ts>::value...>;


	template <typename Callable, typename... Ts>
	static void tinvoke(Callable& callable, Ts &...args)
	{
		callable(args...);
	}

	template <typename Tuple, size_t... Is>
	void apply_impl(Tuple& tup, index_sequence<Is...>)
	{
		tinvoke(std::get<Is>(tup)...);
	}

	template <typename Tuple>
	void tuple_apply(Tuple& tup)
	{
		apply_impl(tup, typename make_index_sequence<std::tuple_size<Tuple>::value>::type{});
	}

	/**
	 * @brief Heap-allocated type-erased callable wrapper with shared ownership
	 * @tparam F Type of callable object
	 * @tparam Args Types of bound arguments
	 * @details
	 * - Stores decayed copies of function and arguments in a shared tuple
	 * - Supports copy/move operations through shared reference counting
	 * - Invocation always passes arguments as lvalues (same as TaskStack)
	 * - Safe for cross-thread usage through shared_ptr semantics
	 */
	template <class F, class... Args>
	class HeapCallable
	{
	private:

		// Storage for decayed function and arguments with shared ownership
		using Package = std::tuple<typename std::decay<F>::type, typename std::decay<Args>::type...>;

		std::shared_ptr<Package> storage;

	public:
		~HeapCallable() = default;
		HeapCallable(const HeapCallable&) = default;
		HeapCallable& operator=(const HeapCallable&) = default;
		HeapCallable(HeapCallable&&) noexcept = default;
		HeapCallable& operator=(HeapCallable&&) noexcept = default;

		/**
		 * @brief Executes stored callable with bound arguments
		 * @note Arguments are always passed as lvalues during invocation
		 * @note No-throw guarantee: exceptions propagate to caller
		 */
		void operator()() noexcept
		{
			if (storage)
				tuple_apply(*storage);
		}

		/**
		 * @brief Constructs callable with function and arguments
		 * @tparam F Forwarding reference to callable type
		 * @tparam Args Forwarding references to argument types
		 * @param func Callable target function
		 * @param args Arguments to bind to function call
		 * @note Disables overload when F is HeapCallable type (prevents nesting)
		 * @note Arguments are stored as decayed types (copy/move constructed)
		 */
		template <typename std::enable_if<!is_generic_hc<typename std::decay<F>::type>::value, int>::type = 0>
		HeapCallable(F&& func, Args &&...args) HSLL_ALLOW_THROW
			: storage(std::make_shared<Package>(std::forward<F>(func), std::forward<Args>(args)...)) {}
	};

	/**
	 * @brief Factory function to create HeapCallable objects
	 * @tparam F Type of callable object
	 * @tparam Args Types of arguments to bind
	 * @param func Callable target function
	 * @param args Arguments to bind to function call
	 * @return HeapCallable instance managing shared ownership of the callable
	 */
	template <typename F,
		typename std::enable_if<!is_generic_hc<typename std::decay<F>::type>::value, int>::type = 0,
		typename... Args>
	HeapCallable<F, Args...> make_callable(F&& func, Args &&...args) HSLL_ALLOW_THROW
	{
		return HeapCallable<F, Args...>(std::forward<F>(func), std::forward<Args>(args)...);
	}

	/**
	 * @brief Base interface for type-erased task objects
	 * @details Provides virtual methods for task execution and storage management
	 */
	struct TaskBase
	{
		virtual ~TaskBase() = default;
		virtual void execute() noexcept = 0;					  ///< Executes the stored task
		virtual void copyTo(void* memory) const  noexcept = 0; ///< Copies task to preallocated memory
		virtual void moveTo(void* memory) noexcept = 0;		  ///< Moves task to preallocated memory
		virtual void move(void* memory) noexcept = 0;		  ///< Moves task to preallocated memory,¿ÉÍË»¯Îª¿½±´
	};

	/**
	 * @brief Concrete task implementation storing function and arguments
	 * @tparam F Type of callable object
	 * @tparam Args Types of bound arguments
	 * @details Stores decayed copies of function and arguments in a tuple
	 */
	template <class F, class... Args>
	struct TaskImpl : TaskBase
	{
		template <typename T, bool Copyable>
		struct CloneHelper;

		/**
		 * @brief Specialization for copyable types
		 * @details Performs copy construction in target memory
		 */
		template <typename T>
		struct CloneHelper<T, true>
		{
			static void copyTo(const T* self, void* memory)  noexcept
			{
				new (memory) T(*self);
			}
		};

		/**
		 * @brief Specialization for non-copyable types
		 * @details Aborts program with error message
		 */
		template <typename T>
		struct CloneHelper<T, false>
		{
			static void copyTo(const T*, void*)  noexcept
			{
				printf("\nTaskImpl must be copy constructible for cloneTo()");
				std::abort();
			}
		};

		template <typename T, bool Movable>
		struct MoveHelper;

		/**
		 * @brief Specialization for movable types
		 * @details Performs move construction in target memory
		 */
		template <typename T>
		struct MoveHelper<T, true>
		{
			static void moveTo(T* self, void* memory)  noexcept
			{
				new (memory) T(std::move(*self));
			}

			static void move(T* self, void* memory)  noexcept
			{
				new (memory) T(std::move(*self));
			}
		};

		/**
		 * @brief Specialization for non-movable types
		 * @details Aborts program with error message
		 */
		template <typename T>
		struct MoveHelper<T, false>
		{
			static void moveTo(T*, void*)  noexcept
			{
				printf("\nTaskImpl must be move constructible for moveTo()");
				std::abort();
			}

			static void move(const T* self, void* memory)  noexcept
			{
				new (memory) T(*self);
			}
		};

		using Tuple = std::tuple<typename std::decay<F>::type, typename std::decay<Args>::type...>;
		Tuple  storage;

		/**
		 * @brief Constructs task with function and arguments
		 * @param func Callable target function
		 * @param args Arguments to bind to function call
		 */
		TaskImpl(F&& func, Args &&...args)
			: storage(std::forward<F>(func), std::forward<Args>(args)...)
		{
		}

		/**
		 * @brief Executes stored task with bound arguments
		 * @details Unpacks tuple and invokes stored callable
		 * @note Arguments are ALWAYS passed as lvalues during invocation
		 *       regardless of original construction value category
		 */
		void execute() noexcept override
		{
			tuple_apply(storage);
		}

		/**
		 * @brief Copies task to preallocated memory
		 * @param memory Preallocated storage for copy
		 * @note Uses CloneHelper to handle copyability
		 */
		void copyTo(void* memory) const   noexcept override
		{
			CloneHelper<TaskImpl,
				are_all_copy_constructible<typename std::decay<F>::type,
				typename std::decay<Args>::type...>::value>::copyTo(this, memory);
		}

		/**
		 * @brief Moves task to preallocated memory
		 * @param memory Preallocated storage for moved object
		 * @note Uses MoveHelper to handle movability
		 */
		void moveTo(void* memory) noexcept  override
		{
			MoveHelper<TaskImpl,
				are_all_move_constructible< typename std::decay<F>::type,
				typename std::decay<Args>::type...>::value > ::moveTo(this, memory);
		}

		void move(void* memory) noexcept override
		{
			MoveHelper<TaskImpl,
				are_all_move_constructible<typename std::decay<F>::type,
				typename std::decay<Args>::type...>::value > ::move(this, memory);
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
		template <class TYPE>
		friend class TPBlockQueue;

		static_assert(TSIZE >= 2 * sizeof(void*), "TSIZE must >= 2 * sizeof(void*)");
		static_assert(ALIGN >= alignof(void*), "Alignment must >= alignof(void*)");
		static_assert(TSIZE% ALIGN == 0, "TSIZE must be a multiple of ALIGN");
		alignas(ALIGN) char storage[TSIZE];

		/**
		 * @brief Helper template to conditionally create stack-allocated or heap-backed TaskStack
		 * @tparam Condition true for stack allocation, false for heap fallback
		 * @tparam F Type of callable object
		 * @tparam Args Types of bound arguments
		 */
		template <bool Condition, typename F, typename... Args>
		struct Maker;

		/**
		 * @brief Specialization for stack allocation
		 * @details Constructs task directly in internal storage
		 */
		template <typename F, typename... Args>
		struct Maker<true, F, Args...>
		{
			static TaskStack make(F&& func, Args &&...args)
			{
				return TaskStack(std::forward<F>(func), std::forward<Args>(args)...);
			}
		};

		/**
		 * @brief Specialization for heap fallback
		 * @details Uses HeapCallable as storage backend
		 */
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
		 * @note Uses SFINAE to prevent nesting of HeapCallable objects
		 */
		template <class F, class... Args,
			typename std::enable_if<!is_generic_hc<typename std::decay<F>::type>::value, int>::type = 0>
		static TaskStack make_auto(F&& func, Args &&...args) HSLL_ALLOW_THROW
		{
			return Maker<task_invalid<F, Args...>::value, F, Args...>::make(
				std::forward<F>(func),
				std::forward<Args>(args)...);
		}

		/**
		 * @brief Factory method that forces heap-backed storage
		 * @tparam F Type of callable object
		 * @tparam Args Types of bound arguments
		 * @param func Callable to store
		 * @param args Arguments to bind
		 * @return TaskStack using HeapCallable storage
		 * @note Always uses heap allocation regardless of size
		 */
		template <class F, class... Args,
			typename std::enable_if<!is_generic_hc<typename std::decay<F>::type>::value, int>::type = 0>
		static TaskStack make_heap(F&& func, Args &&...args) HSLL_ALLOW_THROW
		{
			return Maker<false, F, Args...>::make(
				std::forward<F>(func),
				std::forward<Args>(args)...);
		}

		/**
		 * @brief Executes the stored task
		 */
		void execute()  noexcept
		{
			getBase()->execute();
		}

		/**
		 * @brief Copy constructor (deep copy)
		 * @details Clones the underlying task to new storage
		 */
		TaskStack(const TaskStack& other)  noexcept
		{
			other.getBase()->copyTo(storage);
		}

		/**
		 * @brief Move constructor
		 * @details Moves the underlying task to new storage
		 */
		TaskStack(TaskStack&& other)  noexcept
		{
			other.getBase()->moveTo(storage);
		}

		/**
		 * @brief Destructor invokes stored task's destructor
		 */
		~TaskStack()  noexcept
		{
			getBase()->~TaskBase();
		}

		TaskStack& operator=(const TaskStack& other) = delete;
		TaskStack& operator=(TaskStack&& other) = delete;

	private:

		static void move(TaskStack& dst, TaskStack& src)  noexcept
		{
			src.getBase()->move(dst.storage);
		}

		/**
		 * @brief Gets typed pointer to task storage
		 * @return Pointer to base task interface
		 */
		TaskBase* getBase()  noexcept
		{
			return (TaskBase*)storage;
		}

		/**
		 * @brief Gets const-typed pointer to task storage
		 * @return Const pointer to base task interface
		 */
		const TaskBase* getBase() const  noexcept
		{
			return (const TaskBase*)storage;
		}
	};
}

#endif // !HSLL_TPTASK