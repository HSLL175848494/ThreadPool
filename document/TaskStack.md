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

### 高效使用建议
```cpp
// 1. 需要移动语义时显式转移所有权
task.execute([](std::string& s) {
    auto local_str = std::move(s); // 安全转移存储副本
});

// 2. 长期有效的大对象通过引用捕获
LargeObject obj;
auto lambda = [&obj]{ /* 使用obj */ };
TaskStack<> task(lambda); // 仅存储lambda对象
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
// 工厂函数构造（推荐）
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
TaskStack<> task(make_callable(heavy_task, large_data));

// 自动选择存储策略（栈或堆）
auto task = TaskStack<>::make_auto(
    [](const BigData& data){}, 
    std::move(big_data)  // 超过栈容量时自动使用堆
);

task.execute();  // 统一执行接口
```

### 使用限制
```cpp
// 禁止嵌套使用
auto c1 = make_callable([]{});
auto c2 = make_callable(c1);  // 编译错误！
```

## 最佳实践总结
1. **小任务**：优先使用`TaskStack`直接存储
2. **大任务**：使用`make_auto()`自动选择存储策略
3. **参数所有权**：
   - 短期参数：值存储 + 内部`std::move`
   - 长期参数：引用捕获
4. **错误处理**：
   - 编译期检查`task_invalid_v`
   - 运行期验证可移动性