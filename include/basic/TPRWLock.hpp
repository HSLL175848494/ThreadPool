#ifndef TPRWLOCK
#define TPRWLOCK

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>

namespace HSLL
{
	class ReadWriteLock {
	public:
		ReadWriteLock() {
			InitializeSRWLock(&srwlock_);
		}

		~ReadWriteLock() = default;

		void lock_read() {
			AcquireSRWLockShared(&srwlock_);
		}

		void unlock_read() {
			ReleaseSRWLockShared(&srwlock_);
		}

		void lock_write() {
			AcquireSRWLockExclusive(&srwlock_);
		}

		void unlock_write() {
			ReleaseSRWLockExclusive(&srwlock_);
		}

		ReadWriteLock(const ReadWriteLock&) = delete;
		ReadWriteLock& operator=(const ReadWriteLock&) = delete;

	private:
		SRWLOCK srwlock_;
	};
}

#elif defined(__linux__) || defined(__unix__) || \
      defined(__APPLE__) || defined(__FreeBSD__) || \
      defined(__OpenBSD__) || defined(__NetBSD__)

#include <pthread.h>

namespace HSLL
{
	class ReadWriteLock {
	public:
		ReadWriteLock() {
			pthread_rwlock_init(&rwlock_, nullptr);
		}

		~ReadWriteLock() {
			pthread_rwlock_destroy(&rwlock_);
		}

		void lock_read() {
			pthread_rwlock_rdlock(&rwlock_);
		}

		void unlock_read() {
			pthread_rwlock_unlock(&rwlock_);
		}

		void lock_write() {
			pthread_rwlock_wrlock(&rwlock_);
		}

		void unlock_write() {
			pthread_rwlock_unlock(&rwlock_);
		}

		ReadWriteLock(const ReadWriteLock&) = delete;
		ReadWriteLock& operator=(const ReadWriteLock&) = delete;

	private:
		pthread_rwlock_t rwlock_;
	};
}


#else
#error "Unsupported platform: no ReadWriteLock implementation available"
#endif

namespace HSLL
{
	class ReadLockGuard {
	public:
		explicit ReadLockGuard(ReadWriteLock& lock) : lock_(lock) {
			lock_.lock_read();
		}

		~ReadLockGuard() {
			lock_.unlock_read();
		}

		ReadLockGuard(const ReadLockGuard&) = delete;
		ReadLockGuard& operator=(const ReadLockGuard&) = delete;

	private:
		ReadWriteLock& lock_;
	};

	class WriteLockGuard {
	public:
		explicit WriteLockGuard(ReadWriteLock& lock) : lock_(lock) {
			lock_.lock_write();
		}

		~WriteLockGuard() {
			lock_.unlock_write();
		}

		WriteLockGuard(const WriteLockGuard&) = delete;
		WriteLockGuard& operator=(const WriteLockGuard&) = delete;

	private:
		ReadWriteLock& lock_;
	};
}

#endif // !TPRWLOCK