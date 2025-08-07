#ifndef HSLL_TPSRWLOCK
#define HSLL_TPSRWLOCK

namespace HSLL
{
	namespace INNER
	{
		constexpr int HSLL_SPINREADWRITELOCK_MAXSLOTS = 128;
		constexpr long long HSLL_SPINREADWRITELOCK_MAXREADER = (1LL << 62);

		static_assert(HSLL_SPINREADWRITELOCK_MAXSLOTS > 0, "HSLL_SPINREADWRITELOCK_MAXSLOTS must be > 0");
		static_assert(HSLL_SPINREADWRITELOCK_MAXREADER > 0 && HSLL_SPINREADWRITELOCK_MAXREADER <= (1LL << 62),
			"HSLL_SPINREADWRITELOCK_MAXREADER must be > 0 and <= 2^62");

		/**
		 * @brief Efficient spin lock based on atomic variables, suitable for scenarios where reads significantly outnumber writes
		 */
		class SpinReadWriteLock
		{
		private:

			class InnerRWLock
			{
			private:
				std::atomic<long long> count;

			public:

				InnerRWLock()noexcept :count(0) {}

				void lock_read() noexcept
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

				bool try_lock_read() noexcept
				{
					long long old = count.fetch_add(1, std::memory_order_acquire);

					if (old < 0)
					{
						count.fetch_sub(1, std::memory_order_relaxed);
						return false;
					}

					return true;
				}

				void unlock_read() noexcept
				{
					count.fetch_sub(1, std::memory_order_relaxed);
				}

				void mark_write() noexcept
				{
					count.fetch_sub(HSLL_SPINREADWRITELOCK_MAXREADER, std::memory_order_relaxed);
				}

				void unmark_write() noexcept
				{
					count.fetch_add(HSLL_SPINREADWRITELOCK_MAXREADER, std::memory_order_relaxed);
				}

				bool is_write_ready() noexcept
				{
					return count.load(std::memory_order_relaxed) == -HSLL_SPINREADWRITELOCK_MAXREADER;
				}

				void unlock_write() noexcept
				{
					count.fetch_add(HSLL_SPINREADWRITELOCK_MAXREADER, std::memory_order_release);
				}
			};

			struct alignas(64) Slot
			{
				InnerRWLock lock;

				void lock_read() noexcept
				{
					lock.lock_read();
				}

				bool try_lock_read() noexcept
				{
					return lock.try_lock_read();
				}

				void unlock_read() noexcept
				{
					lock.unlock_read();
				}

				void mark_write() noexcept
				{
					lock.mark_write();
				}

				void unmark_write() noexcept
				{
					lock.unmark_write();
				}

				bool is_write_ready() noexcept
				{
					return lock.is_write_ready();
				}

				void unlock_write() noexcept
				{
					lock.unlock_write();
				}
			};

			std::atomic<bool> flag;
			Slot slots[HSLL_SPINREADWRITELOCK_MAXSLOTS];

			thread_local static int localIndex;
			static std::atomic<unsigned int> globalIndex;

		public:

			SpinReadWriteLock() noexcept :flag(true) {}

			bool try_lock_read() noexcept
			{
				return slots[localIndex].try_lock_read();
			}

			void lock_read() noexcept
			{
				slots[localIndex].lock_read();
			}

			void unlock_read() noexcept
			{
				slots[localIndex].unlock_read();
			}

			void lock_write() noexcept
			{
				bool old = true;

				while (!flag.compare_exchange_weak(old, false, std::memory_order_acquire, std::memory_order_relaxed))
				{
					std::this_thread::yield();
					old = true;
				}

				for (int i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i)
					slots[i].mark_write();

				while (true)
				{
					bool allReady = true;

					for (int i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i)
					{
						if (!slots[i].is_write_ready())
						{
							allReady = false;
							break;
						}
					}

					if (allReady)
						break;

					std::this_thread::yield();
				}
			}

			bool try_lock_write_until(const std::chrono::steady_clock::time_point& timestamp) noexcept
			{
				bool old = true;

				while (!flag.compare_exchange_weak(old, false, std::memory_order_acquire, std::memory_order_relaxed))
				{
					old = true;

					auto now = std::chrono::steady_clock::now();

					if (now >= timestamp)
						return false;

					std::this_thread::yield();
				}

				for (int i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i)
					slots[i].mark_write();

				while (true)
				{
					bool allReady = true;

					for (int i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i)
					{
						if (!slots[i].is_write_ready())
						{
							allReady = false;
							break;
						}
					}

					if (allReady)
						break;

					auto now = std::chrono::steady_clock::now();

					if (now >= timestamp)
					{
						for (int i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i)
							slots[i].unmark_write();

						flag.store(true, std::memory_order_relaxed);
						return false;
					}

					std::this_thread::yield();
				}

				return true;
			}

			void unlock_write() noexcept
			{
				for (int i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i)
					slots[i].unlock_write();

				flag.store(true, std::memory_order_release);
			}

			SpinReadWriteLock(const SpinReadWriteLock&) = delete;
			SpinReadWriteLock& operator=(const SpinReadWriteLock&) = delete;
		};

		std::atomic<unsigned int> SpinReadWriteLock::globalIndex{ 0 };
		thread_local int SpinReadWriteLock::localIndex{ globalIndex.fetch_add(1, std::memory_order_relaxed) % HSLL_SPINREADWRITELOCK_MAXSLOTS };

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

			explicit WriteLockGuard(SpinReadWriteLock& lock)noexcept : lock(lock)
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

#endif // !HSLL_TPSRWLOCK