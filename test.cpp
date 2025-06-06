#include <iostream>
#include <chrono>
#include <vector>
#include <thread>
#include "ThreadPool.hpp"

using namespace HSLL;

#define ROUND 10
#define WORKER 4
#define PRODUCER 4
#define ALIGEN 24
#define PEER 1000000  
#define QUEUELEN 10000
#define SUBMIT_BATCH 32
#define PROCESS_BATCH 32

ThreadPool<TaskStack<ALIGEN>> pool;

// Time-consuming task
/*
void test(int a, int b) {
    static int w = 0;
    for (int i = 0; i < 10000; i++)
        w++;
}
*/

// Empty task
void test(int a, int b) {}

// Worker for batch submission
void bulk_submit_worker() {
    unsigned char buf[SUBMIT_BATCH * sizeof(TaskStack<ALIGEN>)];
    int remaining = PEER;
    while (remaining > 0) {
        for (int i = 0; i < std::min(SUBMIT_BATCH, remaining); i++) {
            new (buf + i * sizeof(TaskStack<ALIGEN>)) TaskStack<ALIGEN>(test, 2, 1);
        }

        unsigned int submitted = pool.append_bulk(
            reinterpret_cast<TaskStack<ALIGEN>*>(buf),
            std::min(SUBMIT_BATCH, remaining)
        );

        if (submitted) {
            remaining -= submitted;
        }
        else {
            std::this_thread::yield();
        }
    }
}

// Worker for single task submission
void single_submit_worker() {
    int remaining = PEER;
    while (remaining > 0) {
        TaskStack<ALIGEN> task(test, 2, 1);
        if (pool.append(task)) {
            remaining--;
        }
        else {
            std::this_thread::yield();
        }
    }
}

// Batch submission test
double test_bulk_submit() {
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> producers;
    producers.reserve(PRODUCER);
    for (int i = 0; i < PRODUCER; ++i) {
        producers.emplace_back(bulk_submit_worker);
    }

    for (auto& t : producers) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end - start;
    return duration.count();
}

// Single submission test
double test_single_submit() {
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> producers;
    producers.reserve(PRODUCER);
    for (int i = 0; i < PRODUCER; ++i) {
        producers.emplace_back(single_submit_worker);
    }

    for (auto& t : producers) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end - start;
    return duration.count();
}

int main()
{
    pool.init(QUEUELEN, WORKER, PROCESS_BATCH);
    double total_bulk_time = 0.0;
    double total_single_time = 0.0;
    const long long total_tasks = static_cast<long long>(PEER) * PRODUCER; // Total tasks

    std::cout << "\n=== Configuration Parameters ==="
        << "\nBatch submit size: " << SUBMIT_BATCH
        << "\nBatch process size: " << PROCESS_BATCH
        << "\nSingle task space size: " << ALIGEN
        << "\nSingle task actual size: " << stack_tsize_v<decltype(test), int, int>
        << "\nProducer threads: " << PRODUCER
        << "\nWorker threads: " << WORKER
        << "\nQueue length: " << QUEUELEN
        << "\nMemory usage: " << (8 + ALIGEN) * QUEUELEN * WORKER
        << "\nTasks per producer: " << PEER
        << "\nTotal tasks/round: " << total_tasks
        << "\nStarting performance test (rounds=" << ROUND << ")...\n";

    // Test batch submission
    std::cout << "\n===== Batch Submission Test =====\n";
    for (int i = 0; i < ROUND; ++i) {
        double time_ms = test_bulk_submit();
        total_bulk_time += time_ms;
        unsigned long long throughput = total_tasks / (time_ms / 1000.0);
        std::cout << "Round " << i + 1 << ": "
            << time_ms << " ms | "
            << "Throughput: " << throughput << " tasks/sec\n";
    }

    // Test single submission
    std::cout << "\n===== Single Submission Test =====\n";
    for (int i = 0; i < ROUND; ++i) {
        double time_ms = test_single_submit();
        total_single_time += time_ms;
        unsigned long long throughput = total_tasks / (time_ms / 1000.0);
        std::cout << "Round " << i + 1 << ": "
            << time_ms << " ms | "
            << "Throughput: " << throughput << " tasks/sec\n";
    }

    pool.exit(false);

    // Calculate averages
    double avg_bulk = total_bulk_time / ROUND;
    double avg_single = total_single_time / ROUND;
    unsigned long long avg_throughput_bulk = total_tasks / (avg_bulk / 1000.0);
    unsigned long long avg_throughput_single = total_tasks / (avg_single / 1000.0);

    std::cout << "\n=== Test Results ==="
        << "\nBatch - Avg time: " << avg_bulk / PRODUCER << " ms/M | Avg throughput: "
        << avg_throughput_bulk << " tasks/sec"
        << "\nSingle - Avg time: " << avg_bulk / PRODUCER << " ms/M | Avg throughput: "
        << avg_throughput_single << " tasks/sec"
        << "\nPerformance ratio (Batch/Single): " << avg_single / avg_bulk << std::endl;

    getchar();
    return 0;
}