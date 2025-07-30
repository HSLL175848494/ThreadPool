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

				// Optimistic locking since reads greatly outnumber writes: increment first then handle rollback if needed
				void lock_read() noexcept
				{
					long long old = count.fetch_add(1, std::memory_order_acquire); // Acquire semantics to get write results

					while (old < 0)
					{
						count.fetch_sub(1, std::memory_order_relaxed);

						while (count.load(std::memory_order_relaxed) < 0)
							std::this_thread::yield();

						old = count.fetch_add(1, std::memory_order_acquire);
					}
				}

				// Relaxed semantics sufficient since read operations don't modify shared state
				void unlock_read() noexcept
				{
					count.fetch_sub(1, std::memory_order_relaxed);
				}

				// Add write flag to prevent new read locks
				void mark_write() noexcept
				{
					long long old = count.load(std::memory_order_relaxed);

					while (!count.compare_exchange_weak(old, old - HSLL_SPINREADWRITELOCK_MAXREADER, std::memory_order_relaxed, std::memory_order_relaxed));
				}

				// Must call mark_write() before this function, otherwise will never succeed
				bool is_write_ready() noexcept
				{
					return count.load(std::memory_order_relaxed) == -HSLL_SPINREADWRITELOCK_MAXREADER;
				}

				// Release semantics to propagate write results
				void unlock_write() noexcept
				{
					count.fetch_add(HSLL_SPINREADWRITELOCK_MAXREADER, std::memory_order_release);
				}
			};

			struct alignas(64) PeerLock
			{
				InnerRWLock lock;

				void lock_read() noexcept
				{
					lock.lock_read();
				}

				void unlock_read() noexcept
				{
					lock.unlock_read();
				}

				void mark_write() noexcept
				{
					lock.mark_write();
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
			thread_local static int local_index;
			static std::atomic<unsigned int> index;
			PeerLock counter[HSLL_SPINREADWRITELOCK_MAXSLOTS];

		public:

			SpinReadWriteLock() noexcept :flag(true) {}

			unsigned int get_local_index() noexcept
			{
				if (local_index == -1)
					local_index = index.fetch_add(1, std::memory_order_relaxed) % HSLL_SPINREADWRITELOCK_MAXSLOTS;

				return local_index;
			}

			void lock_read() noexcept
			{
				counter[get_local_index()].lock_read();
			}

			void unlock_read() noexcept
			{
				counter[get_local_index()].unlock_read();
			}

			void lock_write() noexcept
			{
				bool old = true;

				// Set write flag to block new writers and acquire write permission
				while (!flag.compare_exchange_weak(old, false, std::memory_order_acquire, std::memory_order_relaxed))
				{
					std::this_thread::yield();
					old = true;
				}

				for (int i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i) // Mark writer waiting to prevent new readers
					counter[i].mark_write();

				while (true)
				{
					bool allReady = true;

					for (int i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i) // Lock successful when all write locks acquired
					{
						if (!counter[i].is_write_ready())
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

				// Set write flag to block new writers and acquire write permission
				while (!flag.compare_exchange_weak(old, false, std::memory_order_acquire, std::memory_order_relaxed))
				{
					old = true;

					auto now = std::chrono::steady_clock::now();

					if (now >= timestamp)
						return false;

					std::this_thread::yield();
				}

				for (int i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i) // Mark writer waiting to prevent new readers
					counter[i].mark_write();

				while (true)
				{
					bool allReady = true;

					for (int i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i) // Lock successful when all write locks acquired
					{
						if (!counter[i].is_write_ready())
						{
							allReady = false;
							break;
						}
					}

					if (allReady)
						break;

					auto now = std::chrono::steady_clock::now();

					if (now >= timestamp)//rollback
					{
						for (int i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i)
							counter[i].unlock_write();

						flag.store(true, std::memory_order_relaxed);
						return false;
					}

					std::this_thread::yield();
				}

				return true;
			}

			void unlock_write() noexcept
			{
				for (int i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i) // Release all read-write locks and propagate to readers
					counter[i].unlock_write();

				flag.store(true, std::memory_order_release); // Allow new writers and propagate result
			}

			SpinReadWriteLock(const SpinReadWriteLock&) = delete;
			SpinReadWriteLock& operator=(const SpinReadWriteLock&) = delete;
		};

		thread_local int SpinReadWriteLock::local_index{ -1 };
		std::atomic<unsigned int> SpinReadWriteLock::index{ 0 };

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