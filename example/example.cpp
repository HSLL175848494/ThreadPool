#include "ThreadPool.hpp"
#include <iostream>

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

// ���ͺ������϶������
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
}

// �첽����ʾ��
void asyncExample()
{
	auto task = make_callable_async(calculateSum, 333, 333);
	auto future = task.get_future();

	//task��ʽת��ΪTaskType����(���캯��)
	globalPool.enqueue(std::move(task));

	auto result = future.get();
	std::cout << "Async result1: " << result << std::endl;
}

// �첽����ʾ��1(Ч�ʸ��ڶ�������,����Ҫȷ�����񷵻�ǰpromise��Ч)
void asyncExample2()
{
	std::promise<int> promise;

	globalPool.enqueue([&]() {
		promise.set_value(777);
		});

	std::future<int> future = promise.get_future();
	std::cout << "Async result3: " << future.get() << std::endl;
}

// ��ȡ������ʾ��
void cancelableExample1()
{
	auto task = make_callable_cancelable([]() {
		std::cout << "Cancelable task completed." << std::endl;
		return;
		});

	auto controller = task.get_controller();
	globalPool.enqueue(std::move(task));
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

// ��ȡ������ʾ��2(Ч�ʸ��ڶ�������,����Ҫȷ������ִ��ǰCancelable��Ч)
void cancelableExample2()
{
	Cancelable<void> cancelable;

	globalPool.enqueue([&]() {

		if (cancelable.enter())//���Խ����ٽ���
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

// ���������ύʾ��
void batchExample()
{
	BatchSubmitter<TaskType, 10> batch(globalPool);

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
	auto lambda_small = [] {
		std::cout << "Small task (stack storage)" << std::endl;
	};

	// ������ - �Ѵ洢
	auto lambda_big = [](const std::string& a, const std::string& b,
		const std::string& c, const std::string& d) {
			std::cout << "Big task (heap storage): "
				<< a << b << c << d << std::endl;
	};

	TaskType smallTask(lambda_small);
	TaskType bigTask(lambda_big, "Large", " parameters", " require", " heap allocation.");

	//if (TaskType::is_stored_on_stack<decltype(lambda_small)>::value)
	globalPool.enqueue(std::move(smallTask));

	//if (!TaskType::is_stored_on_stack<decltype(lambda_big), std::string, std::string, std::string, std::string>::value)
	globalPool.enqueue(std::move(bigTask));
}

// �������Լ��
void taskPropertiesExample()
{
	auto lambda = [](int x) { return x * x; };
	TaskType task(lambda, 5);

	// �������
	std::cout << "Task properties:\n"
		<< "Storage size: " << sizeof(task) << " bytes\n"
		<< "Actual size:" << task_stack<decltype(lambda), int>::size << "\n"
		<< "Copyable: " << (task.is_copyable() ? "Yes" : "No") << "\n"
		<< "Moveable: " << (task.is_moveable() ? "Yes" : "No") << "\n"
		<< "Is stored on stack: " << (TaskType::is_stored_on_stack<decltype(lambda), int>::value ? "Yes" : "No") << "\n";
}

// �߳�ע��ʾ��
void threadRegisterExample()
{
	/*
	* ע���̵߳��̳߳صķ��������
	* ÿ��ע���̻߳ᱻ��̬����һ��ר�������飨RoundRobinGroup��
	* ���������һ������������У�TPBlockQueue����������ԣ�
	*   - ������ >= �߳�����ÿ���̷߳��� (�ܶ�����/�߳���) ������
	*   - ������ < �߳�����ÿ�����б� (�߳���/������) ���̹߳���
	*
	* ʾ����3������(1,2,3)��4�������߳�(A,B,C,D)
	* δע��ʱ���з������(������ѯ):
	*
	*   A->1, A->2, A->3, A->1
	*	B->1, B->2, B->3, B->1
	* 	C->1, C->2, C->3, C->1
	*	D->1, D->2, D->3, D->1
	*
	*	(Ƶ���л����»���ʧЧ)
	*
	* ע���̬���䣺
	*   A: ר��������[1]
	*   B: ר��������[2]
	*   C: ר��������[3]
	*   D: ר��������[1,2,3] (�������)
	*/

	globalPool.register_this_thread(); // ע�ᵱǰ�߳�

	/* ���������� */

	// ע��:����ʹ���̳߳�ʱ�輰ʱע��
	globalPool.unregister_this_thread();
	std::cout << "Thread register example" << std::endl;
}

int main()
{
	// ��ʼ���̳߳أ�10000������������С/���߳���1,��������
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