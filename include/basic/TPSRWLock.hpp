#ifndef HSLL_TPSRWLOCK
#define HSLL_TPSRWLOCK

namespace HSLL
{
	namespace INNER
	{
		constexpr long long HSLL_SPINREADWRITELOCK_MAXSLOTS = 128;
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

			class alignas(64) InnerLock
			{
			private:
				std::atomic<long long> count;

			public:

				InnerLock() noexcept :count(0) {}

				void lock_read() noexcept
				{
					long long old = count.fetch_add(1, std::memory_order_acquire);

					while (old < 0)
					{
						count.fetch_sub(1, std::memory_order_relaxed);

						std::this_thread::yield();

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

				bool try_lock_read_check_before() noexcept
				{
					if (count.load(std::memory_order_relaxed) < 0)
						return false;

					return try_lock_read();
				}

				void unlock_read() noexcept
				{
					count.fetch_sub(1, std::memory_order_relaxed);
				}

				bool mark_write() noexcept
				{
					return !count.fetch_sub(HSLL_SPINREADWRITELOCK_MAXREADER, std::memory_order_relaxed);
				}

				void unmark_write() noexcept
				{
					count.fetch_add(HSLL_SPINREADWRITELOCK_MAXREADER, std::memory_order_relaxed);
				}

				void unlock_write() noexcept
				{
					count.fetch_add(HSLL_SPINREADWRITELOCK_MAXREADER, std::memory_order_release);
				}

				bool is_write_ready() noexcept
				{
					return count.load(std::memory_order_relaxed) == -HSLL_SPINREADWRITELOCK_MAXREADER;
				}
			};

			std::atomic<bool> flag;
			InnerLock rwLocks[HSLL_SPINREADWRITELOCK_MAXSLOTS];

			thread_local static long long localIndex;
			static std::atomic<long long> globalIndex;

			InnerLock& get_local_lock() noexcept
			{
				return rwLocks[localIndex + 1 ? localIndex : (localIndex = globalIndex.fetch_add(1, std::memory_order_relaxed) % HSLL_SPINREADWRITELOCK_MAXSLOTS)];
			}

			bool try_mark_write(bool flagArray[HSLL_SPINREADWRITELOCK_MAXSLOTS]) noexcept
			{
				bool old = true;

				if (!flag.compare_exchange_strong(old, false, std::memory_order_acquire, std::memory_order_relaxed))
					return false;

				for (long long i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i)
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

			void unmark_write() noexcept
			{
				for (long long i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i)
					rwLocks[i].unmark_write();

				flag.store(true, std::memory_order_relaxed);
			}

			long long ready_count(long long startIndex, bool flagArray[HSLL_SPINREADWRITELOCK_MAXSLOTS]) noexcept
			{
				long long index = HSLL_SPINREADWRITELOCK_MAXSLOTS;

				for (long long i = startIndex; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i)
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

			bool try_lock_read() noexcept
			{
				return get_local_lock().try_lock_read();
			}

			template <typename Rep, typename Period>
			bool try_lock_read_for(const std::chrono::duration<Rep, Period>& timeout) noexcept
			{
				auto absTime = std::chrono::steady_clock::now() + timeout;
				return try_lock_read_until(absTime);
			}

			template <typename Clock, typename Duration>
			bool try_lock_read_until(const std::chrono::time_point<Clock, Duration>& absTime) noexcept
			{
				InnerLock& lock = get_local_lock();

				if (lock.try_lock_read())
					return true;

				std::this_thread::yield();

				while (!lock.try_lock_read_check_before())
				{
					auto now = Clock::now();

					if (now >= absTime)
						return false;

					std::this_thread::yield();
				}

				return true;
			}

			void lock_read() noexcept
			{
				get_local_lock().lock_read();
			}

			void unlock_read() noexcept
			{
				get_local_lock().unlock_read();
			}

			bool try_lock_write() noexcept
			{
				bool flagArray[HSLL_SPINREADWRITELOCK_MAXSLOTS];

				if (!try_mark_write(flagArray))
					return false;

				if (ready_count(0, flagArray) == HSLL_SPINREADWRITELOCK_MAXSLOTS)
					return true;
				else
					unmark_write();

				return false;
			}

			template <typename Rep, typename Period>
			bool try_lock_write_for(const std::chrono::duration<Rep, Period>& timeout) noexcept
			{
				auto absTime = std::chrono::steady_clock::now() + timeout;
				return try_lock_write_until(absTime);
			}

			template <typename Clock, typename Duration>
			bool try_lock_write_until(const std::chrono::time_point<Clock, Duration>& absTime) noexcept
			{
				std::chrono::time_point<Clock, Duration> now;

				bool flagArray[HSLL_SPINREADWRITELOCK_MAXSLOTS];

				if (!try_mark_write(flagArray))
				{
					std::this_thread::yield();

					while (!try_mark_write_check_before(flagArray))
					{
						now = Clock::now();

						if (now >= absTime)
							return false;

						std::this_thread::yield();
					}
				}

				long long nextCheckIndex = 0;

				while ((nextCheckIndex = ready_count(nextCheckIndex, flagArray)) != HSLL_SPINREADWRITELOCK_MAXSLOTS)
				{
					now = Clock::now();

					if (now >= absTime)
					{
						unmark_write();
						return false;
					}

					std::this_thread::yield();
				}

				return true;
			}

			void lock_write() noexcept
			{
				bool flagArray[HSLL_SPINREADWRITELOCK_MAXSLOTS];

				mark_write(flagArray);

				long long nextCheckIndex = 0;

				while ((nextCheckIndex = ready_count(nextCheckIndex, flagArray)) != HSLL_SPINREADWRITELOCK_MAXSLOTS)
					std::this_thread::yield();
			}

			void unlock_write() noexcept
			{
				for (long long i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i)
					rwLocks[i].unlock_write();

				flag.store(true, std::memory_order_release);
			}

			SpinReadWriteLock(const SpinReadWriteLock&) = delete;
			SpinReadWriteLock& operator=(const SpinReadWriteLock&) = delete;
		};

		std::atomic<long long> SpinReadWriteLock::globalIndex{ 0 };
		thread_local long long SpinReadWriteLock::localIndex{ -1 };

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

#endif // !HSLL_TPSRWLOCK