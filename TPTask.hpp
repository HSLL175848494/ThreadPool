#ifndef HSLL_TPTASK
#define HSLL_TPTASK

#include <new>
#include <tuple>
#include <cstddef>
#include <type_traits>

namespace HSLL
{
	/**
	 * @brief Internal namespace for task implementation details
	 * @details Contains base task interface and implementation helpers
	 */
	namespace TSTACK
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
		static void invoke(Callable &callable, Ts &...args)
		{
			callable(args...);
		}

		/**
		 * @brief Base interface for type-erased task objects
		 * @details Provides virtual methods for task execution and storage management
		 */
		struct TaskBase
		{
			virtual ~TaskBase() = default;
			virtual void execute() = 0;					  ///< Executes the stored task
			virtual void cloneTo(void *memory) const = 0; ///< Copies task to preallocated memory
			virtual void moveTo(void *memory) = 0;		  ///< Moves task to preallocated memory
		};

		/**
		 * @brief Helper for applying tuple elements to a function
		 */
		template <typename Tuple, size_t... Is>
		void apply_impl(Tuple &tup, index_sequence<Is...>)
		{
			invoke(std::get<Is>(tup)...);
		}

		/**
		 * @brief Invokes function with arguments from tuple
		 */
		template <typename Tuple>
		void tuple_apply(Tuple &tup)
		{
			apply_impl(tup, typename make_index_sequence<std::tuple_size<Tuple>::value>::type{});
		}

		/**
		 * @brief Concrete task implementation storing function and arguments
		 * @tparam F Type of callable object
		 * @tparam Args Types of bound arguments
		 * @details Stores decayed copies of function and arguments in a tuple
		 */
		template <class F, class... Args>
		struct TaskImpl : TaskBase
		{
			std::tuple<
				typename std::decay<F>::type,
				typename std::decay<Args>::type...>
				storage; ///< Type-erased storage

			/**
			 * @brief Constructs task with function and arguments
			 */
			TaskImpl(F &&func, Args &&...args)
				: storage(std::forward<F>(func), std::forward<Args>(args)...) {}

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
			void cloneTo(void *memory) const override
			{
				new (memory) TaskImpl(*this);
			}

			/**
			 * @brief Moves task to preallocated memory
			 */
			void moveTo(void *memory) override
			{
				new (memory) TaskImpl(std::move(*this));
			}
		};
	}

	/**
	 * @brief Metafunction to compute the task implementation type and its size
	 * @details Provides:
	 *   - `type`: Concrete TaskImpl type for given function and arguments
	 *   - `size`: Size in bytes of the TaskImpl type
	 */
	template <class F, class... Args>
	struct task_stack
	{
		using type = typename TSTACK::TaskImpl<F, Args...>;
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
		static_assert(TSIZE >= 2 * sizeof(void *), "TSIZE must >= 2 * sizeof(void*)");
		static_assert(ALIGN >= alignof(void *), "Alignment must >= alignof(void*)");
		static_assert(TSIZE % ALIGN == 0, "TSIZE must be a multiple of ALIGN");
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
		TaskStack(F &&func, Args &&...args)
		{
			typedef typename task_stack<F, Args...>::type ImplType;
			static_assert(sizeof(ImplType) <= TSIZE, "TaskImpl size exceeds storage");
			static_assert(alignof(ImplType) <= ALIGN, "TaskImpl alignment exceeds storage alignment");
			new (storage) ImplType(std::forward<F>(func), std::forward<Args>(args)...);
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
		TaskStack(const TaskStack &other)
		{
			other.getBase()->cloneTo(storage);
		}

		/**
		 * @brief Move constructor
		 */
		TaskStack(TaskStack &&other)
		{
			other.getBase()->moveTo(storage);
		}

		/**
		 * @brief Copy assignment operator
		 */
		TaskStack &operator=(const TaskStack &other)
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
		TaskStack &operator=(TaskStack &&other)
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
		TSTACK::TaskBase *getBase()
		{
			return (TSTACK::TaskBase *)storage;
		}

		/**
		 * @brief Gets const-typed pointer to task storage
		 */
		const TSTACK::TaskBase *getBase() const
		{
			return (const TSTACK::TaskBase *)storage;
		}
	};
}

#endif // !HSLL_TPTASK