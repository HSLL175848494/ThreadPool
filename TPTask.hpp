#ifndef HSLL_TPTASK
#define HSLL_TPTASK

#include <new>
#include <tuple>
#include <cstdio>
#include <memory>
#include <cstddef>
#include <exception>
#include <type_traits>


namespace HSLL
{

	// C++11 compatible index_sequence implementation
	template <size_t... Is>
	struct index_sequence
	{
	};

	template <size_t N, size_t... Is>
	struct make_index_sequence_impl : make_index_sequence_impl<N - 1, N - 1, Is...>
	{
	};

	template <size_t... Is>
	struct make_index_sequence_impl<0, Is...>
	{
		typedef index_sequence<Is...> type;
	};

	template <size_t N>
	struct make_index_sequence
	{
		typedef typename make_index_sequence_impl<N>::type type;
	};

	template <typename Callable, typename... Ts>
	static void tinvoke(Callable& callable, Ts &...args)
	{
		callable(args...);
	}

	/**
	 * @brief Helper for applying tuple elements to a function
	 */
	template <typename Tuple, size_t... Is>
	void apply_impl(Tuple& tup, index_sequence<Is...>)
	{
		tinvoke(std::get<Is>(tup)...);
	}

	/**
	 * @brief Invokes function with arguments from tuple
	 */
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
		// SFINAE helper to detect nested task types
		template <typename T>
		struct is_generic_task : std::false_type
		{
		};

		template <class T, class... Params>
		struct is_generic_task<HeapCallable<T, Params...>> : std::true_type
		{
		};

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
		 * @note Disables overload when F is HeapCallable type (prevent nesting)
		 * @note Arguments are stored as decayed types (copy/move constructed)
		 */
		template <typename std::enable_if<!is_generic_task<typename std::decay<F>::type>::value, int>::type = 0>
		HeapCallable(F&& func, Args &&...args)
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
	template <typename F, typename... Args>
	HeapCallable<F, Args...> make_callable(F&& func, Args &&...args)
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
		virtual void execute() = 0;					  ///< Executes the stored task
		virtual void cloneTo(void* memory) const = 0; ///< Copies task to preallocated memory
		virtual void moveTo(void* memory) = 0;		  ///< Moves task to preallocated memory
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
		template <bool Copyable>
		struct CloneHelper;

		template <>
		struct CloneHelper<true>
		{
			static void clone(const TaskImpl* self, void* memory)
			{
				new (memory) TaskImpl(*self);
			}
		};

		template <>
		struct CloneHelper<false>
		{
			static void clone(const TaskImpl*, void*)
			{
				printf("\nTaskImpl must be copy constructible for cloneTo()");
				std::abort();
			}
		};

		template <bool Movable>
		struct MoveHelper;

		template <>
		struct MoveHelper<true>
		{
			static void move(TaskImpl* self, void* memory)
			{
				new (memory) TaskImpl(std::move(*self));
			}
		};

		template <>
		struct MoveHelper<false>
		{
			static void move(TaskImpl*, void*)
			{
				printf("\nTaskImpl must be move constructible for moveTo()");
				std::abort();
			}
		};

		using Tuple = std::tuple<typename std::decay<F>::type, typename std::decay<Args>::type...>;
		Tuple  storage;

		/**
		 * @brief Constructs task with function and arguments
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
		void execute() override
		{
			tuple_apply(storage);
		}

		/**
		 * @brief Copies task to preallocated memory
		 */
		void cloneTo(void* memory) const override
		{
			CloneHelper<std::is_copy_constructible<Tuple>::value>::clone(this, memory);
		}

		/**
		 * @brief Moves task to preallocated memory
		 */
		void moveTo(void* memory) override
		{
			MoveHelper<std::is_move_constructible<Tuple>::value>::move(this, memory);
		}
	};

	/**
	 * @brief Metafunction to compute the task implementation type and its size
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
	 * @details Uses SBO (Small Buffer Optimization) to avoid heap allocation
	 */
	template <unsigned int TSIZE = 64, unsigned int ALIGN = 8>
	class TaskStack
	{
		static_assert(TSIZE >= 2 * sizeof(void*), "TSIZE must >= 2 * sizeof(void*)");
		static_assert(ALIGN >= alignof(void*), "Alignment must >= alignof(void*)");
		static_assert(TSIZE% ALIGN == 0, "TSIZE must be a multiple of ALIGN");
		alignas(ALIGN) char storage[TSIZE];

		// SFINAE helper to detect nested TaskStack types
		template <typename T>
		struct is_generic_task : std::false_type
		{
		};

		template <unsigned int S, unsigned int A>
		struct is_generic_task<TaskStack<S, A>> : std::true_type
		{
		};

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
		 * @value `true` if task can be stored, `false` otherwise
		 * @note Size calculation uses adjusted storage size (TSIZE - (TSIZE % ALIGN))
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
		 * @note Disables overload when F is a TaskStack (prevent nesting)
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
			typename std::enable_if<!is_generic_task<typename std::decay<F>::type>::value, int>::type = 0>
		TaskStack(F&& func, Args &&...args)
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
		 * @note Uses SFINAE to prevent nesting of TaskStack objects
		 */
		template <class F, class... Args,
			typename std::enable_if<!is_generic_task<typename std::decay<F>::type>::value, int>::type = 0>
		static TaskStack make_auto(F&& func, Args &&...args)
		{
			return Maker<task_invalid<F, Args...>::value, F, Args...>::make(
				std::forward<F>(func),
				std::forward<Args>(args)...);
		}

		/**
		 * @brief Executes the stored task
		 */
		void execute()
		{
			getBase()->execute();
		}

		/**
		 * @brief Copy constructor (deep copy)
		 */
		TaskStack(const TaskStack& other)
		{
			other.getBase()->cloneTo(storage);
		}

		/**
		 * @brief Move constructor
		 */
		TaskStack(TaskStack&& other)
		{
			other.getBase()->moveTo(storage);
		}

		/**
		 * @brief Copy assignment operator
		 */
		TaskStack& operator=(const TaskStack& other)
		{
			if (this != &other)
			{
				getBase()->~TaskBase();
				other.getBase()->cloneTo(storage);
			}
			return *this;
		}

		/**
		 * @brief Move assignment operator
		 */
		TaskStack& operator=(TaskStack&& other)
		{
			if (this != &other)
			{
				getBase()->~TaskBase();
				other.getBase()->moveTo(storage);
			}
			return *this;
		}

		/**
		 * @brief Destructor invokes stored task's destructor
		 */
		~TaskStack()
		{
			getBase()->~TaskBase();
		}

	private:
		/**
		 * @brief Gets typed pointer to task storage
		 */
		TaskBase* getBase()
		{
			return (TaskBase*)storage;
		}

		/**
		 * @brief Gets const-typed pointer to task storage
		 */
		const TaskBase* getBase() const
		{
			return (const TaskBase*)storage;
		}
	};
}

#endif // !HSLL_TPTASK