#ifndef HSLL_TPSRWLOCK
#define HSLL_TPSRWLOCK

#include <atomic>
#include <thread>

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
			@brief Efficient spin lock based on atomic variables, suitable for scenarios where reads significantly outnumber writes
		*/
		class SpinReadWriteLock
		{
		private:

			class alignas(64) InnerLock
			{
			private:
				std::atomic<intptr_t> m_count;

			public:

				InnerLock() noexcept : m_count(0) {}

				void lock_read() noexcept
				{
					intptr_t old = m_count.fetch_add(1, std::memory_order_acquire);

					while (old < 0)
					{
						m_count.fetch_sub(1, std::memory_order_relaxed);

						std::this_thread::yield();

						while (m_count.load(std::memory_order_relaxed) < 0)
						{
							std::this_thread::yield();
						}

						old = m_count.fetch_add(1, std::memory_order_acquire);
					}
				}

				void unlock_read() noexcept
				{
					m_count.fetch_sub(1, std::memory_order_relaxed);
				}

				bool mark_write() noexcept
				{
					return !m_count.fetch_sub(HSLL_SPINREADWRITELOCK_MAXREADER, std::memory_order_relaxed);
				}

				void unmark_write(bool ready) noexcept
				{
					if (ready)
					{
						m_count.store(0, std::memory_order_relaxed);
					}
					else
					{
						m_count.fetch_add(HSLL_SPINREADWRITELOCK_MAXREADER, std::memory_order_relaxed);
					}
				}

				void unlock_write() noexcept
				{
					m_count.store(0, std::memory_order_release);
				}

				bool is_write_ready() noexcept
				{
					return m_count.load(std::memory_order_relaxed) == -HSLL_SPINREADWRITELOCK_MAXREADER;
				}
			};

			class LocalReadLock
			{
				InnerLock& m_lock;

			public:

				explicit LocalReadLock(InnerLock& lock) noexcept : m_lock(lock) {}

				void lock_read() noexcept
				{
					m_lock.lock_read();
				}

				void unlock_read() noexcept
				{
					m_lock.unlock_read();
				}
			};

			std::atomic<bool> m_flag;
			InnerLock m_rwLocks[HSLL_SPINREADWRITELOCK_MAXSLOTS];

			inline static std::atomic<intptr_t>& global_index() noexcept
			{
				static std::atomic<intptr_t> instance{ 0 };
				return instance;
			}

			inline static intptr_t& local_index() noexcept
			{
				thread_local static intptr_t instance = -1;
				return instance;
			}

			inline LocalReadLock get_local_lock() noexcept
			{
				intptr_t index = local_index();

				if (index == -1)
				{
					index = global_index().fetch_add(1, std::memory_order_relaxed) % HSLL_SPINREADWRITELOCK_MAXSLOTS;
					local_index() = index;
				}

				return LocalReadLock(m_rwLocks[index]);
			}

			bool try_mark_write(bool flagArray[HSLL_SPINREADWRITELOCK_MAXSLOTS]) noexcept
			{
				bool old = true;

				if (!m_flag.compare_exchange_strong(old, false, std::memory_order_acquire, std::memory_order_relaxed))
				{
					return false;
				}

				for (intptr_t i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i)
				{
					flagArray[i] = m_rwLocks[i].mark_write();
				}

				return true;
			}

			bool try_mark_write_check_before(bool flagArray[HSLL_SPINREADWRITELOCK_MAXSLOTS]) noexcept
			{
				if (!m_flag.load(std::memory_order_relaxed))
				{
					return false;
				}

				return try_mark_write(flagArray);
			}

			void mark_write(bool flagArray[HSLL_SPINREADWRITELOCK_MAXSLOTS]) noexcept
			{
				if (try_mark_write(flagArray))
				{
					return;
				}

				std::this_thread::yield();

				while (!try_mark_write_check_before(flagArray))
				{
					std::this_thread::yield();
				}
			}

			void unmark_write(bool flagArray[HSLL_SPINREADWRITELOCK_MAXSLOTS]) noexcept
			{
				for (intptr_t i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i)
				{
					m_rwLocks[i].unmark_write(flagArray[i]);
				}

				m_flag.store(true, std::memory_order_relaxed);
			}

			intptr_t ready_count(intptr_t startIndex, bool flagArray[HSLL_SPINREADWRITELOCK_MAXSLOTS]) noexcept
			{
				intptr_t index = HSLL_SPINREADWRITELOCK_MAXSLOTS;

				for (intptr_t i = startIndex; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i)
				{
					if (flagArray[i])
					{
						continue;
					}

					flagArray[i] = m_rwLocks[i].is_write_ready();

					if (!flagArray[i] && i < index)
					{
						index = i;
					}
				}

				return index;
			}

		public:

			SpinReadWriteLock() noexcept : m_flag(true) {}

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
				{
					std::this_thread::yield();
				}
			}

			void unlock_write() noexcept
			{
				for (intptr_t i = 0; i < HSLL_SPINREADWRITELOCK_MAXSLOTS; ++i)
				{
					m_rwLocks[i].unlock_write();
				}

				m_flag.store(true, std::memory_order_release);
			}

			SpinReadWriteLock(const SpinReadWriteLock&) = delete;
			SpinReadWriteLock& operator=(const SpinReadWriteLock&) = delete;
		};

		class ReadLockGuard
		{
		private:

			SpinReadWriteLock& m_lock;

		public:

			explicit ReadLockGuard(SpinReadWriteLock& lock) noexcept : m_lock(lock)
			{
				m_lock.lock_read();
			}

			~ReadLockGuard() noexcept
			{
				m_lock.unlock_read();
			}

			ReadLockGuard(const ReadLockGuard&) = delete;
			ReadLockGuard& operator=(const ReadLockGuard&) = delete;
		};

		class WriteLockGuard
		{
		private:

			SpinReadWriteLock& m_lock;

		public:

			explicit WriteLockGuard(SpinReadWriteLock& lock) noexcept : m_lock(lock)
			{
				m_lock.lock_write();
			}

			~WriteLockGuard() noexcept
			{
				m_lock.unlock_write();
			}

			WriteLockGuard(const WriteLockGuard&) = delete;
			WriteLockGuard& operator=(const WriteLockGuard&) = delete;
		};
	}
}

#endif // !HSLL_TPSRWLOCK