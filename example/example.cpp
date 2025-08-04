#include "ThreadPool.hpp"
#include <iostream>

using namespace HSLL;
using TaskType = TaskStack<64, 8>;
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

// ����洢�ڶ���ʾ��
void heapExample()
{
	//������������,����Callable
	auto callable = make_callable([]() {
		std::cout << "Heap task1 completed." << std::endl;
		});

	globalPool.submit(std::move(callable));
}

// �첽����ʾ��
void asyncExample()
{
	auto task = make_callable_async(calculateSum, 333, 333);
	auto future = task.get_future();

	globalPool.submit(std::move(task));

	auto result = future.get();
	std::cout << "Async result1: " << result << std::endl;
}

// ��ȡ������ʾ��
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

// ���������ύʾ��
void batchExample()
{
	BatchSubmitter<TaskType, 10> batch(globalPool);

	//BatchSubmitter������������ʱ�Զ��ύ,����10��
	for (int i = 0; i < 10; ++i) {
		batch.add([i] {
			std::cout << "Batch task " << i << std::endl;
			});
	}

	std::cout << "Batch tasks submitted" << std::endl;
}

// �������λ�ÿ���
void positionControlExample()
{
	// β�����루�����ȼ�,Ĭ�ϣ�
	globalPool.submit<INSERT_POS::TAIL>([] {
		std::cout << "Low priority task (tail)" << std::endl;
		});

	// ͷ�����루�����ȼ���
	globalPool.submit<INSERT_POS::HEAD>([] {
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
	TaskType bigTask(lambda_big, std::string("Large"), std::string(" parameters"), std::string(" require"), std::string(" heap allocation."));

	//if (TaskType::is_stored_on_stack<decltype(lambda_small)>::value)
	globalPool.submit(std::move(smallTask));

	//if (!TaskType::is_stored_on_stack<decltype(lambda_big), std::string, std::string, std::string, std::string>::value)
	globalPool.submit(std::move(bigTask));
}


// �������Լ��
void taskPropertiesExample()
{
	auto lambda = [](int x) { return x * x; };
	TaskType task(lambda, 5);

	// �������
	std::cout << "Task properties:\n"
		<< "Storage size: " << sizeof(task) << " bytes\n"
		<< "Actual size:" << TaskImplTraits<decltype(lambda), int>::size << "\n"
		<< "Is stored on stack: " << (TaskType::is_stored_on_stack<decltype(lambda), int>::value ? "Yes" : "No") << "\n";
}

// �߳�ע��ʾ��
void threadRegisterExample()
{
	/*
	* ע���̵߳��̳߳صķ��������
	* ÿ��ע���̻߳ᱻ��̬����һ��ר�������飨RoundRobinGroup��
	* ���������һ������������У�TPBlockQueue��
	*
	* ʾ����3������(1,2,3)��4�������߳�(A,B,C,D)
	*
	* ---------------------------------------------------------------------
	* δע��ʱ���з����Ǹ����ڲ�indexԭ�ӱ�������ȫ����ѯ
	*
	* ���з���	1	 2      3     1     2     3    ...
	* �ύ��¼ A�ύ B�ύ C�ύ A�ύ B�ύ C�ύ ...
	*
	* �÷����ᵼ�»���Ƶ����ʧЧ,����indexԭ�ӱ���Ҳ������ƿ��
	*----------------------------------------------------------------------
	* ע�����з���:
	*
	*   A: ר��������[1]
	*   B: ר��������[2]
	*   C: ר��������[3]
	*   D: ר��������[1,2,3] (�������)
	*
	* �����ڰ���������еĶ����飬�����ύ���������ﵽ��ֵʱ���л�����һ�����У���ѯ��
	* ---------------------------------------------------------------------
	*/

	globalPool.register_this_thread(); // ע�ᵱǰ�߳�

	/* ���������� */

	// ע��:��ǰ�̲߳���ʹ���̳߳�ʱ�輰ʱע�������µ����̳߳صĶ��������
	globalPool.unregister_this_thread();
	std::cout << "Thread register example" << std::endl;
}

//�Զ����ڴ������ͷ�����HeapCallable������tp_smart_ptr��
void customAllocatorExample()
{
	//����:����һ��ʵ����AllocatorBase�麯��������
	//Ϊ��ʱ��Ĭ���ڴ������ͷ���(malloc/free)
	set_tp_smart_ptr_allocator();
}

int main()
{
	// ��ʼ���̳߳أ�10000������������С/���߳���1,��������
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