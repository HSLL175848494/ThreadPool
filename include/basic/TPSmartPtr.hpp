#ifndef HSLL_TPSMARTPTR
#define HSLL_TPSMARTPTR

namespace HSLL
{
	namespace INNER
	{
		template<typename T>
		class tp_unique_ptr;

		template<typename T>
		class tp_shared_ptr;

		template <typename T>
		struct is_tp_unique_ptr : std::false_type {};

		template <typename T>
		struct is_tp_unique_ptr<tp_unique_ptr<T>> : std::true_type {};

		template <typename T>
		struct is_tp_shared_ptr : std::false_type {};

		template <typename T>
		struct is_tp_shared_ptr<tp_shared_ptr<T>> : std::true_type {};

		class AllocatorBase
		{
		public:
			virtual void* allocate(size_t size) const = 0;
			virtual void deallocate(void* p) const = 0;
		};

		class DefaultAllocator : public AllocatorBase
		{
		public:
			void* allocate(size_t size) const override
			{
				return malloc(size);
			}

			void deallocate(void* p) const override
			{
				free(p);
			}
		};

		static const DefaultAllocator smart_ptr_default_allocator;
		static const AllocatorBase* smart_ptr_allocator = &smart_ptr_default_allocator;

		template<typename T>
		class tp_unique_ptr
		{
			static_assert(alignof(T) <= alignof(std::max_align_t),
				"The alignment requirement of T exceeds the maximum alignment supported by the standard library allocators.");

			T* data;

		public:

			template<typename U,
				typename std::enable_if<!is_tp_unique_ptr<typename std::decay<U>::type>::value, bool>::type = true,
				typename... Args>
			tp_unique_ptr(U&& any, Args&&... args)
			{
				data = (T*)smart_ptr_allocator->allocate(sizeof(T));

				if (!data)
					throw std::bad_alloc();

				try
				{
					new (data) T(std::forward<U>(any), std::forward<Args>(args)...);
				}
				catch (...)
				{
					smart_ptr_allocator->deallocate(data);
					throw;
				}
			}

			T& operator*() const
			{
				if (!data)
					throw std::logic_error("Dereferencing null unique_ptr");

				return *data;
			}

			tp_unique_ptr(tp_unique_ptr&& other) noexcept : data(other.data)
			{
				other.data = nullptr;
			}

			void release() noexcept
			{
				if (data)
				{
					data->~T();
					smart_ptr_allocator->deallocate(data);
					data = nullptr;
				}
			}

			~tp_unique_ptr()
			{
				release();
			}

			tp_unique_ptr(const tp_unique_ptr&) = delete;
			tp_unique_ptr& operator=(const tp_unique_ptr&) = delete;
			tp_unique_ptr& operator=(tp_unique_ptr&&) = delete;
		};

		template<typename T>
		class tp_shared_ptr
		{
			static_assert(alignof(T) <= alignof(std::max_align_t),
				"The alignment requirement of T exceeds the maximum alignment supported by the standard library allocators.");

			struct Controller
			{
				T data;
				std::atomic<unsigned int> refcount;
			};

			Controller* ctrl;

		public:

			template<typename U,
				typename std::enable_if<!is_tp_shared_ptr<typename std::decay<U>::type>::value, bool>::type = true,
				typename... Args>
			tp_shared_ptr(U&& any, Args&&... args)
			{
				ctrl = (Controller*)smart_ptr_allocator->allocate(sizeof(Controller));

				if (!ctrl)
					throw std::bad_alloc();

				try
				{
					new (&(ctrl->data)) T(std::forward<U>(any), std::forward<Args>(args)...);
					new (&(ctrl->refcount)) std::atomic<unsigned int>(1);//seq_cst
				}
				catch (...)
				{
					smart_ptr_allocator->deallocate(ctrl);
					throw;
				}
			}

			T& operator*() const
			{
				if (!ctrl)
					throw std::logic_error("Dereferencing null shared_ptr");

				return ctrl->data;
			}

			tp_shared_ptr(const tp_shared_ptr& other) noexcept : ctrl(other.ctrl)
			{
				if (ctrl)
					ctrl->refcount.fetch_add(1, std::memory_order_relaxed);
			}

			tp_shared_ptr(tp_shared_ptr&& other) noexcept : ctrl(other.ctrl)
			{
				other.ctrl = nullptr;
			}

			void release() noexcept
			{
				if (ctrl && ctrl->refcount.fetch_sub(1, std::memory_order_relaxed) == 1)
				{
					ctrl->~Controller();
					smart_ptr_allocator->deallocate(ctrl);
					ctrl = nullptr;
				}
			}

			~tp_shared_ptr()
			{
				release();
			}

			tp_shared_ptr& operator=(const tp_shared_ptr& other) = delete;
			tp_shared_ptr& operator=(tp_shared_ptr&& other) = delete;
		};

		/**
		 * @brief Sets the global allocator for tp_smart_ptr  (Not thread-safe)
		 * @param allocator Custom allocator pointer. Defaults to `nullptr`, indicating use of the built-in malloc/free allocator
		 * @note Important considerations:
		 *  1. Before replacing the allocator, ensure all smart pointers relying on the previous allocator are fully released
		 *  2. While smart pointers are still alive, ensure the instance pointed to by `allocator` remains valid
		 */
		void set_tp_smart_ptr_allocator(const AllocatorBase* allocator = nullptr)
		{
			const AllocatorBase* select = allocator ? allocator : &smart_ptr_default_allocator;

			if (smart_ptr_allocator != select)
				smart_ptr_allocator = select;
		}
	}

	using INNER::AllocatorBase;
	using INNER::set_tp_smart_ptr_allocator;
}

#endif //HSLL_TPSMARTPTR