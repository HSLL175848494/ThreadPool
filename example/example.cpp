#include "ThreadPool.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>

using namespace HSLL;
using TaskType = TaskStack<128, 8>;  // 使用128字节的任务存储
ThreadPool<TaskType> globalPool;

// 基本函数示例
void simpleTask(const std::string& msg)
{
	std::cout << "Simple task: " << msg << std::endl;
}

// 带返回值的函数
int calculateSum(int a, int b)
{
	return a + b;
}

// 大型函数（参数多）
void bigTask(int a, double b, const std::string& c, char d, float e)
{
	std::cout << "Big task: " << a << ", " << b << ", " << c << ", " << d << ", " << e << std::endl;
}

// 任务存储在堆上示例
void heapExample()
{
	//创建堆上任务,返回Callable
	auto callable = make_callable([]() {
		std::cout << "Heap task1 completed." << std::endl;
		});

	//Callable隐式转化为TaskStack
	globalPool.enqueue(std::move(callable));

	//创建堆上任务,返回TaskStack
	auto task =TaskType::make_heap([]() {
		std::cout << "Heap task2 completed." << std::endl;
		});

	//直接提交TaskStack
	globalPool.enqueue(std::move(task));
}

// 异步任务示例
void asyncExample()
{
	auto task = make_callable_async<int>(calculateSum, 10, 20);
	auto future = task.get_future();

	//task隐式转化为TaskType类型(构造函数)
	globalPool.enqueue(std::move(task));

	try {
		auto result = future.get();
		std::cout << "Async result: " << result << std::endl;
	}
	catch (const std::exception& e) {
		std::cerr << "Async error: " << e.what() << std::endl;
	}
}

// 异步任务示例2
void asyncExample2()
{
	std::promise<int> promise;
	globalPool.enqueue([&]() {
		promise.set_value(666);
		});
	std::future<int> future = promise.get_future();
	std::cout << "Async result2: " << future.get() << std::endl;
}

// 可取消任务示例
void cancelableExample()
{
	auto task = make_callable_cancelable<void>([]() {
		std::cout << "Cancelable task completed." << std::endl;
		return;
		});

	auto controller = task.get_controller();
	auto future = task.get_future();

	globalPool.enqueue(std::move(task));
	std::this_thread::sleep_for(std::chrono::nanoseconds(150));

	if (controller.cancel())
	{
		std::cout << "Task canceled successfully." << std::endl;
	}
	else
	{
		std::cout << "Task already started." << std::endl;

		try
		{
			future.get();
			std::cout << "Task finished normally." << std::endl;
		}
		catch (const std::exception& e) {
			std::cerr << "Task canceled: " << e.what() << std::endl;
		}
	}
}

// 批量任务提交示例
void batchExample()
{
	BatchSubmitter<TaskType, 10> batch(&globalPool);

	//BatchSubmitter会在容量已满时自动提交,即第10次
	for (int i = 0; i < 10; ++i) {
		batch.emplace([i] {
			std::cout << "Batch task " << i << std::endl;
			});
	}

	std::cout << "Batch tasks submitted" << std::endl;
}

// 任务插入位置控制
void positionControlExample()
{
	// 尾部插入（低优先级,默认）
	globalPool.enqueue<INSERT_POS::TAIL>([] {
		std::cout << "Low priority task (tail)" << std::endl;
		});

	// 头部插入（高优先级）
	globalPool.enqueue<INSERT_POS::HEAD>([] {
		std::cout << "High priority task (head)" << std::endl;
		});
}

// 自动选择存储策略
void storageStrategyExample()
{
	// 小任务 - 栈存储
	TaskType smallTask = TaskType::make_auto([] {
		std::cout << "Small task (stack storage)" << std::endl;
		});

	// 大任务 - 堆存储
	auto lambda = [](const std::string& a, const std::string& b,
		const std::string& c, const std::string& d) {
			std::cout << "Big task (heap storage): "
				<< a << b << c << d << std::endl;
	};

	globalPool.enqueue(std::move(smallTask));

	//TaskType::task_invalid并不需要需要每个参数都对应原值类型,即使是退化类型如
	//(const std::string& ->std::string)也是可以的
	if (!TaskType::task_invalid<decltype(lambda), std::string, std::string, std::string, std::string>::value)
	{
		TaskType bigTask = TaskType::make_auto(lambda, "Large", " parameters", " require", " heap allocation");
		globalPool.enqueue(std::move(bigTask));
	}
}


// 任务属性检查
void taskPropertiesExample()
{
	auto lambda = [](int x) { return x * x; };

	// 创建任务
	TaskType task(lambda, 5);

	// 检查属性
	std::cout << "Task properties:\n"
		<< "Storage size: " << sizeof(task) << " bytes\n"
		<< "Actual size:" << task_stack<decltype(lambda), int>::size << "\n"
		<< "Copyable: " << (task.is_copyable() ? "Yes" : "No") << "\n"
		<< "Moveable: " << (task.is_moveable() ? "Yes" : "No") << "\n"
		<< "Valid for storage: " << (TaskType::task_invalid<decltype(lambda), int>::value ? "Yes" : "No") << "\n";
}

int main() {
	// 初始化线程池：10000任务容量,最小/大线程数1,无批处理
	globalPool.init(10000, 1, 1, 1);

	std::cout << "==== Simple Task Example ====" << std::endl;
	TaskType task(simpleTask, "Hello, World.");
	globalPool.enqueue(std::move(task));
	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	std::cout << "\n==== Heap Task Example ====" << std::endl;
	heapExample();
	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	std::cout << "\n==== Async Result Example ====" << std::endl;
	asyncExample();
	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	std::cout << "\n==== Async Result Example2 ====" << std::endl;
	asyncExample2();
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

	globalPool.exit(true);
	return 0;
}