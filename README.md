# HSLL::ThreadPool 线程池

## 概述
HSLL ThreadPool 是一个高性能C++线程池实现，具有以下核心特性：

1. **多队列架构** - 每个工作线程拥有独立的任务队列，减少锁争用
2. **核心绑定** - 支持将工作线程绑定到指定CPU核心（Linux/Windows）
3. **灵活任务提交** - 提供阻塞/非阻塞、单任务/批量任务等多种接口
4. **优雅关闭** - 支持立即关闭和等待任务完成的优雅关闭模式
5. **类型安全** - 基于模板的任务类型支持，可搭配自定义任务类型

## 核心组件

### ThreadPool 类模板

#### 模板参数
```cpp
template <class T = TaskStack<>>
class ThreadPool
```
- `T`: 任务类型，需实现 `execute()` 方法，默认使用基于栈的预分配任务容器


#### 初始化方法
```cpp
bool init(unsigned queueLength, unsigned threadNum, unsigned batchSize = 1)
```
- **参数**：
  - `queueLength`: 每个工作队列的容量
  - `threadNum`: 工作线程数量
  - `batchSize`: 单次处理任务数（>=1）
- **返回值**：初始化成功返回true
- **功能**：分配资源并启动工作线程

#### 任务提交接口

1. **单任务提交**
```cpp
template <typename... Args>
bool emplace(Args&&... args) // 非阻塞提交（立即返回）
template <typename... Args>
bool wait_emplace(Args&&... args) // 阻塞提交
template <class Rep, class Period, typename... Args>
bool wait_emplace(const duration<Rep, Period>& timeout, Args&&... args) // 带超时
```

2. **预构建任务提交**
```cpp
template <typename U>
bool append(U&& task) // 非阻塞
template <class U>
bool wait_append(U&& task) // 阻塞
template <class U, class Rep, class Period>
bool wait_append(U&& task, const duration<Rep, Period>& timeout) // 带超时
```

3. **批量提交
```cpp
template <BULK_CMETHOD METHOD = COPY>
unsigned append_bulk(T* tasks, unsigned count) // 非阻塞批量
template <BULK_CMETHOD METHOD = COPY>
unsigned wait_appendBulk(T* tasks, unsigned count) // 阻塞批量
template <BULK_CMETHOD METHOD = COPY, class Rep, class Period>
unsigned wait_appendBulk(T* tasks, unsigned count, const duration<Rep, Period>& timeout)
```

#### 关闭方法
```cpp
void exit(bool shutdownPolicy = true) noexcept
```
- `shutdownPolicy`: 
  - true: 优雅关闭（执行完队列剩余任务）
  - false: 立即关闭

#### 工作机制
- **任务分发**：采用轮询+队列负载均衡策略
- **工作线程**：每个线程优先处理自己的队列，空闲时支持任务窃取
- **核心绑定**：自动将工作线程绑定到不同CPU核心（需硬件支持）


### 基本使用
```cpp
HSLL::ThreadPool<> pool;
pool.init(1000, 4); // 4线程，每队列容量1000

// 提交lambda任务
pool.emplace([]{
    std::cout << "Task executed!\n";
});

// 提交带参数的函数
void taskFunc(int a, double b) { /*...*/ }
pool.emplace(taskFunc, 42, 3.14);

//线程池析构时自动调用exit(false), 但仍然建议手动调用以控制退出行为
pool.exit(true); // 优雅关闭
```

### 性能优化建议
1. **批量提交**：优先使用`append_bulk`处理任务组
2. **任务大小**：根据任务复杂度选择合适存储模板参数
3. **线程数量**：通常设置为CPU核心数
4. **队列容量**：根据任务吞吐量需求调整

## 注意事项
1. 确保任务对象的线程安全性
2. 避免在任务中执行长时间阻塞操作

## 接口对比表

| 方法类型      | 非阻塞      | 阻塞等待    | 超时等待      |
|-------------|------------|------------|--------------|
| 单任务提交    | emplace    | wait_emplace| wait_emplace |
| 预构建任务   | append     | wait_append| wait_append  |
| 批量任务     | append_bulk| wait_appendBulk | wait_appendBulk |

## 平台支持
- Linux (pthread affinity)
- Windows (SetThreadAffinityMask)
- C++14 或更新标准
