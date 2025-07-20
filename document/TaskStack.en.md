# TaskStack - Stack-Based Task Container

## Overview
`TaskStack` is a stack memory-based task container template class capable of storing tasks and their parameters without using heap memory.

## Class Template Declaration
```cpp
template <unsigned int TSIZE = 64, unsigned int ALIGN = 8>
class TaskStack;
```

## Template Parameters
| Parameter | Description                                                                 | Constraints |
|-----------|-----------------------------------------------------------------------------|-------------|
| `TSIZE`   | Stack storage area size (bytes)                                             | `≥24` and must be multiple of `ALIGN` |
| `ALIGN`   | Maximum allowed alignment value for all components (callable object + each parameter) | `≥alignof(void*)` |

## Public Member Types

### `is_stored_on_stack`
```cpp
template <class F, class... Args>
struct is_stored_on_stack {
    static constexpr bool value = /* Compile-time calculation result */;
};
```
- **Function**: Checks if a specified task is stored directly on the stack
- **Return Value**:
  - `true`: Task size ≤ `TSIZE` and all component alignments ≤ `ALIGN`
  - `false`: Task requires heap storage

## Constructors

### Task Construction
```cpp
template <class F, class... Args>
TaskStack(F&& func, Args&&... args);
```
- **Function**: Constructs and stores a task
- **Parameters**:
  - `func`: Callable object (function pointer/lambda/function object)
  - `args...`: Task parameters (perfectly forwarded)
- **Storage Decision**:
  - Stack storage: When `sizeof(task) ≤ TSIZE` and `alignof(task) ≤ ALIGN`
  - Heap storage: Otherwise (wrapped using `HeapCallable`)
- **Parameter Handling Rules**:
  1. **Type Decay**: All parameters are stored by value (removing references and cv-qualifiers)
  2. **Lvalue Passing**: Parameters are always passed as lvalues during execution

```cpp
// Correct reference passing example
int value = 42;
TaskStack task([](int& v) { v *= 2; }, std::ref(value));

// Incorrect reference passing example
TaskStack bad_task([](int& v) {v *= 2;},value);  // Stores a copy of value internally

// Prohibited function rvalue signature
TaskStack bad_task2([](int&& v) {}, 5);  // Compilation error
```

## Public Member Functions

### `execute`
```cpp
void execute();
```
- **Function**: Synchronously executes the stored task
- **Key Features**:
  - Parameters are always passed to the callable object as **lvalues**
  - Guaranteed not to throw any exceptions (noexcept guarantee)
- **Precondition**: The object must contain a valid task

### `is_copyable`
```cpp
bool is_copyable();
```
- **Function**: Checks if the task is copyable
- **Return Value**:
  - `true`: Underlying callable object supports copying
  - `false`: Contains non-copyable objects (e.g., std::packaged_task)
- **Note**: Always verify this property before copy construction

### `is_moveable`
```cpp
bool is_moveable();
```
- **Function**: Checks if the task is movable
- **Return Value**: Always returns `true`
- **Note**: Move operations are always safe and effective

## Usage Notes

### Reference Passing Specification
```cpp
// Correct reference passing (using std::ref)
std::vector data;
TaskStack task([](auto& vec) { 
    vec.push_back(42); 
}, std::ref(data));

// Incorrect approach (value copy)
TaskStack bad_task([](auto vec) {...}, data); // Data copy occurs
```

### Handling Non-Copyable Objects

**Since TaskStack as a container must enable copy/move constructors, the true copyability/movability of tasks can only be detected at runtime**

```cpp
std::packaged_task<void()> pt; // Non-copyable object
TaskStack t1(std::move(pt));   // OK
if (!t1.is_copyable()) {
    // Prohibit copy operations
    // TaskStack t2(t1); // Prints error and terminates process via std::abort()!
}
```