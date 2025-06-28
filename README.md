# HSLL::ThreadPool

## 概述
HSLL::ThreadPool 是一个无第三方依赖、head-only的轻量级C++11线程池实现，具有以下特性：

1. **定长任务容器** - 基于栈的预分配任务容器，将所有参数储存在栈上，避免动态申请空间
2. **多提交接口** - 提供阻塞/非阻塞、单任务/批量任务等多种接口
3. **双端插入支持** - 支持从队列头/尾插入
4. **负载均衡** - 采用round-robin+二级队列选取机制+任务窃取实现负载均衡
5. **动态线程** - 活跃线程数量动态调整以调整内存占用
6. **优雅关闭** - 支持立即关闭和等待任务完成的优雅关闭模式

## 引入
```cpp
//请保证basic文件夹及其内容在ThreadPool.hpp的同级目录
#include "ThreadPool.hpp"
```

## ThreadPool 类模板

### 模板参数
```cpp
template <class TYPE = TaskStack<>>
class ThreadPool
```
- `TYPE`: 基于栈的预分配任务容器

### 初始化方法
```cpp
bool init(unsigned int queueLength, unsigned int minThreadNum,
            unsigned int maxThreadNum, unsigned int batchSize = 1)
```
- **参数**：
  - `queueLength`: 每个工作队列的容量
  - `minThreadNum`: 工作线程最小数量
  - `maxThreadNum`:工作线程最大数量
  - `batchSize`: 单次处理任务数
- **返回值**：初始化成功返回true
- **功能**：分配资源并启动工作线程(初始值为最大数量)

### 关闭方法
```cpp
void exit(bool shutdownPolicy = true)
```
- `shutdownPolicy`: 
  - true: 优雅关闭（执行完队列剩余任务）
  - false: 立即关闭

## 任务提交接口

| 方法类型      | 非阻塞      | 阻塞等待    | 超时等待      |
|-------------|------------|------------|--------------|
| 单任务提交    | emplace    | wait_emplace| wait_emplace |
| 预构建任务   | enqueue     | wait_enqueue| wait_enqueue  |
| 批量任务     | enqueue_bulk| wait_enqueue_bulk | wait_enqueue_bulk |


## 基本使用
```cpp
HSLL::ThreadPool<> pool;

pool.init(1000,1,4); // 队列容量1000,最小活跃线程数1，最大线程数4

// 提交lambda任务
pool.emplace([]{
    std::cout << "Task executed!\n";
});

// 提交带参数的函数
void taskFunc(int a, double b) { /*...*/ }

//添加任务示例
pool.enqueue(taskFunc, 42, 3.14);//taskFunc,42,3.14 隐式转化为TaskStack对象

//异步示例
std::promise<int> resultPromise;
auto resultFuture = resultPromise.get_future();
pool.emplace([&resultPromise] {
      int sum = 0;
      for (int i = 1; i <= 100; i++) {
        sum += i;
   }
  resultPromise.set_value(sum); 
});
int total = resultFuture.get();

//线程池析构时自动调用exit(false), 但仍然建议手动调用以控制退出行为
pool.exit(true); // 优雅关闭。调用后可通过init重新初始化队列
```

### 任务生命周期
```mermaid
graph TD
    A[任务提交] --> B{提交方式}
    B -->|emplace/wait_emplace| C[在队列存储中<br/>直接构造任务]
    B -->|enqueue/wait_enqueue| D[用户构造任务对象<br/>拷贝/移动到队列存储]
    
    C --> E[工作线程从队列取出任务]
    D --> E
    
    E --> F[在预分配执行内存上<br/>就地构造（移动构造）]
    F --> G[执行execute方法]
    G --> H[显式调用析构函数]
    H --> I[清理执行内存]
```

## 注意事项
1. **类型匹配**：提交任务类型必须严格匹配队列任务类型
2. **对齐要求**：任务最大对齐值必须小于等于队列任务类型的对齐值
3. **异常安全**：
   - 任何入队列行为不允许抛出异常
   - 调用emplace类接口需要保证任务（参数/拷贝/移动构造）不抛出异常，其它类型接口需要保证任务(拷贝/移动构造)不抛出异常
   - execute()方法不允许抛出异常，需要在任务内部捕获并处理所有可能的异常

4. **参数传递**：
   - 大型对象应使用指针或移动语义传递
   - 避免在任务中捕获可能失效的引用
5. **结果获取**：
   - 使用std::promise/std::future获取异步结果
   - promise必须在任务执行期间保持有效

## 平台支持
- Linux (aligned_alloc)
- Windows (aligned_malloc)
- C++11 或更新标准

## 其它
- **组件文档**: `document`文件夹
- **性能测试**: `test`文件夹
