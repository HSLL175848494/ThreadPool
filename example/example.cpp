#include "ThreadPool.hpp"
#include <iostream>

using namespace HSLL;
using TaskType = TaskStack<64, 8>;
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

// 任务存储在堆上示例
void heapExample()
{
	//创建堆上任务,返回Callable
	auto callable = make_callable([]() {
		std::cout << "Heap task1 completed." << std::endl;
		});

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

// 可取消任务示例
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
	TaskType bigTask(lambda_big, std::string("Large"), std::string(" parameters"), std::string(" require"), std::string(" heap allocation."));

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
	* ---------------------------------------------------------------------
	*/

	globalPool.register_this_thread(); // 注册当前线程

	/* 添加任务操作 */

	// 注意:当前线程不再使用线程池时需及时注销以重新调整线程池的队列组分配
	globalPool.unregister_this_thread();
	std::cout << "Thread register example" << std::endl;
}

//自定义内存申请释放器（HeapCallable依赖于tp_smart_ptr）
void customAllocatorExample()
{
	//参数:传入一个实现了AllocatorBase虚函数的子类
	//为空时用默认内存申请释放器(malloc/free)
	set_tp_smart_ptr_allocator();
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