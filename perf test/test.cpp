#include <chrono>
#include "ThreadPool.hpp"

unsigned int k;

void testA() {

	for (int i = 0; i < 10000; ++i)
		k = k * 2 + i;
}

void testB() {

	for (int i = 0; i < 100; ++i)
		k = k * 2 + i;
}

void testC() {

}

constexpr const char* taskType[] = { "A","B","C" };

#define WORKER 8
#define PRODUCER 8
#define SUBMIT_BATCH 1
#define PROCESS_BATCH 1
#define PEER 10000000/PRODUCER
#define TSIZE 24
#define CAPACITY 8192
#define FUNC testC
#define CALLABLE_STACK FUNC
#define CALLABLE_HEAP make_callable(FUNC)
#define CALLABLE CALLABLE_STACK

using namespace HSLL;

using ContainerType = TaskStack<TSIZE>;
ThreadPool<ContainerType> pool;

// Worker thread for batch submission
void bulk_submit_worker()
{
	pool.register_this_thread();

	BatchSubmitter<ContainerType, SUBMIT_BATCH> submitter(pool);

	int remaining = PEER;

	while (remaining > 0)
	{
		std::future<void> f;
		if (submitter.add(CALLABLE))
			remaining--;
		else
			std::this_thread::yield();
	}

	while (submitter.get_size())
	{
		if (!submitter.submit())
			std::this_thread::yield();
	}

	pool.unregister_this_thread();
}

// Worker thread for single task submission
void single_submit_worker()
{
	pool.register_this_thread();

	int remaining = PEER;

	while (remaining > 0)
	{
		if (pool.submit(CALLABLE))
			remaining--;
		else
			std::this_thread::yield();
	}

	pool.unregister_this_thread();
}

// Batch submission test
double test_bulk_submit()
{
	auto start = std::chrono::high_resolution_clock::now();

	std::vector<std::thread> producers;
	producers.reserve(PRODUCER);

	for (int i = 0; i < PRODUCER; ++i)
		producers.emplace_back(bulk_submit_worker);

	for (auto& t : producers)
		t.join();

	pool.drain();
	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double, std::milli> duration = end - start;

	return duration.count();
}

// Single task submission test
double test_single_submit()
{
	auto start = std::chrono::high_resolution_clock::now();

	std::vector<std::thread> producers;
	producers.reserve(PRODUCER);

	for (int i = 0; i < PRODUCER; ++i)
		producers.emplace_back(single_submit_worker);

	for (auto& t : producers)
		t.join();

	pool.drain();
	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double, std::milli> duration = end - start;
	return duration.count();
}

int main()
{
	pool.init(CAPACITY, WORKER, PROCESS_BATCH);

	//std::this_thread::sleep_for(std::chrono::seconds(15));

	long long total_tasks = (long long)(PEER * PRODUCER);

	printf("%-20s: %d\n", "Producer Threads", PRODUCER);
	printf("%-20s: %d\n", "Worker Threads", WORKER);
	printf("%-20s: %d\n", "Submit Batch Size", SUBMIT_BATCH);
	printf("%-20s: %d\n", "Process Batch Size", PROCESS_BATCH);
	printf("%-20s: %d\n", "Container Size", TSIZE);
	printf("%-20s: %s\n", "Task type", FUNC == testA ? taskType[0] : FUNC == testB ? taskType[1] : taskType[2]);
	printf("%-20s: %u\n", "Actual Size", TaskImplTraits<decltype(FUNC)>::size);
	printf("%-20s: %s\n", "Storage method ", ContainerType::is_stored_on_stack<decltype(FUNC)>::value ? "stack" : "heap");
	printf("%-20s: %d\n", "Queue Capacity", CAPACITY);
	printf("%-20s: %d\n", "Tasks per Producer", PEER);
	printf("%-20s: %lld\n", "Total Tasks", total_tasks);

	if (SUBMIT_BATCH == 1)
	{
		// Single task submission test
		double single_time = test_single_submit();
		double single_throughput = total_tasks / (single_time / 1000.0) / 1000000.0;		 // M/s
		double single_time_per_million = (single_time / 1000.0) / (total_tasks / 1000000.0); // s/M

		printf("\n=== Test Results ===\n");
		printf("%-20s: %10.2f ms | %10s: %8.2f M/s | %15s: %.4f s/M\n",
			"Single Submit Time", single_time,
			"Throughput", single_throughput,
			"Time/Million", single_time_per_million);
	}
	else
	{
		// Batch submission test
		double bulk_time = test_bulk_submit();
		double bulk_throughput = total_tasks / (bulk_time / 1000.0) / 1000000.0;		 // M/s
		double bulk_time_per_million = (bulk_time / 1000.0) / (total_tasks / 1000000.0); // s/M

		printf("\n=== Test Results ===\n");
		printf("%-20s: %10.2f ms | %10s: %8.2f M/s | %15s: %.4f s/M\n",
			"Bulk Submit Time", bulk_time,
			"Throughput", bulk_throughput,
			"Time/Million", bulk_time_per_million);
	}

	pool.shutdown();
	return 0;
}