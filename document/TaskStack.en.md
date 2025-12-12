# TaskStack - Stack-Allocated Task Container

## Overview
`TaskStack` is a task container template class based on stack memory that can store tasks and their parameters without using heap memory.

## Class Template Declaration
```cpp
template <unsigned int TSIZE = 64, unsigned int ALIGN = 8>
class TaskStack;
```

## Template Parameters
| Parameter | Description                                                                 | Constraints |
|-----------|-----------------------------------------------------------------------------|-------------|
| `TSIZE`   | Size of the stack storage area (in bytes)                                   | `≥24` and must be a multiple of `ALIGN` |
| `ALIGN`   | Maximum allowed alignment value for all components (callable object + each parameter) within a task | `≥alignof(void*)` |

## Public Member Types

### `is_stored_on_stack`
```cpp
template <class F, class... Args>
struct is_stored_on_stack {
    static constexpr bool value = /* Compile-time calculation result */;
};
```
- **Function**: Checks whether a specified task is stored directly on the stack
- **Return Value**:
  - `true`: Task size ≤ `TSIZE` and alignment of all components ≤ `ALIGN`
  - `false`: Task requires heap storage

## Constructors

### Task Construction
```cpp
template <class F, class... Args>
TaskStack(F&& func, Args&&... args);
```
- **Function**: Constructs and stores a task
- **Parameters**:
  - `func`: Callable object (function object/function pointer/lambda expression/functor/member function)
  - `args...`: Task parameters (perfectly forwarded)
- **Storage Decision**:
  - Stack storage: When `sizeof(task) ≤ TSIZE` and `alignof(task) ≤ ALIGN`
  - Heap storage: Otherwise (wrapped using `HeapCallable`)
- **Parameter Processing Rules**:
  1. **Type Decay**: All parameters are stored by value (removing references and cv-qualifiers)
  2. **Lvalue Passing**: Parameters are always passed as lvalues during execution

```cpp
// Correct reference passing example
int value = 42;
TaskStack task([](int& v) { v *= 2; }, std::ref(value));

// Incorrect reference passing example
TaskStack bad_task([](int& v) { v *= 2; }, value);  // Internally stores a copy of value

// Prohibited function rvalue signatures
TaskStack bad_task2([](int&& v) {}, 5);  // Compilation error
```

**Note**: The parameter types within the task container are deduced from `Args&&` during construction and may not match the function signature.

For example:
```cpp
void example(long long a, float b) {}

// In the constructor, 5/10.0 might be deduced as int and double types. Passing arguments may lead to loss of precision or data truncation, requiring special attention.
TaskStack task(example, 5, 10.0);
```
Explicitly specifying types or using type casting can avoid these issues:
```cpp
TaskStack task(example, 5ll, 10.0f);
TaskStack task(example, (long long)5, (float)10.0);
```

## Public Member Functions

### `execute`
```cpp
void execute();
```
- **Function**: Synchronously executes the stored task
- **Key Characteristics**:
  - Parameters are always passed to the callable object as **lvalues**
  - Guaranteed not to throw any exceptions (noexcept guarantee)
- **Precondition**: The object must contain a valid task

## Usage Notes

### Reference Passing Conventions
```cpp
// Correct reference passing (using std::ref)
std::vector data;
TaskStack task([](auto& vec) {...}, std::ref(data));

// Incorrect approach (value copy)
TaskStack bad_task([](auto vec) {...}, data); // Data is copied
```

## Extensions

### HeapCallable
When `TaskStack` cannot store a complete task due to insufficient space, it first wraps the task as a `HeapCallable` object for storage (static assert `TSIZE > 24` ensures storability). Users can also directly construct `HeapCallable` to independently store tasks:
```cpp
auto callable = make_callable(func);
TaskStack task(callable);
```

### HeapCallable_Async
To address the lack of native return value support in `TaskStack`, `HeapCallable_Async` returns a `std::future` object via `get_future()` after construction. During task execution, it automatically handles value/exception passing logic:
```cpp
auto callable = make_callable_async(Sum, 333, 333);
auto future = callable.get_future();  // Get future object
/* Task executes in a thread */
auto result = future.get();           // Get result
```

### HeapCallable_Cancelable
Extends `HeapCallable_Async` with cancellation capability by obtaining a task controller via `get_controller()`:
```cpp
auto callable = make_callable_cancelable(func);
auto controller = callable.get_controller();  // Get controller

/* Task executes in a thread */

if (controller.cancel()) {  // Successfully canceled the task
    // controller.get() will throw "Task canceled"
} else {                   // Task has started execution
    controller.get();       // Get result normally
}
```