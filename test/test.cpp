#include "ThreadPool.hpp"

using namespace HSLL;

#define WORKER 8
#define PRODUCER 1
#define SUBMIT_BATCH 1
#define PROCESS_BATCH 1
#define PEER 10000000
#define TSIZE 16
#define QUEUELEN 10000

ThreadPool<TaskStack<TSIZE>> pool;
using task_type = TaskStack<TSIZE>;

unsigned int k;

// Empty task function
void test() {

	for (int i = 0; i < 10000; i++)
		k = k*2+i;
}

// Worker thread for batch submission
void bulk_submit_worker()
{
	unsigned char buf[SUBMIT_BATCH * sizeof(task_type)];
	for (int i = 0; i < SUBMIT_BATCH; i++)
		new (buf + i * sizeof(task_type)) task_type(test);

	int remaining = PEER;
	while (remaining > 0)
	{
		remaining -= pool.wait_enqueue_bulk<COPY>(
			reinterpret_cast<task_type*>(buf),
			std::min(SUBMIT_BATCH, remaining));
	}
}

// Worker thread for single task submission
void single_submit_worker()
{
	int remaining = PEER;
	while (remaining > 0)
	{
		pool.wait_emplace(test);
		remaining--;
	}
}

// Batch submission test
double test_bulk_submit()
{
	pool.init(QUEUELEN,1, WORKER, PROCESS_BATCH);

	auto start = std::chrono::high_resolution_clock::now();

	std::vector<std::thread> producers;
	producers.reserve(PRODUCER);

	for (int i = 0; i < PRODUCER; ++i)
		producers.emplace_back(bulk_submit_worker);

	for (auto& t : producers)
		t.join();

	pool.exit(true);
	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double, std::milli> duration = end - start;

	return duration.count();
}

// Single task submission test
double test_single_submit()
{
	pool.init(QUEUELEN,1,WORKER, PROCESS_BATCH);
	auto start = std::chrono::high_resolution_clock::now();

	std::vector<std::thread> producers;
	producers.reserve(PRODUCER);

	for (int i = 0; i < PRODUCER; ++i)
		producers.emplace_back(single_submit_worker);

	for (auto& t : producers)
		t.join();

	pool.exit(true);

	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double, std::milli> duration = end - start;
	return duration.count();
}

int main()
{	
	const long long total_tasks = static_cast<long long>(PEER) * PRODUCER;
	printf("\n=== Configuration Parameters ===\n");
	printf("%-20s: %d\n", "Submit Batch Size", SUBMIT_BATCH);
	printf("%-20s: %d\n", "Process Batch Size", PROCESS_BATCH);
	printf("%-20s: %d\n", "Task Space Size", TSIZE);
	printf("%-20s: %u\n", "Actual Task Size", task_stack<decltype(test)>::size);
	printf("%-20s: %d\n", "Producer Threads", PRODUCER);
	printf("%-20s: %d\n", "Worker Threads", WORKER);
	printf("%-20s: %d\n", "Queue Length", QUEUELEN);
	printf("%-20s: %lu\n", "Max memory Usage", sizeof(task_type) * QUEUELEN * WORKER);
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
		"Bulk Submit Time", bulk_time,
		"Throughput", bulk_throughput,
		"Time/Million", bulk_time_per_million);

	printf("%-20s: %10.2f ms | %10s: %8.2f M/s | %15s: %.4f s/M\n",
		"Single Submit Time", single_time,
		"Throughput", single_throughput,
		"Time/Million", single_time_per_million);

	printf("%-20s: %10.5f\n", "Ratio (Bulk/Single)", single_time / bulk_time);

	std::this_thread::sleep_for(std::chrono::seconds(1));

	return 0;
}