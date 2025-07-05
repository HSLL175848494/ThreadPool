#ifndef HSLL_TPSEMAPHORE
#define HSLL_TPSEMAPHORE

#include <chrono>
#include <thread>
#include <limits>
#include <system_error>
#include <atomic>
#include <cerrno>

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
            return try_acquire_until(std::chrono::steady_clock::now() + timeout);
        }

        template<typename Clock, typename Duration>
        bool try_acquire_until(const std::chrono::time_point<Clock, Duration>& abs_timeout)
        {
            auto now = Clock::now();
            if (now >= abs_timeout)
                return false;

            auto timeout_duration = abs_timeout - now;
            auto timeout_ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout_duration);
            DWORD wait_ms = (timeout_ms.count() > MAX_WAIT_MS) ?
                MAX_WAIT_MS :
                static_cast<DWORD>(timeout_ms.count());

            DWORD result = WaitForSingleObject(m_sem, wait_ms);
            if (result == WAIT_OBJECT_0)
                return true;
            else if (result == WAIT_TIMEOUT)
                return false;

            throw std::system_error(GetLastError(), std::system_category());
        }

        void release(unsigned int count = 1)
        {
            if (!ReleaseSemaphore(m_sem, static_cast<LONG>(count), nullptr))
                throw std::system_error(GetLastError(), std::system_category());
        }

        Semaphore(const Semaphore&) = delete;
        Semaphore& operator=(const Semaphore&) = delete;
        Semaphore(Semaphore&&) = delete;
        Semaphore& operator=(Semaphore&&) = delete;

    private:
        HANDLE m_sem = nullptr;
        static constexpr DWORD MAX_WAIT_MS = 0xFFFFFFF;
    };
}

#elif defined(__linux__) || defined(__unix__) || \
      defined(__APPLE__) || defined(__FreeBSD__) || \
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
            return try_acquire_until(std::chrono::steady_clock::now() + timeout);
        }

        template<typename Clock, typename Duration>
        bool try_acquire_until(const std::chrono::time_point<Clock, Duration>& abs_timeout)
        {
            auto nowSys = std::chrono::system_clock::now();
            auto nowClk = Clock::now();
            auto sysTimeout = nowSys + (abs_timeout - nowClk);

            auto t = std::chrono::system_clock::to_time_t(sysTimeout);
            auto duration = sysTimeout.time_since_epoch();
            auto sec = std::chrono::duration_cast<std::chrono::seconds>(duration);
            duration -= sec;
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);

            timespec ts{};
            ts.tv_sec = t;
            ts.tv_nsec = static_cast<long>(ns.count());

            int ret;
            do {
                ret = sem_timedwait(&m_sem, &ts);
            } while (ret != 0 && errno == EINTR);

            if (ret == 0)
                return true;
            else if (errno == ETIMEDOUT)
                return false;

            throw std::system_error(errno, std::system_category());
        }

        void release(unsigned int count = 1)
        {
            for (unsigned int i = 0; i < count; ++i) {
                if (sem_post(&m_sem) != 0) {
                    throw std::system_error(errno, std::system_category());
                }
            }
        }

        Semaphore(const Semaphore&) = delete;
        Semaphore& operator=(const Semaphore&) = delete;
        Semaphore(Semaphore&&) = delete;
        Semaphore& operator=(Semaphore&&) = delete;

    private:
        sem_t m_sem;
    };
}

#else
#error "Unsupported platform: no Semaphore implementation available"
#endif

#endif // !HSLL_TPSEMAPHORE