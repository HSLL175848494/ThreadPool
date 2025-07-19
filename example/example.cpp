#include "ThreadPool.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>

using namespace HSLL;
using TaskType = TaskStack<128, 8>;  // ʹ��128�ֽڵ�����洢
ThreadPool<TaskType> globalPool;

// ��������ʾ��
void simpleTask(const std::string& msg)
{
	std::cout << "Simple task: " << msg << std::endl;
}

// ������ֵ�ĺ���
int calculateSum(int a, int b)
{
	return a + b;
}

// ���ͺ����������ࣩ
void bigTask(int a, double b, const std::string& c, char d, float e)
{
	std::cout << "Big task: " << a << ", " << b << ", " << c << ", " << d << ", " << e << std::endl;
}

// ����洢�ڶ���ʾ��
void heapExample()
{
	//������������,����Callable
	auto callable = make_callable([]() {
		std::cout << "Heap task1 completed." << std::endl;
		});

	//Callable��ʽת��ΪTaskStack
	globalPool.enqueue(std::move(callable));

	//������������,����TaskStack
	auto task =TaskType::make_heap([]() {
		std::cout << "Heap task2 completed." << std::endl;
		});

	//ֱ���ύTaskStack
	globalPool.enqueue(std::move(task));
}

// �첽����ʾ��
void asyncExample()
{
	auto task = make_callable_async<int>(calculateSum, 10, 20);
	auto future = task.get_future();

	//task��ʽת��ΪTaskType����(���캯��)
	globalPool.enqueue(std::move(task));

	try {
		auto result = future.get();
		std::cout << "Async result: " << result << std::endl;
	}
	catch (const std::exception& e) {
		std::cerr << "Async error: " << e.what() << std::endl;
	}
}

// �첽����ʾ��2
void asyncExample2()
{
	std::promise<int> promise;
	globalPool.enqueue([&]() {
		promise.set_value(666);
		});
	std::future<int> future = promise.get_future();
	std::cout << "Async result2: " << future.get() << std::endl;
}

// ��ȡ������ʾ��
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

// ���������ύʾ��
void batchExample()
{
	BatchSubmitter<TaskType, 10> batch(&globalPool);

	//BatchSubmitter������������ʱ�Զ��ύ,����10��
	for (int i = 0; i < 10; ++i) {
		batch.emplace([i] {
			std::cout << "Batch task " << i << std::endl;
			});
	}

	std::cout << "Batch tasks submitted" << std::endl;
}

// �������λ�ÿ���
void positionControlExample()
{
	// β�����루�����ȼ�,Ĭ�ϣ�
	globalPool.enqueue<INSERT_POS::TAIL>([] {
		std::cout << "Low priority task (tail)" << std::endl;
		});

	// ͷ�����루�����ȼ���
	globalPool.enqueue<INSERT_POS::HEAD>([] {
		std::cout << "High priority task (head)" << std::endl;
		});
}

// �Զ�ѡ��洢����
void storageStrategyExample()
{
	// С���� - ջ�洢
	TaskType smallTask = TaskType::make_auto([] {
		std::cout << "Small task (stack storage)" << std::endl;
		});

	// ������ - �Ѵ洢
	auto lambda = [](const std::string& a, const std::string& b,
		const std::string& c, const std::string& d) {
			std::cout << "Big task (heap storage): "
				<< a << b << c << d << std::endl;
	};

	globalPool.enqueue(std::move(smallTask));

	//TaskType::task_invalid������Ҫ��Ҫÿ����������Ӧԭֵ����,��ʹ���˻�������
	//(const std::string& ->std::string)Ҳ�ǿ��Ե�
	if (!TaskType::task_invalid<decltype(lambda), std::string, std::string, std::string, std::string>::value)
	{
		TaskType bigTask = TaskType::make_auto(lambda, "Large", " parameters", " require", " heap allocation");
		globalPool.enqueue(std::move(bigTask));
	}
}


// �������Լ��
void taskPropertiesExample()
{
	auto lambda = [](int x) { return x * x; };

	// ��������
	TaskType task(lambda, 5);

	// �������
	std::cout << "Task properties:\n"
		<< "Storage size: " << sizeof(task) << " bytes\n"
		<< "Actual size:" << task_stack<decltype(lambda), int>::size << "\n"
		<< "Copyable: " << (task.is_copyable() ? "Yes" : "No") << "\n"
		<< "Moveable: " << (task.is_moveable() ? "Yes" : "No") << "\n"
		<< "Valid for storage: " << (TaskType::task_invalid<decltype(lambda), int>::value ? "Yes" : "No") << "\n";
}

int main() {
	// ��ʼ���̳߳أ�10000��������,��С/���߳���1,��������
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