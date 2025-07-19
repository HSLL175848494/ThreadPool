# TaskStack - 栈分配任务容器

## 概述
`TaskStack`是一个基于栈内存的任务容器模板类，可在不使用堆内存的情况下存储任务及其参数。通过完美转发参数并利用编译期计算，确保任务存储空间完全在栈上分配。

## 参数传递机制

### 存储阶段
- **完美转发存储**：保留原始值的左值/右值语义构造存储副本
- **参数类型处理**：使用`std::decay`移除引用和cv限定符
- **示例**：
  ```cpp
  std::string s = "hello";
  // 移动构造存储参数副本
  TaskStack<> task([](std::string& s){ /*...*/ }, std::move(s)); 
  ```

### 执行阶段
- **左值传递**：调用时任务参数始终以左值形式传递
- **重要限制**：
  ```cpp
  // 错误示例：不支持右值引用参数
  TaskStack<> task([](std::string&& s){}, "tmp");
  ```

## 编译期查询
```cpp
// 获取任务所需存储大小
constexpr size_t size_c11 = task_stack<F, Args...>::size;
constexpr size_t size_c14 = task_stack_V<F, Args...>;

// 检查任务是否可被存储
using MyTaskStack = TaskStack<1024, 16>;
constexpr bool valid_c11 = MyTaskStack::task_invalid<F, Args...>::value;
constexpr bool valid_c14 = MyTaskStack::task_invalid_v<F, Args...>;
```

## 注意事项
1. **右值参数限制**：任务函数签名禁止包含右值引用参数
2. **对象生命周期**：
   - 引用捕获的参数需保证在任务执行时有效
   - 推荐对长期对象使用引用捕获
3. **存储溢出**：任务超出可用栈空间会导致编译错误
4. **移动语义**：
   - 移动构造后源对象失效
   - 任务对象本身支持移动语义
5. **对齐要求**：
   - 存储空间必须满足`ALIGN`对齐要求
   - 默认对齐为`alignof(std::max_align_t)`
6. **异常安全**：
   - 可调用对象和参数的拷贝/移动构造不允许抛出异常

## 重要警告：任务参数的可拷贝/移动性

### 核心问题
由于设计上必须启用拷贝/移动构造函数：
1. 任务的可拷贝/移动性**只能在运行期检测**
2. 存储不可拷贝/移动对象时：
   - 编译期不报错
   - 运行期调用构造函数时失败

### 危险示例
```cpp
std::packaged_task<void()> task_func([]{});
TaskStack<> task1(std::move(task_func));  // 正确：移动构造

// 编译通过但运行期崩溃！
TaskStack<> task2(task1);  // 尝试拷贝不可拷贝对象
```
---

## HeapCallable - 堆分配任务包装器

### 设计目的
补充栈上任务的不足，为大型任务提供自动堆内存管理

### 核心特性
1. **自动内存管理**：参数存储在堆上，生命周期由引用计数管理
2. **动态空间分配**：适合存储大型任务对象
3. **异常安全**：构造时可能抛出`std::bad_alloc`

### 构造方式
```cpp
// 工厂函数构造
auto callable = make_callable([](int x){}, 42);

// 直接构造（需显式指定类型）
auto p = [](int x){}；
HeapCallable<decltype(p),int> callable(p, 42);

// 执行任务
callable();
```

### 与TaskStack集成
```cpp
// 显式使用堆分配
//TaskStack<> task = TaskStack<>::make_heap(heavy_task, large_data);
TaskStack<> task(make_callable(heavy_task, large_data));

// 自动选择存储策略（栈或堆）
auto task = TaskStack<>::make_auto(
    [](const BigData& data){}, 
    std::move(big_data)  // 超过栈容量时自动使用堆
);

task.execute();  // 统一执行接口
```

# 扩展：HeapCallable_Cancelable 与 HeapCallable_Async

## HeapCallable_Async - 异步任务包装器

### 设计目的
提供异步执行机制，通过`std::promise/std::future`实现任务结果获取

### 核心特性
1. **异步结果传递**：通过promise/future机制返回任务结果
2. **异常捕获**：自动捕获任务执行中的异常并传递给future
3. **类型安全**：严格匹配返回类型与promise类型a
4. **资源管理**：使用unique_ptr确保单次执行语义

### 构造方式
```cpp
// 工厂函数构造（推荐）
auto async_task = make_callable_async<ReturnType>(
    [](int x) -> ReturnType { /* ... */ }, 
    42
);

// 直接构造
HeapCallable_Async<ReturnType, decltype(func), int> 
    task_async(std::move(func), 42);
```

### 执行与结果获取
```cpp
// 获取future（必须在执行前获取）
std::future<ReturnType> fut = async_task.get_future();

// 执行任务（通常在独立线程中）
async_task(); 

// 获取结果（阻塞直到完成）
ReturnType result = fut.get();
```

### 异常处理
```cpp
try {
    fut.get();
} 
catch (const std::exception& e) {
    // 处理任务抛出的异常
}
```

### 生命周期规则
1. **移动语义**：支持移动构造，禁止拷贝
2. **单次执行**：每个对象只能执行一次
3. **future单次获取**：get_future()只能调用一次

---

## HeapCallable_Cancelable - 可取消的异步任务

### 设计目的
在异步任务基础上增加任务取消功能

### 核心特性
1. **原子取消标志**：使用std::atomic<bool>实现线程安全取消
2. **取消控制器**：通过Controller对象提供取消接口
3. **取消异常**：取消时设置std::runtime_error异常
4. **共享状态**：使用shared_ptr实现控制器与任务的状态共享

### 构造方式
```cpp
// 工厂函数构造
auto cancelable_task = make_callable_cancelable<ReturnType>(
    [](int x) { /* ... */ }, 
    42
);

// 直接构造
HeapCallable_Cancelable<ReturnType, decltype(func), int>
    task_cancelable(func, 42);
```

### 控制器使用
```cpp
// 获取控制器
auto controller = cancelable_task.get_controller();

// 尝试取消任务
if (controller.cancel()) {
    std::cout << "任务已取消\n";
} else {
    std::cout << "取消失败（任务已开始）\n";
}
```

### 取消机制详解

```cpp
   bool cancel()//取消任务
   {
      assert(storage);//任务容器
      bool expected = false;
      auto& flag = std::get<1>(*storage);

      if (flag.compare_exchange_strong(expected, true))
      {
         auto& promise = std::get<0>(*storage);
         promise.set_exception(std::make_exception_ptr(std::runtime_error("Task canceled")));
         return true;
      }
      return false;
   }

   void operator()()//执行任务
   {
      assert(storage);//任务容器
      bool expected = false;
      auto& flag = std::get<1>(*storage);

      if (flag.compare_exchange_strong(expected, true))
         invoke();
   }
```
1. **取消结果**：
   - 成功：返回true，promise设置"Task canceled"异常
   - 失败：返回false，任务已开始执行或已完成

2. **执行检查**:若已经设置取消任务标志则忽略任务直接返回

## 三种HeapCallable对比

| 特性                | HeapCallable | HeapCallable_Async | HeapCallable_Cancelable |
|---------------------|--------------|--------------------|-------------------------|
| 返回值获取          | ❌           | ✅ (future)         | ✅ (future)             |
| 异常传递            | ❌           | ✅                  | ✅                      |
| 任务取消            | ❌           | ❌                  | ✅                      |
| 执行线程安全        | ✅           | ✅                  | ✅ (原子操作)           |
| 内存管理            | unique_ptr   | unique_ptr          | shared_ptr              |
| 典型应用场景        | 简单任务     | 需要结果的任务     | 可中断的长任务         |