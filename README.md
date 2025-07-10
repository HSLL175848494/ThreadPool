# HSLL::ThreadPool

## 概述

这是一个轻量级的 C++11 线程池实现，**无第三方依赖，仅需头文件即可使用**。

它的核心优势在于**高效和灵活**：
*   **避免动态内存分配：** 通过基于栈的预分配任务容器，将任务及其参数直接存储在栈上。
*   **多种提交方式：** 支持阻塞或非阻塞地提交单个任务或批量任务，满足不同场景需求。
*   **灵活的任务管理：** 允许从任务队列的头部或尾部插入任务（双端操作）。
*   **智能负载均衡：** 结合 Round-Robin 调度、二级队列选取和任务窃取机制，有效分散工作负载。
*   **资源优化：** 能够根据当前负载动态调整活跃线程数量，减少不必要的内存占用。
*   **优雅关闭：** 提供立即关闭或等待所有任务执行完毕后再关闭两种模式。

## 引入
```cpp
//确保basic文件夹在其的同级目录
#include "ThreadPool.hpp"
```

## ThreadPool 类模板

### 模板参数
```cpp
template <class TYPE = TaskStack<>>
class ThreadPool
```
- `TYPE`: 基于栈的预分配任务容器（详见TaskStack.md文档）

### 初始化方法
```cpp
bool init(unsigned int queueLength, unsigned int minThreadNum,
            unsigned int maxThreadNum, unsigned int batchSize = 1,
            std::chrono::milliseconds adjustInterval = std::chrono::milliseconds(3000))
```
- **参数**：
  - `queueLength`: 每个工作队列的容量
  - `minThreadNum`: 工作线程最小数量
  - `maxThreadNum`:工作线程最大数量
  - `batchSize`: 单次处理任务数
  - `adjustInterval`：活跃线程数动态调整间隔
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

#include "ThreadPool.hpp"

using namespace HSLL;
using Type = TaskStack<64,8>;//最大容量为64字节,最大对齐值为8的任务容器

void Func(int a, double b) { /*...*/ }

int main()
{
    //创建线程池实例,任务容器使用Type类型
    ThreadPool<Type> pool;

    // 初始化线程池: 队列容量1000，最小活跃线程数1，最大线程数4，批处理大小为1（default）
    pool.init(1000,1,4); 

    //添加任务_基本示例
    Type task(Func, 42, 3.14);
    pool.enqueue(task);

    //添加任务_就地构造
    pool.emplace(Func, 42, 3.14);//相比于enqueue减少了一次临时对象的构造

    //添加任务_std::function
    std::function<void(int,int)> func(Func);
    pool.emplace(f,42,3.14);

    //添加任务_lambda
    pool.enqueue([](int a,int b){});

    //线程池析构时自动调用exit(false), 但仍然建议手动调用以控制退出行为
    pool.exit(true); // 优雅关闭。调用后可通过init重新初始化队列

    return 0;
}
```
**其它用法可参照example中的示例文件**

## 任务生命周期
```mermaid
graph TD
    A[任务提交] --> B{提交方式}
    B -->|emplace| C[在队列中直接构造任务]
    B -->|enqueue/enqueue_bulk| D[将已构造的任务对象后拷贝/移动到队列中]
    
    C --> E[以移动的形式取出任务]
    D --> E
    
    E --> F[执行execute方法]
    F --> G[显式调用析构函数]
    G --> H[清理执行内存]
```

## 参数传递过程
```mermaid
graph LR
    A[构造任务] --> B[参数传递方式]
    B --> C{左值参数}
    B --> D{右值参数}
    C --> E[拷贝到任务容器]
    D --> F[移动到任务容器]
    
    H[执行任务]  --> I[容器中存储的参数以左值引用方式传递]
    E --> H
    F --> H
    
    I --> J[函数调用]
    J --> K{传参类型}
    K --> L[值类型 T]
    K --> M[左值引用 T&]
    K --> N[const 引用 const T&]
    K --> O[不支持：右值引用 T&& ]:::unsupported
    
    classDef unsupported fill:#f10,stroke:#333
```

## 注意事项
1. **类型匹配**：提交任务类型必须严格匹配队列任务类型
2. **对齐要求**：任务最大对齐值必须小于等于队列任务类型的对齐值
3. **异常安全**：
   - 任何入队列行为不允许抛出异常
   - 调用emplace系列接口需要保证任务（参数/拷贝/移动构造）不抛出异常，其它类型接口需要保证任务(拷贝/移动构造)不抛出异常
   - execute()方法不允许抛出异常，需要在任务内部捕获并处理所有可能的异常
     
**不同于申请在堆上的任务，栈上任务的拷贝可能发生异常。由于异步执行的栈上任务无法
将异常传播到调用者，因此严格的异常保证是出于将任务存储在栈上的必要妥协**

## 平台支持
- Linux (aligned_alloc)
- Windows (aligned_malloc)
- C++11 或更新标准

## 其它
- **组件文档**: `document`文件夹
- **使用示例**: `example`文件夹
- **性能测试**: `perf test`文件夹