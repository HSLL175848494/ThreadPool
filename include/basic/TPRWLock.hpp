#ifndef TPRWLOCK
#define TPRWLOCK

namespace HSLL
{
	constexpr long long HSLL_SPINREADWRITELOCK_MAXREADER = (1LL << 62);

	/**
	 * @brief Efficient spin lock based on atomic variables, suitable for scenarios where reads significantly outnumber writes
	 */
	class SpinReadWriteLock
	{
	private:
		std::atomic<long long> count;

	public:

		SpinReadWriteLock() :count(0) {}


		void lock_read()
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

		void unlock_read()
		{
			count.fetch_sub(1, std::memory_order_relaxed);
		}

		void lock_write()
		{
			long long old = count.load(std::memory_order_relaxed);

			while (true)
			{
				if (old < 0)
				{
					std::this_thread::yield();
					old = count.load(std::memory_order_relaxed);
				}
				else if (count.compare_exchange_weak(old, old - HSLL_SPINREADWRITELOCK_MAXREADER, std::memory_order_acquire, std::memory_order_relaxed))
				{
					break;
				}
			}

			while (count.load(std::memory_order_relaxed) != -HSLL_SPINREADWRITELOCK_MAXREADER);

			std::atomic_thread_fence(std::memory_order_acquire);
		}

		void unlock_write()
		{
			count.fetch_add(HSLL_SPINREADWRITELOCK_MAXREADER, std::memory_order_release);
		}

		SpinReadWriteLock(const SpinReadWriteLock&) = delete;
		SpinReadWriteLock& operator=(const SpinReadWriteLock&) = delete;
	};

	class ReadLockGuard
	{
	private:

		SpinReadWriteLock& lock;

	public:

		explicit ReadLockGuard(SpinReadWriteLock& lock) : lock(lock)
		{
			lock.lock_read();
		}

		~ReadLockGuard()
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

		explicit WriteLockGuard(SpinReadWriteLock& lock) : lock(lock)
		{
			lock.lock_write();
		}

		~WriteLockGuard()
		{
			lock.unlock_write();
		}

		WriteLockGuard(const WriteLockGuard&) = delete;
		WriteLockGuard& operator=(const WriteLockGuard&) = delete;
	};
}

#endif // !TPRWLOCK