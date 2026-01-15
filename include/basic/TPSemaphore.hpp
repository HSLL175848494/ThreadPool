#ifndef HSLL_TPSEMAPHORE
#define HSLL_TPSEMAPHORE

#include <condition_variable>

namespace HSLL
{
	namespace INNER
	{
		class Semaphore
		{
		public:
			explicit Semaphore(unsigned int initialCount = 0) : m_count(initialCount) {}

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
				{
					return false;
				}

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
			unsigned int m_count;
			std::mutex m_mutex;
			std::condition_variable m_cv;
		};
	}
}

#endif // !HSLL_TPSEMAPHORE