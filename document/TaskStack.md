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
  - `func`：可调用对象（函数对象/函数指针/lambda表达式/仿函数/成员函数）
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

**注意**：任务容器中的参数类型由构造时`Args&&`推导,其类型可能与函数签名不一致

例如：
```cpp

void example(long long a, float b){}

//在构造函数中5/10.0 可能会被推导为int与double类型。传入参数时可能导致损失精度或数据截断，需要尤为注意
TaskStack task(example,5,10.0);
```
可以显式指定类型或强制类型转换避免以上问题 
```cpp
TaskStack task(example,5ll,10.0f);
TaskStack task(example,(long long)5,(float)10.0);
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


## 使用注意事项

### 引用传递规范
```cpp
// 正确引用传递（使用std::ref）
std::vector data;
TaskStack task([](auto& vec) {...}, std::ref(data));

// 错误做法（值拷贝）
TaskStack bad_task([](auto vec) {...}, data); // 数据拷贝