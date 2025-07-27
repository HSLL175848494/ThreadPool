#ifndef HSLL_TPSEMAPHORE
#define HSLL_TPSEMAPHORE

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>

namespace HSLL
{
	class Semaphore
	{
	public:
		explicit Semaphore(unsigned int initial_count = 0)
		{
			m_sem = CreateSemaphoreW(nullptr, static_cast<LONG>(initial_count),
				std::numeric_limits<LONG>::max(), nullptr);
			if (!m_sem)
				throw std::system_error(GetLastError(), std::system_category());
		}

		~Semaphore() noexcept
		{
			if (m_sem) CloseHandle(m_sem);
		}

		void acquire()
		{
			DWORD result = WaitForSingleObject(m_sem, INFINITE);
			if (result != WAIT_OBJECT_0)
				throw std::system_error(GetLastError(), std::system_category());
		}

		template<typename Rep, typename Period>
		bool try_acquire_for(const std::chrono::duration<Rep, Period>& timeout)
		{
			auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
			if (ms.count() < 0) ms = std::chrono::milliseconds(0);

			DWORD wait_ms;
			if (ms.count() > MAX_WAIT_MS)
				wait_ms = MAX_WAIT_MS;
			else
				wait_ms = static_cast<DWORD>(ms.count());

			DWORD result = WaitForSingleObject(m_sem, wait_ms);
			if (result == WAIT_OBJECT_0)
				return true;
			else if (result == WAIT_TIMEOUT)
				return false;
			else
				throw std::system_error(GetLastError(), std::system_category());
		}

		void release()
		{
			if (!ReleaseSemaphore(m_sem, 1, nullptr))
				throw std::system_error(GetLastError(), std::system_category());
		}

		Semaphore(const Semaphore&) = delete;
		Semaphore& operator=(const Semaphore&) = delete;
		Semaphore(Semaphore&&) = delete;
		Semaphore& operator=(Semaphore&&) = delete;

	private:
		HANDLE m_sem = nullptr;
		static constexpr DWORD MAX_WAIT_MS = INFINITE - 1;
	};
}

#elif defined(__APPLE__)
#include <dispatch/dispatch.h>

namespace HSLL
{
	class Semaphore
	{
	public:
		explicit Semaphore(unsigned int initial_count = 0)
		{
			m_sem = dispatch_semaphore_create(static_cast<long>(initial_count));
			if (!m_sem)
				throw std::system_error(errno, std::system_category());
		}

		~Semaphore() noexcept
		{
			dispatch_release(m_sem);
		}

		void acquire()
		{
			dispatch_semaphore_wait(m_sem, DISPATCH_TIME_FOREVER);
		}

		template<typename Rep, typename Period>
		bool try_acquire_for(const std::chrono::duration<Rep, Period>& timeout)
		{
			auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout);
			return dispatch_semaphore_wait(m_sem,
				dispatch_time(DISPATCH_TIME_NOW, ns.count())) == 0;
		}

		void release()
		{
			dispatch_semaphore_signal(m_sem);
		}

		Semaphore(const Semaphore&) = delete;
		Semaphore& operator=(const Semaphore&) = delete;
		Semaphore(Semaphore&&) = delete;
		Semaphore& operator=(Semaphore&&) = delete;

	private:
		dispatch_semaphore_t m_sem;
	};
}

#elif defined(__linux__) || defined(__FreeBSD__) || \
      defined(__OpenBSD__) || defined(__NetBSD__)

#include <semaphore.h>
#include <time.h>

namespace HSLL
{
	class Semaphore
	{
	public:
		explicit Semaphore(unsigned int initial_count = 0)
		{
			if (sem_init(&m_sem, 0, initial_count) != 0)
				throw std::system_error(errno, std::system_category());
		}

		~Semaphore() noexcept
		{
			sem_destroy(&m_sem);
		}

		void acquire()
		{
			int ret;
			do {
				ret = sem_wait(&m_sem);
			} while (ret != 0 && errno == EINTR);

			if (ret != 0)
				throw std::system_error(errno, std::system_category());
		}

		template<typename Rep, typename Period>
		bool try_acquire_for(const std::chrono::duration<Rep, Period>& timeout)
		{
			auto abs_time = std::chrono::system_clock::now() + timeout;
			auto since_epoch = abs_time.time_since_epoch();

			auto secs = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
			auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch - secs);

			struct timespec ts;
			ts.tv_sec = secs.count();
			ts.tv_nsec = ns.count();

			while (true) {
				int result = sem_timedwait(&m_sem, &ts);
				if (result == 0)
					return true;
				else if (errno == EINTR)
					continue;
				else if (errno == ETIMEDOUT)
					return false;
				else
					throw std::system_error(errno, std::system_category());
			}
		}

		void release()
		{
			if (sem_post(&m_sem) != 0)
				throw std::system_error(errno, std::system_category());
		}

		Semaphore(const Semaphore&) = delete;
		Semaphore& operator=(const Semaphore&) = delete;
		Semaphore(Semaphore&&) = delete;
		Semaphore& operator=(Semaphore&&) = delete;

	private:
		sem_t m_sem;
	};
}

#else // Generic fallback for other platforms
#include <mutex>
#include <condition_variable>

namespace HSLL
{
	class Semaphore
	{
	public:
		explicit Semaphore(unsigned int initial_count = 0)
			: m_count(initial_count) {}

		void acquire()
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_cv.wait(lock, [this] { return m_count > 0; });
			--m_count;
		}

		template<typename Rep, typename Period>
		bool try_acquire_for(const std::chrono::duration<Rep, Period>& timeout)
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			if (!m_cv.wait_for(lock, timeout, [this] { return m_count > 0; }))
				return false;
			--m_count;
			return true;
		}

		void release()
		{
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				++m_count;
			}
			m_cv.notify_one();
		}

		Semaphore(const Semaphore&) = delete;
		Semaphore& operator=(const Semaphore&) = delete;
		Semaphore(Semaphore&&) = delete;
		Semaphore& operator=(Semaphore&&) = delete;

	private:
		std::mutex m_mutex;
		std::condition_variable m_cv;
		unsigned int m_count = 0;
	};
}
#endif

#endif // !HSLL_TPSEMAPHORE