#ifndef HSLL_TPTASK
#define HSLL_TPTASK

#include <new>
#include <tuple>
#include <utility>
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
		/**
		 * @brief Base interface for type-erased task objects
		 * @details Provides virtual methods for task execution and storage management
		 */
		struct TaskBase
		{
			virtual ~TaskBase() = default;
			virtual void execute() = 0;				///< Executes the stored task
			virtual void cloneTo(void* memory) = 0; ///< Copies task to preallocated memory
			virtual void moveTo(void* memory) = 0;	///< Moves task to preallocated memory
		};

		/**
		 * @brief Helper for applying tuple elements to a function
		 */
		template <typename Func, typename Tuple, size_t... Is>
		void apply_impl(Func& func, Tuple& tup, std::index_sequence<Is...>)
		{
			func(std::get<Is>(tup)...);
		}

		/**
		 * @brief Invokes function with arguments from tuple
		 */
		template <typename Func, typename Tuple>
		void tuple_apply(Func& func, Tuple& tup)
		{
			apply_impl(func, tup, std::make_index_sequence<std::tuple_size<Tuple>::value>{});
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
			std::tuple<std::decay_t<F>, std::decay_t<Args>...> storage; ///< Type-erased storage

			/**
			 * @brief Constructs task with function and arguments
			 */
			TaskImpl(F&& func, Args &&...args)
				: storage(std::forward<F>(func), std::forward<Args>(args)...) {}

			/**
			 * @brief Executes stored task with bound arguments
			 * @details Unpacks tuple and invokes stored callable
			 * @note Arguments are ALWAYS passed as lvalues during invocation
			 *       regardless of original construction value category
			 */
			void execute() override
			{
				auto invoker = [](auto& func, auto &...args)
				{
					func(args...);
				};
				tuple_apply(invoker, storage);
			}

			/**
			 * @brief Copies task to preallocated memory
			 */
			void cloneTo(void* memory) override
			{
				new (memory) TaskImpl(*this);
			}

			/**
			 * @brief Moves task to preallocated memory
			 */
			void moveTo(void* memory) override
			{
				new (memory) TaskImpl(std::move(*this));
			}
		};
	}

	// Public type aliases
	template <class F, class... Args>
	using stack_task_t = TSTACK::TaskImpl<F, Args...>;

	/// Compile-time size calculator for task objects
	template <class F, class... Args>
	constexpr unsigned int stack_tsize_v = sizeof(stack_task_t<F, Args...>);

	/**
	 * @brief Stack-allocated task container with fixed-size storage
	 * @tparam TSIZE Size of internal storage buffer ( 64 - sizeof(std::max_align_t))
	 * @tparam ALIGN Alignment requirement for storage (default = std::max_align_t)
	 * @details Uses SBO (Small Buffer Optimization) to avoid heap allocation
	 */
	template <unsigned int TSIZE = 64 - sizeof(std::max_align_t), unsigned int ALIGN = sizeof(std::max_align_t)>
	class TaskStack
	{
		static_assert(ALIGN >= 1, "Alignment must be at least 1");
		static_assert(TSIZE - (TSIZE % ALIGN) >= 8, "Adjusted storage size must be at least 8 bytes");

		alignas(ALIGN) char storage[TSIZE-(TSIZE% ALIGN)]; ///< Raw task storage

		// SFINAE helper to detect nested TaskStack types
		template <typename T>
		struct is_generic_task : std::false_type
		{
		};

		template <unsigned int SZ>
		struct is_generic_task<TaskStack<SZ>> : std::true_type
		{
		};

	public:
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
		template <class F, class... Args, std::enable_if_t<!is_generic_task<std::decay_t<F>>::value, int> = 0>
		TaskStack(F&& func, Args &&...args)
		{
			using ImplType = stack_task_t<F, Args...>;
			static_assert(sizeof(ImplType) <= sizeof(storage), "TaskImpl size exceeds storage");
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
		TSTACK::TaskBase* getBase()
		{
			return (TSTACK::TaskBase*)storage;
		}

		/**
		 * @brief Gets const-typed pointer to task storage
		 */
		TSTACK::TaskBase* getBase() const
		{
			return (TSTACK::TaskBase*)storage;
		}
	};
}

#endif // !HSLL_TPTASK
