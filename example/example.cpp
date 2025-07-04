#include"ThreadPool.hpp"
#include<future>

using namespace HSLL;
using Type = TaskStack<64, 8>;
ThreadPool<Type> pool;

void TestFunc(std::string& s)
{
	printf("%s\n", s.c_str());
}

void TestBigFunc(double a, double b, double c, double d, double e, double f, double g)
{
	printf("%s %f\n", "big task:", a);
}

void example_enqueue()
{
	Type task(TestFunc, std::string("example_enqueue"));
	pool.enqueue(std::move(task));//需要任务每个参数（包括可调用对象）的拷贝或移动可用
	//pool.enqueue(task);//需要任务每个参数（包括可调用对象）的拷贝可用
}

void example_emplace()
{
	pool.emplace(TestFunc, std::string("example_emplace"));
}

void example_bulk()
{
	alignas(alignof(Type)) unsigned char buf[4 * sizeof(Type)];

	Type* p = (Type*)buf;

	for (int i = 0; i < 4; i++)
		new (p + i) Type(TestFunc, std::string("example_bulk") + std::to_string(i));

	//参数以拷贝的形式储存到队列
	unsigned int num = pool.enqueue_bulk<COPY>((Type*)buf, 4);

	//参数以移动的形式储存到队列
	//unsigned int num = pool.enqueue_bulk<MOVE>((Type*)buf, 4);

	for (int i = 0; i < 4; i++)
		(p + i)->~Type();
}

void example_async()
{
	std::promise<int> promise;
	auto future = promise.get_future();

	pool.emplace([&promise] {

		int sum = 0;

		for (int i = 1; i <= 100; i++)
			sum += i;

		promise.set_value(sum);

		});

	int total = future.get();

	printf("%s %d\n", "async task:", total);
}

void example_pos_insert()
{
	pool.emplace<INSERT_POS::TAIL>(&TestFunc, std::string("example_insert_tail"));
	pool.emplace<INSERT_POS::HEAD>(&TestFunc, std::string("example_insert_head"));
}

void example_bigtask_callable()
{
	double param = 1;

	//超出可容纳范围,静态断言失败
	//Type task(TestBigFunc,param, param, param, param, param, param, param);

	//创建堆上任务,两步创建
	auto callable = make_callable(TestBigFunc, param, param, param, param, param, param, param);
	Type task(callable);

	//一次性创建Type
	//Type task= Type::make_heap(TestBigFunc, param, param, param, param, param, param, param);

	pool.enqueue(task);
}

void example_bigtask_auto()
{
	//当任务无法储存时自动选择创建堆上任务
	double param = 2;
	Type task = Type::make_auto(TestBigFunc, param, param, param, param, param, param, param);
	pool.enqueue(task);
}

void example_static()
{
	Type task(TestFunc, std::string("example_static"));

	//判断任务是否可拷贝(不含有is_moveable()函数,因为其始终可移动)
	printf("TestFunc is_copyable: %d\n", task.is_copyable());

	//判断是否为有效任务（可储存的任务）
	printf("TestFunc is_invalid: %d\n", Type::task_invalid<decltype(TestFunc), std::string&>::value);

	//获取任务实际需要存储空间大小
	printf("TestFunc size: %d\n", task_stack<decltype(TestFunc), std::string&>::size);
}

int main()
{
	pool.init(10000, 1, 1, 1);
	example_enqueue();
	example_emplace();
	example_bulk();
	example_async();
	example_pos_insert();
	example_bigtask_callable();
	example_bigtask_auto();
	pool.exit(true);
	example_static();
	return 0;
}