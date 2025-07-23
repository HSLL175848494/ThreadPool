#include "ThreadPool.hpp"

unsigned int k;

void testA() {

	for (int i = 0; i < 10000; i++)
		k = k * 2 + i;
}

void testB() {

	for (int i = 0; i < 100; i++)
		k = k * 2 + i;
}


void testC() {

}

#define WORKER 8
#define PRODUCER 8
#define SUBMIT_BATCH 32
#define PROCESS_BATCH 32
#define PEER 10000000/PRODUCER
#define TSIZE 24
#define QUEUELEN 8192
#define FUNC testC

using namespace HSLL;
using Type = TaskStack<TSIZE>;
ThreadPool<Type> pool;

// Worker thread for batch submission
void bulk_submit_worker()
{
	BatchSubmitter<Type, SUBMIT_BATCH> submitter(pool);

	int remaining = PEER;

	while (remaining > 0)
	{
		if (submitter.emplace(FUNC))
			remaining--;
		else
			std::this_thread::yield();
	}

	while (submitter.get_size())
	{
		if (!submitter.submit())
			std::this_thread::yield();
	}
}

// Worker thread for single task submission
void single_submit_worker()
{
	int remaining = PEER;

	while (remaining > 0)
	{
		if (pool.emplace(FUNC))
			remaining--;
		else
			std::this_thread::yield();
	}
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

	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double, std::milli> duration = end - start;
	return duration.count();
}


int main()
{
	pool.init(QUEUELEN, WORKER, WORKER, PROCESS_BATCH);

	const long long total_tasks = static_cast<long long>(PEER) * PRODUCER;

	printf("%-20s: %d\n", "Submit Batch Size", SUBMIT_BATCH);
	printf("%-20s: %d\n", "Process Batch Size", PROCESS_BATCH);
	printf("%-20s: %d\n", "Task Container Size", TSIZE);
	printf("%-20s: %u\n", "Actual Task Size", task_stack<decltype(testC)>::size);
	printf("%-20s: %d\n", "Producer Threads", PRODUCER);
	printf("%-20s: %d\n", "Worker Threads", WORKER);
	printf("%-20s: %d\n", "Queue Length", QUEUELEN);
	printf("%-20s: %llu\n", "Max memory Usage", pool.get_max_usage());
	printf("%-20s: %d\n", "Tasks per Producer", PEER);
	printf("%-20s: %lld\n", "Total Tasks", total_tasks);

	// Single task submission test
	double single_time = test_single_submit();
	double single_throughput = total_tasks / (single_time / 1000.0) / 1000000.0;		 // M/s
	double single_time_per_million = (single_time / 1000.0) / (total_tasks / 1000000.0); // s/M

	// Batch submission test
	double bulk_time = test_bulk_submit();
	double bulk_throughput = total_tasks / (bulk_time / 1000.0) / 1000000.0;		 // M/s
	double bulk_time_per_million = (bulk_time / 1000.0) / (total_tasks / 1000000.0); // s/M

	printf("\n=== Test Results ===\n");

	printf("%-20s: %10.2f ms | %10s: %8.2f M/s | %15s: %.4f s/M\n",
		"Single Submit Time", single_time,
		"Throughput", single_throughput,
		"Time/Million", single_time_per_million);

	printf("%-20s: %10.2f ms | %10s: %8.2f M/s | %15s: %.4f s/M\n",
		"Bulk Submit Time", bulk_time,
		"Throughput", bulk_throughput,
		"Time/Million", bulk_time_per_million);

	printf("%-20s: %10.5f\n", "Ratio (Bulk/Single)", single_time / bulk_time);

	pool.exit(true);
	return 0;
}