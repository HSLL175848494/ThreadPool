#ifndef HSLL_SEMAPHORE
#define HSLL_SEMAPHORE

#include <chrono>
#include <thread>
#include <limits>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#endif

namespace HSLL
{
	class Semaphore 
	{
	public:
		explicit Semaphore(unsigned int initial_count = 0) 
		{
#ifdef _WIN32
			m_sem = CreateSemaphore(nullptr, initial_count, std::numeric_limits<LONG>::max(), nullptr);

			if (!m_sem) 
			throw std::system_error(GetLastError(), std::system_category());		
#else
			if (sem_init(&m_sem, 0, initial_count) != 0) 
			throw std::system_error(errno, std::system_category());
#endif
		}

		~Semaphore() noexcept 
		{
#ifdef _WIN32
			CloseHandle(m_sem);
#else
			sem_destroy(&m_sem);
#endif
		}

		void acquire() 
		{
#ifdef _WIN32
			DWORD result = WaitForSingleObject(m_sem, INFINITE);

			if (result != WAIT_OBJECT_0) 
			throw std::system_error(GetLastError(), std::system_category());
		
#else
			while (true) 
			{

				if (sem_wait(&m_sem) 
				{
					if (errno == EINTR) 
						continue;

					throw std::system_error(errno, std::system_category());
				}
				break;
			}
#endif
		}

		template<typename Rep, typename Period>
		bool try_acquire_for(const std::chrono::duration<Rep, Period>& timeout) 
		{
			return try_acquire_until(std::chrono::steady_clock::now() + timeout);
		}

		template<typename Clock, typename Duration>
		bool try_acquire_until(const std::chrono::time_point<Clock, Duration>& abs_timeout) 
		{
#ifdef _WIN32
			auto now = Clock::now();

			if (now >= abs_timeout) 
				return false;

			auto timeout_ms = std::chrono::duration_cast<std::chrono::milliseconds>(abs_timeout - now).count();

			DWORD result = WaitForSingleObject(m_sem, static_cast<DWORD>(timeout_ms));
			if (result == WAIT_OBJECT_0) 
				return true;	
			else if (result == WAIT_TIMEOUT) 
				return false;
			
			throw std::system_error(GetLastError(), std::system_category());

#else
			auto ts = duration_to_timespec(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					abs_timeout - std::chrono::steady_clock::now()
				)
			);

			while (true) 
			{
				int result = sem_timedwait(&m_sem, &ts);

				if (result == 0) 
					return true;
				else if (errno == ETIMEDOUT) 
					return false;
				else if (errno == EINTR) 
					continue; 

				throw std::system_error(errno, std::system_category());
			}
#endif
		}

		void release(unsigned int count = 1) 
		{
#ifdef _WIN32
			if (!ReleaseSemaphore(m_sem, count, nullptr)) 
				throw std::system_error(GetLastError(), std::system_category());
#else
			while (count-- > 0) 
			{
				if (sem_post(&m_sem) 
				throw std::system_error(errno, std::system_category());
			}
#endif
		}

	private:
#ifdef _WIN32
		HANDLE m_sem;
#else
		sem_t m_sem;

		static timespec duration_to_timespec(std::chrono::nanoseconds ns) 
		{
			auto sec = std::chrono::duration_cast<std::chrono::seconds>(ns);
			ns -= sec;
			timespec ts;
			ts.tv_sec = sec.count();
			ts.tv_nsec = static_cast<long>(ns.count());
			return ts;
		}
#endif
	};
}
#endif // !HSLL_SEMAPHORE