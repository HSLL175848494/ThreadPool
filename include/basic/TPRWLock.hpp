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

		class InnerRWLock
		{
		private:
			std::atomic<long long> count;

		public:

			InnerRWLock() :count(0) {}

			// Optimistic locking since reads greatly outnumber writes: increment first then handle rollback if needed
			void lock_read()
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
			void unlock_read()
			{
				count.fetch_sub(1, std::memory_order_relaxed);
			}

			// Add write flag to prevent new read locks
			void mark_write()
			{
				long long old = count.load(std::memory_order_relaxed);

				while (!count.compare_exchange_weak(old, old - HSLL_SPINREADWRITELOCK_MAXREADER, std::memory_order_relaxed, std::memory_order_relaxed));
			}

			// Must call mark_write() before this function, otherwise will never succeed
			bool is_write_ready()
			{
				return count.load(std::memory_order_relaxed) == -HSLL_SPINREADWRITELOCK_MAXREADER;
			}

			// Release semantics to propagate write results
			void unlock_write()
			{
				count.fetch_add(HSLL_SPINREADWRITELOCK_MAXREADER, std::memory_order_release);
			}
		};

		struct alignas(64) PeerLock
		{
			InnerRWLock lock;

			void lock_read()
			{
				lock.lock_read();
			}

			void unlock_read()
			{
				lock.unlock_read();
			}

			void mark_write()
			{
				lock.mark_write();
			}

			bool is_write_ready()
			{
				return lock.is_write_ready();
			}

			void unlock_write()
			{
				lock.unlock_write();
			}
		};

		PeerLock counter[256];
		std::atomic<bool> flag;
		thread_local static int local_index;
		static std::atomic<unsigned char> index;

	public:

		SpinReadWriteLock() :flag(true) {}

		unsigned int get_local_index()
		{
			if (local_index == -1)
				local_index = index.fetch_add(1, std::memory_order_relaxed); // unsigned char auto-wraps, no modulo needed

			return local_index;
		}

		void lock_read()
		{
			counter[get_local_index()].lock_read();
		}

		void unlock_read()
		{
			counter[get_local_index()].unlock_read();
		}

		void lock_write()
		{
			bool old = true;

			// Set write flag to block new writers and acquire write permission
			while (!flag.compare_exchange_weak(old, false, std::memory_order_acquire, std::memory_order_relaxed))
			{
				std::this_thread::yield();
				old = true;
			}

			for (int i = 0; i < 256; ++i) // Mark writer waiting to prevent new readers
				counter[i].mark_write();

			while (true)
			{
				bool allReady = true;

				for (int i = 0; i < 256; ++i) // Lock successful when all write locks acquired
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
			int a = 5;
		}

		void unlock_write()
		{
			for (int i = 0; i < 256; ++i) // Release all read-write locks and propagate to readers
				counter[i].unlock_write();

			flag.store(true, std::memory_order_release); // Allow new writers and propagate result
		}

		SpinReadWriteLock(const SpinReadWriteLock&) = delete;
		SpinReadWriteLock& operator=(const SpinReadWriteLock&) = delete;
	};

	thread_local int SpinReadWriteLock::local_index = -1;
	std::atomic<unsigned char> SpinReadWriteLock::index = 0;

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