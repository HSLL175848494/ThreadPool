#include "ThreadPool.hpp"
#include <iostream>

using namespace HSLL;
using TaskType = TaskStack<128, 8>;  // 128-byte task storage
ThreadPool<TaskType> globalPool;

// Basic function example
void simpleTask(const std::string& msg)
{
    std::cout << "Simple task: " << msg << std::endl;
}

// Function with return value
int calculateSum(int a, int b)
{
    return a + b;
}

// Large function (multiple parameters)
void bigTask(int a, double b, const std::string& c, char d, float e)
{
    std::cout << "Big task: " << a << ", " << b << ", " << c << ", " << d << ", " << e << std::endl;
}

// Heap-stored task example
void heapExample()
{
    // Create heap task, returns Callable
    auto callable = make_callable([]() {
        std::cout << "Heap task1 completed." << std::endl;
        });

    globalPool.submit(std::move(callable));
}

// Asynchronous task example
void asyncExample()
{
    auto task = make_callable_async(calculateSum, 333, 333);
    auto future = task.get_future();

    globalPool.submit(std::move(task));

    auto result = future.get();
    std::cout << "Async result1: " << result << std::endl;
}

// Cancelable task example
void cancelableExample()
{
    auto callable = make_callable_cancelable([]() {
        std::cout << "Cancelable task completed." << std::endl;
        return;
        });

    auto controller = callable.get_controller();
    globalPool.submit(std::move(callable));
    std::this_thread::sleep_for(std::chrono::nanoseconds(150));

    if (controller.cancel())
    {
        std::cout << "Task canceled successfully." << std::endl;

        try
        {
            controller.get();
            std::cout << "Task finished normally." << std::endl;
        }
        catch (const std::exception& e) {
            std::cerr << "Task exception: " << e.what() << std::endl;
        }
    }
    else
    {
        std::cout << "Task already started." << std::endl;

        try
        {
            controller.get();
            std::cout << "Task finished normally." << std::endl;
        }
        catch (const std::exception& e) {
            std::cerr << "Task exception: " << e.what() << std::endl;
        }
    }
}

// Batch task submission example
void batchExample()
{
    BatchSubmitter<TaskType, 10> batch(globalPool);

    // BatchSubmitter auto-submits when capacity is reached (10th task)
    for (int i = 0; i < 10; ++i) {
        batch.add([i] {
            std::cout << "Batch task " << i << std::endl;
            });
    }

    std::cout << "Batch tasks submitted" << std::endl;
}

// Task insertion position control
void positionControlExample()
{
    // Tail insertion (low priority, default)
    globalPool.submit<INSERT_POS::TAIL>([] {
        std::cout << "Low priority task (tail)" << std::endl;
        });

    // Head insertion (high priority)
    globalPool.submit<INSERT_POS::HEAD>([] {
        std::cout << "High priority task (head)" << std::endl;
        });
}

// Automatic storage strategy selection
void storageStrategyExample()
{
    // Small task - stack storage
    auto lambda_small = [] {
        std::cout << "Small task (stack storage)" << std::endl;
    };

    // Large task - heap storage
    auto lambda_big = [](const std::string& a, const std::string& b,
        const std::string& c, const std::string& d) {
            std::cout << "Big task (heap storage): "
                << a << b << c << d << std::endl;
    };

    TaskType smallTask(lambda_small);
    TaskType bigTask(lambda_big, "Large", " parameters", " require", " heap allocation.");

    globalPool.submit(std::move(smallTask));
    globalPool.submit(std::move(bigTask));
}

// Task property inspection
void taskPropertiesExample()
{
    auto lambda = [](int x) { return x * x; };
    TaskType task(lambda, 5);

    // Inspect properties
    std::cout << "Task properties:\n"
        << "Storage size: " << sizeof(task) << " bytes\n"
        << "Actual size:" << TaskImplTraits<decltype(lambda), int>::size << "\n"
        << "Is stored on stack: " << (TaskType::is_stored_on_stack<decltype(lambda), int>::value ? "Yes" : "No") << "\n";
}

// Thread registration example
void threadRegisterExample()
{
    /*
    * Register thread to pool's group allocator
    * Each registered thread gets a dedicated queue group (RoundRobinGroup)
    * Queue group contains one or more task queues (TPBlockQueue)
    *
    * Example: 3 queues(1,2,3), 4 producer threads(A,B,C,D)
    *
    * ---------------------------------------------------------------------
    * Unregistered queue assignment uses global round-robin via atomic index
    *
    * Queue Assignment  1     2     3     1     2     3    ...
    * Submission Log    A-sub B-sub C-sub A-sub B-sub C-sub ...
    *
    * Causes frequent cache invalidation and atomic contention
    *----------------------------------------------------------------------
    * Registered queue assignment:
    *
    *   A: Dedicated group [1]
    *   B: Dedicated group [2]
    *   C: Dedicated group [3]
    *   D: Dedicated group [1,2,3] (multi-queue group)
    *
    * Groups with multiple queues switch queues after threshold submissions
    */

    globalPool.register_this_thread(); // Register current thread

    /* Task submission operations */

    // Unregister when done to rebalance queue groups
    globalPool.unregister_this_thread();
    std::cout << "Thread register example" << std::endl;
}

// Custom memory allocator (HeapCallable uses tp_smart_ptr)
void customAllocatorExample()
{
    // Parameter: Custom allocator implementing AllocatorBase
    // Uses malloc/free if nullptr
    set_tp_smart_ptr_allocator();
}

int main()
{
    // Initialize pool: 10000 task capacity, min/max threads=1, no batch processing
    globalPool.init(10000, 1, 1, 1);

    std::cout << "==== Simple Task Example ====" << std::endl;
    TaskType task(simpleTask, "Hello, World.");
    globalPool.submit(std::move(task));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cout << "\n==== Heap Task Example ====" << std::endl;
    heapExample();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cout << "\n==== Async Result Example ====" << std::endl;
    asyncExample();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cout << "\n==== Cancelable Task Example ====" << std::endl;
    cancelableExample();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cout << "\n==== Batch Processing Example ====" << std::endl;
    batchExample();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cout << "\n==== Position Control Example ====" << std::endl;
    positionControlExample();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cout << "\n==== Storage Strategy Example ====" << std::endl;
    storageStrategyExample();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cout << "\n==== Task Properties Example ====" << std::endl;
    taskPropertiesExample();

    std::cout << "\n==== Thread register Example ====" << std::endl;
    threadRegisterExample();

    globalPool.exit(true);
    return 0;
}