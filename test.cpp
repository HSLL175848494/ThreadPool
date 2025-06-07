#include <iostream>
#include <chrono>
#include <vector>
#include <thread>
#include <cstdio>  // 添加 printf 支持
#include "ThreadPool.hpp"

using namespace HSLL;

#define WORKER 1
#define PRODUCER 1
#define TSIZE 16
#define PEER 10000000  // 提高任务数量近似多轮测试
#define QUEUELEN 10000
#define SUBMIT_BATCH 1
#define PROCESS_BATCH 1

ThreadPool<TaskStack<TSIZE>> pool;

// 空任务
void test() {}

// 批量提交工作线程
void bulk_submit_worker()
{
    unsigned char buf[SUBMIT_BATCH * sizeof(TaskStack<TSIZE>)];
    for (int i = 0; i < SUBMIT_BATCH; i++)
        new (buf + i * sizeof(TaskStack<TSIZE>)) TaskStack<TSIZE>(test);

    int remaining = PEER;
    while (remaining > 0)
    {
        //或改为非阻塞接口不断尝试
        remaining -= pool.wait_appendBulk<COPY>(
            reinterpret_cast<TaskStack<TSIZE>*>(buf),
            std::min(SUBMIT_BATCH, remaining));
    }
}

// 单任务提交工作线程
void single_submit_worker()
{
    int remaining = PEER;
    while (remaining > 0)
    {
        //或改为非阻塞接口不断尝试
        pool.wait_emplace(test);
        remaining--;
    }
}

// 批量提交测试
double test_bulk_submit()
{
    pool.init(QUEUELEN, WORKER, PROCESS_BATCH);

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

// 单任务提交测试
double test_single_submit()
{
    pool.init(QUEUELEN, WORKER, PROCESS_BATCH);
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

    printf("\n=== 配置参数 ===\n");
    printf("%-16s: %d\n", "批量提交大小", SUBMIT_BATCH);
    printf("%-16s: %d\n", "批处理大小", PROCESS_BATCH);
    printf("%-16s: %d\n", "任务空间大小", TSIZE);
    printf("%-16s: %zu\n", "实际任务大小", stack_tsize<decltype(test)>::value);
    printf("%-16s: %d\n", "生产者线程", PRODUCER);
    printf("%-16s: %d\n", "工作者线程", WORKER);
    printf("%-16s: %d\n", "队列长度", QUEUELEN);
    printf("%-16s: %lld bytes\n", "内存使用", static_cast<long long>((8 + TSIZE) * QUEUELEN * WORKER));
    printf("%-16s: %d\n", "单生产者任务数", PEER);
    printf("%-16s: %lld\n", "总任务数", total_tasks);

    // 单任务提交测试
    double single_time = test_single_submit();
    double single_throughput = total_tasks / (single_time / 1000.0) / 1000000.0; // M/s
    double single_time_per_million = (single_time / 1000.0) / (total_tasks / 1000000.0); // s/M

    // 批量提交测试
    double bulk_time = test_bulk_submit();
    double bulk_throughput = total_tasks / (bulk_time / 1000.0) / 1000000.0; // M/s
    double bulk_time_per_million = (bulk_time / 1000.0) / (total_tasks / 1000000.0); // s/M

    printf("\n=== 测试结果 ===\n");
    printf("%-20s: %10.2f ms | %10s: %8.2f M/s | %15s: %.4f s/M\n",
        "批量提交测试耗时", bulk_time,
        "吞吐量", bulk_throughput,
        "每百万任务耗时", bulk_time_per_million);

    printf("%-20s: %10.2f ms | %10s: %8.2f M/s | %15s: %.4f s/M\n",
        "单任务提交测试耗时", single_time,
        "吞吐量", single_throughput,
        "每百万任务耗时", single_time_per_million);

    printf("%-20s: %10.5f\n","性能比(批量/单任务)", single_time / bulk_time);

    return 0;
}