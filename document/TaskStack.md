# TaskStack - 栈分配任务容器

## 概述
`TaskStack`是一个基于栈内存的任务容器模板类，可在不使用堆内存的情况下存储任务及其参数。

## 类模板声明
```cpp
template <unsigned int TSIZE = 64, unsigned int ALIGN = 8>
class TaskStack;
```

## 模板参数
| 参数   | 说明                                                                 | 约束条件 |
|--------|----------------------------------------------------------------------|----------|
| `TSIZE` | 栈存储区大小（字节）                                                | `≥24` 且必须是 `ALIGN` 的倍数 |
| `ALIGN` | 任务中所有组件（可调用对象+每个参数）的最大允许对齐值               | `≥alignof(void*)` |

## 公共成员类型

### `is_stored_on_stack`
```cpp
template <class F, class... Args>
struct is_stored_on_stack {
    static constexpr bool value = /* 编译期计算结果 */;
};
```
- **功能**：检查指定任务是否直接存储在栈上
- **返回值**：
  - `true`：任务大小 ≤ `TSIZE` 且所有组件对齐 ≤ `ALIGN`
  - `false`：任务需要堆存储

## 构造函数

### 任务构造
```cpp
template <class F, class... Args>
TaskStack(F&& func, Args&&... args);
```
- **功能**：构造并存储任务
- **参数**：
  - `func`：可调用对象（函数指针/lambda/函数对象）
  - `args...`：任务参数（完美转发）
- **存储决策**：
  - 栈存储：当 `sizeof(任务) ≤ TSIZE` 且 `alignof(任务) ≤ ALIGN`
  - 堆存储：否则（使用 `HeapCallable` 包装）
- **参数处理规则**：
  1. **类型退化**：所有参数均按值存储（移除引用和cv限定符）
  2. **左值传递**：执行时始终以左值传递参数

```cpp
// 正确引用传递示例
int value = 42;
TaskStack task([](int& v) { v *= 2; }, std::ref(value));

// 错误引用传递示例
TaskStack bad_task([](int& v) {v *= 2;},value);  // 内部存储的是value的副本

// 禁止函数右值签名
TaskStack bad_task2([](int&& v) {}, 5);  // 编译错误
```

## 公共成员函数

### `execute`
```cpp
void execute();
```
- **功能**：同步执行存储的任务
- **关键特性**：
  - 参数始终以**左值形式**传递给可调用对象
  - 禁止抛出任何异常（noexcept保证）
- **前置条件**：对象必须包含有效任务

### `is_copyable`
```cpp
bool is_copyable();
```
- **功能**：检查任务是否可复制
- **返回值**：
  - `true`：底层可调用对象支持复制
  - `false`：包含不可复制对象（如std::packaged_task）
- **注意**：复制构造前请尽量确认该对象可拷贝

### `is_moveable`
```cpp
bool is_moveable();
```
- **功能**：检查任务是否可移动
- **返回值**：始终返回 `true`
- **注意**：移动操作总是安全有效

## 使用注意事项

### 引用传递规范
```cpp
// 正确引用传递（使用std::ref）
std::vector data;
TaskStack task([](auto& vec) { 
    vec.push_back(42); 
}, std::ref(data));

// 错误做法（值拷贝）
TaskStack bad_task([](auto vec) {...}, data); // 数据拷贝
```

### 不可复制对象处理

**由于TaskStack作为容器必须启用拷贝/移动构造函数，所以任务真正的可拷贝/移动性只能在运行期检测**

```cpp
std::packaged_task<void()> pt;//不可拷贝对象
TaskStack t1(std::move(pt));  // OK
if (!t1.is_copyable()) {
    // 禁止复制操作
    // TaskStack t2(t1); // printf错误并结束进程std::abort()！
}
```