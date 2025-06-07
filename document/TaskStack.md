# TaskStack - 栈分配任务容器

## 概述
TaskStack 是一个基于栈内存的任务容器模板类，通过小缓冲区优化(SBO)实现无堆内存分配的任务存储

## 核心特性

- **零堆分配**：使用固定大小栈存储，避免动态内存分配
- **类型擦除**：可存储函数指针、lambda、函数对象等任意可调用类型
- **参数绑定**：支持绑定任意数量/类型的参数
- **语义支持**：支持拷贝/移动构造和赋值操作
- **智能对齐**：智能调整存储大小保证对齐要求
- **编译期检查**：静态断言确保存储空间充足

---

## 使用示例

### 基本用法
```cpp
void print_sum(int a, int b) {
    std::cout << a + b << std::endl;
}

TaskStack<> task1(print_sum, 3, 5); 
task1.execute(); // 输出8
```

### Lambda表达式
```cpp
std::string prefix = "Result:";
TaskStack<128> task2(
    [](const std::string& s, int x) {
        std::cout << s << x << std::endl;
    },
    prefix, 42
);
task2.execute(); // 输出Result:42
```

### 避免参数拷贝
```cpp
std::vector<int> big_data(1000);

// 1.通过lambda捕获引用(不存储参数副本)
TaskStack<> task3([&]{
    process_data(big_data); 
});


// 2.通过指针传递
TaskStack<> task3([](std::vector<int>* big_data){
    process_data(big_data); 
},&big_data);

```

---

## 参数传递机制

### 存储阶段
- 参数按**完美转发**存储：保留原始值/右值语义对储存副本进行构造
- 参数类型会经过`std::decay`处理
- **示例**：
  ```cpp
  std::string s = "hello";
  TaskStack<> task([](std::string& s){...}, std::move(s)); // 移动构造存储
  ```

### 执行阶段
- **始终传递左值**：调用时参数以左值形式传递
- **限制**：
  - 不支持接收右值引用的函数参数
  ```cpp
  // 错误示例：
  TaskStack<> task([](std::string&& s){...}, "tmp"); 
  ```

### 高效使用建议
1. 对需要移动语义的参数：
   ```cpp
   void process(std::string& s) {
       auto s2 = std::move(s); // 安全操作：s是存储在任务内的副本
   }
   ```
2. 对长期有效的大对象：通过lambda捕获引用

### 详细流程

```mermaid
sequenceDiagram
    participant Caller as 调用线程
    participant Task as TaskStack
    participant Exec as 工作线程
    
    Caller->>Task: 构造任务(参数拷贝/移动)
    Note over Task: 参数独立存储
    Caller-->>Caller: 原始参数可立即销毁
    Task->>Exec: 任务入队
    Exec->>Exec: 任意时间后执行
    Exec->>Task: execute()
    Task->>Exec: 传递存储的参数副本
```
---

## 存储管理 (更新)

### 容量控制
- 默认存储大小`64 - sizeof(std::max_align_t)`字节，可通过模板参数调整：
  ```cpp
  TaskStack<256> large_task(...);
  ```
- **智能对齐调整**：存储大小自动向下对齐到指定对齐要求
  ```cpp
  // 实际存储大小 = TSIZE - (TSIZE % ALIGN) = sizeof(TaskStack<TSIZE, ALIGN>)
  TaskStack<63,4> task; // 实际大小为60字节(当ALIGN=4时)
  ```

### 编译期检查
```cpp
// 存储空间检查
static_assert(sizeof(TaskImpl) <= sizeof(storage));
static_assert(alignof(TaskImpl) <= ALIGN);
```

### 查询任务所需大小

```cpp
// 编译期获取任务所需大小
constexpr unsigned int needed_size = stack_tsize_v<F, Args...>;
```

---

## 注意事项

1. **右值参数限制**：任务函数签名禁止包含右值引用参数
2. **对象生命周期**：
   - 引用捕获/指针传递的参数需保证在任务执行前有效
   - 推荐对长期对象使用引用捕获/指针转递
3. **存储溢出**：
   - 超出预设大小会导致编译错误
   - 可通过`stack_tsize_v`预计算所需大小
   - 可通过`sizeof(TaskStack<TSIZE,ALIGN>)`获取存储空间大小
4. **移动语义**：
   - 移动构造后源对象失效
   - 任务对象本身支持移动语义
5. **对齐要求**：
   - 实际存储大小可能小于指定大小（对齐调整）
   - 确保任务对齐要求不超过`ALIGN`设置
