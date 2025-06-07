#include <iostream>
#include <chrono>
#include <vector>
#include <thread>
#include <cstdio>  // ��� printf ֧��
#include "ThreadPool.hpp"

using namespace HSLL;

#define WORKER 1
#define PRODUCER 1
#define TSIZE 16
#define PEER 10000000  // ��������������ƶ��ֲ���
#define QUEUELEN 10000
#define SUBMIT_BATCH 1
#define PROCESS_BATCH 1

ThreadPool<TaskStack<TSIZE>> pool;

// ������
void test() {}

// �����ύ�����߳�
void bulk_submit_worker()
{
    unsigned char buf[SUBMIT_BATCH * sizeof(TaskStack<TSIZE>)];
    for (int i = 0; i < SUBMIT_BATCH; i++)
        new (buf + i * sizeof(TaskStack<TSIZE>)) TaskStack<TSIZE>(test);

    int remaining = PEER;
    while (remaining > 0)
    {
        //���Ϊ�������ӿڲ��ϳ���
        remaining -= pool.wait_appendBulk<COPY>(
            reinterpret_cast<TaskStack<TSIZE>*>(buf),
            std::min(SUBMIT_BATCH, remaining));
    }
}

// �������ύ�����߳�
void single_submit_worker()
{
    int remaining = PEER;
    while (remaining > 0)
    {
        //���Ϊ�������ӿڲ��ϳ���
        pool.wait_emplace(test);
        remaining--;
    }
}

// �����ύ����
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

// �������ύ����
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

    printf("\n=== ���ò��� ===\n");
    printf("%-16s: %d\n", "�����ύ��С", SUBMIT_BATCH);
    printf("%-16s: %d\n", "�������С", PROCESS_BATCH);
    printf("%-16s: %d\n", "����ռ��С", TSIZE);
    printf("%-16s: %zu\n", "ʵ�������С", stack_tsize<decltype(test)>::value);
    printf("%-16s: %d\n", "�������߳�", PRODUCER);
    printf("%-16s: %d\n", "�������߳�", WORKER);
    printf("%-16s: %d\n", "���г���", QUEUELEN);
    printf("%-16s: %lld bytes\n", "�ڴ�ʹ��", static_cast<long long>((8 + TSIZE) * QUEUELEN * WORKER));
    printf("%-16s: %d\n", "��������������", PEER);
    printf("%-16s: %lld\n", "��������", total_tasks);

    // �������ύ����
    double single_time = test_single_submit();
    double single_throughput = total_tasks / (single_time / 1000.0) / 1000000.0; // M/s
    double single_time_per_million = (single_time / 1000.0) / (total_tasks / 1000000.0); // s/M

    // �����ύ����
    double bulk_time = test_bulk_submit();
    double bulk_throughput = total_tasks / (bulk_time / 1000.0) / 1000000.0; // M/s
    double bulk_time_per_million = (bulk_time / 1000.0) / (total_tasks / 1000000.0); // s/M

    printf("\n=== ���Խ�� ===\n");
    printf("%-20s: %10.2f ms | %10s: %8.2f M/s | %15s: %.4f s/M\n",
        "�����ύ���Ժ�ʱ", bulk_time,
        "������", bulk_throughput,
        "ÿ���������ʱ", bulk_time_per_million);

    printf("%-20s: %10.2f ms | %10s: %8.2f M/s | %15s: %.4f s/M\n",
        "�������ύ���Ժ�ʱ", single_time,
        "������", single_throughput,
        "ÿ���������ʱ", single_time_per_million);

    printf("%-20s: %10.5f\n","���ܱ�(����/������)", single_time / bulk_time);

    return 0;
}