#include "ThreadPool.hpp"
#include <iostream>

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

// 大型函数（较多参数）
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
	globalPool.submit(std::move(callable));
}

// 异步任务示例
void asyncExample()
{
	auto task = make_callable_async(calculateSum, 333, 333);
	auto future = task.get_future();

	globalPool.submit(std::move(task));

	auto result = future.get();
	std::cout << "Async result1: " << result << std::endl;
}

// 异步任务示例1(效率高于堆上任务,但需要确保任务返回前promise有效)
void asyncExample2()
{
	std::promise<int> promise;

	globalPool.submit([&]() {
		promise.set_value(777);
		});

	std::future<int> future = promise.get_future();
	std::cout << "Async result3: " << future.get() << std::endl;
}

// 可取消任务示例
void cancelableExample1()
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

// 可取消任务示例2(效率高于堆上任务,但需要确保任务执行前Cancelable有效)
void cancelableExample2()
{
	Cancelable<void> cancelable;

	globalPool.submit([&]() {

		if (cancelable.enter())//尝试进入临界区
		{
			try
			{
				std::cout << "Cancelable task completed." << std::endl;
				cancelable.set_value();
			}
			catch (const std::exception&)
			{
				cancelable.set_exception(std::make_exception_ptr(std::current_exception()));
			}
		}
		return;
		});

	std::this_thread::sleep_for(std::chrono::nanoseconds(150));

	if (cancelable.cancel())
	{
		std::cout << "Task canceled successfully." << std::endl;

		try
		{
			cancelable.get();
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
			cancelable.get();
			std::cout << "Task finished normally." << std::endl;
		}
		catch (const std::exception& e) {
			std::cerr << "Task exception: " << e.what() << std::endl;
		}
	}
}

// 批量任务提交示例
void batchExample()
{
	BatchSubmitter<TaskType, 10> batch(globalPool);

	//BatchSubmitter会在容量已满时自动提交,即第10次
	for (int i = 0; i < 10; ++i) {
		batch.add([i] {
			std::cout << "Batch task " << i << std::endl;
			});
	}

	std::cout << "Batch tasks submitted" << std::endl;
}

// 任务插入位置控制
void positionControlExample()
{
	// 尾部插入（低优先级,默认）
	globalPool.submit<INSERT_POS::TAIL>([] {
		std::cout << "Low priority task (tail)" << std::endl;
		});

	// 头部插入（高优先级）
	globalPool.submit<INSERT_POS::HEAD>([] {
		std::cout << "High priority task (head)" << std::endl;
		});
}

// 自动选择存储策略
void storageStrategyExample()
{
	// 小任务 - 栈存储
	auto lambda_small = [] {
		std::cout << "Small task (stack storage)" << std::endl;
	};

	// 大任务 - 堆存储
	auto lambda_big = [](const std::string& a, const std::string& b,
		const std::string& c, const std::string& d) {
			std::cout << "Big task (heap storage): "
				<< a << b << c << d << std::endl;
	};

	TaskType smallTask(lambda_small);
	TaskType bigTask(lambda_big, "Large", " parameters", " require", " heap allocation.");

	//if (TaskType::is_stored_on_stack<decltype(lambda_small)>::value)
	globalPool.submit(std::move(smallTask));

	//if (!TaskType::is_stored_on_stack<decltype(lambda_big), std::string, std::string, std::string, std::string>::value)
	globalPool.submit(std::move(bigTask));
}


// 任务属性检查
void taskPropertiesExample()
{
	auto lambda = [](int x) { return x * x; };
	TaskType task(lambda, 5);

	// 检查属性
	std::cout << "Task properties:\n"
		<< "Storage size: " << sizeof(task) << " bytes\n"
		<< "Actual size:" << TaskImplTraits<decltype(lambda), int>::size << "\n"
		<< "Is stored on stack: " << (TaskType::is_stored_on_stack<decltype(lambda), int>::value ? "Yes" : "No") << "\n";
}

// 线程注册示例
void threadRegisterExample()
{
	/*
	* 注册线程到线程池的分组分配器
	* 每个注册线程会被动态分配一个专属队列组（RoundRobinGroup）
	* 队列组包含一个或多个任务队列（TPBlockQueue）
	*
	* 示例：3个队列(1,2,3)，4个生产线程(A,B,C,D)
	*
	* ---------------------------------------------------------------------
	* 未注册时队列分配是根据内部index原子变量进行全局轮询
	*
	* 队列分配	1	 2      3     1     2     3    ...
	* 提交记录 A提交 B提交 C提交 A提交 B提交 C提交 ...
	*
	* 该方案会导致缓存频繁地失效,单个index原子变量也会性能瓶颈
	*----------------------------------------------------------------------
	* 注册后队列分配:
	*
	*   A: 专属队列组[1]
	*   B: 专属队列组[2]
	*   C: 专属队列组[3]
	*   D: 专属队列组[1,2,3] (多队列组)
	*
	* 对于内包含多个队列的队列组，其在提交任务数量达到阈值时会切换到另一个队列（轮询）
	* 对于不同的生产者，其生产速率可能不同，因此这种方案的负载均衡能力会若于全局轮询方案。
	* 但任务窃取机制和多级队列选取缓解了这一点。
	*
	* 三级队列选取策略(队列已满则选取下级队列):
	* 一级:队列组内当前轮询队列
	* 二级:队列组内其他队列
	* 三级:不包括在队列组的其它队列
	* ---------------------------------------------------------------------
	*/

	globalPool.register_this_thread(); // 注册当前线程

	/* 添加任务操作 */

	// 注意:当前线程不再使用线程池时需及时注销以重新调整线程池的队列组分配
	globalPool.unregister_this_thread();
	std::cout << "Thread register example" << std::endl;
}

int main()
{
	// 初始化线程池：10000任务容量，最小/大线程数1,无批处理
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

	std::cout << "\n==== Async Result Example2 ====" << std::endl;
	asyncExample2();
	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	std::cout << "\n==== Cancelable Task Example ====" << std::endl;
	cancelableExample1();
	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	std::cout << "\n==== Cancelable Task Example2 ====" << std::endl;
	cancelableExample2();
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