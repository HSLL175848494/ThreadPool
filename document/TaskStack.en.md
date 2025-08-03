# TaskStack - Stack-Based Task Container

## Overview
`TaskStack` is a stack memory-based task container template class that stores tasks and their parameters without using heap memory.

## Class Template Declaration
```cpp
template <unsigned int TSIZE = 64, unsigned int ALIGN = 8>
class TaskStack;
```

## Template Parameters
| Parameter | Description                                                                 | Constraints |
|-----------|-----------------------------------------------------------------------------|-------------|
| `TSIZE`   | Stack storage area size (bytes)                                             | `≥24` and must be a multiple of `ALIGN` |
| `ALIGN`   | Maximum allowed alignment value for all components (callable object + each parameter) | `≥alignof(void*)` |

## Public Member Types

### `is_stored_on_stack`
```cpp
template <class F, class... Args>
struct is_stored_on_stack {
    static constexpr bool value = /* compile-time calculation result */;
};
```
- **Function**: Checks whether a specified task is stored directly on the stack
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
  - `func`: Callable object (function object/function pointer/lambda expression/functor/member function)
  - `args...`: Task parameters (perfectly forwarded)
- **Storage Decision**:
  - Stack storage: When `sizeof(task) ≤ TSIZE` and `alignof(task) ≤ ALIGN`
  - Heap storage: Otherwise (using `HeapCallable` wrapper)
- **Parameter Handling Rules**:
  1. **Type Decay**: All parameters are stored by value (removing references and cv-qualifiers)
  2. **Lvalue Passing**: Parameters are always passed as lvalues during execution

```cpp
// Correct reference passing example
int value = 42;
TaskStack task([](int& v) { v *= 2; }, std::ref(value));

// Incorrect reference passing example
TaskStack bad_task([](int& v) {v *= 2;},value);  // Stores a copy of value internally

// Prohibited rvalue function signature
TaskStack bad_task2([](int&& v) {}, 5);  // Compilation error
```

**Note**: Parameter types in the task container are deduced during construction from `Args&&`, which may not match the function signature.

Example:
```cpp
void example(long long a, float b){}

// In the constructor, 5/10.0 may be deduced as int/double types. 
// This may cause precision loss or data truncation - requires special attention
TaskStack task(example,5,10.0);
```
Explicitly specify types or use type casting to avoid these issues:
```cpp
TaskStack task(example,5ll,10.0f);
TaskStack task(example,(long long)5,(float)10.0);
```

## Public Member Functions

### `execute`
```cpp
void execute();
```
- **Function**: Synchronously executes the stored task
- **Key Features**:
  - Parameters are always passed to the callable object as **lvalues**
  - Prohibited from throwing any exceptions (noexcept guarantee)
- **Precondition**: The object must contain a valid task

## Usage Notes

### Reference Passing Guidelines
```cpp
// Correct reference passing (using std::ref)
std::vector data;
TaskStack task([](auto& vec) {...}, std::ref(data));

// Incorrect approach (value copy)
TaskStack bad_task([](auto vec) {...}, data); // Data copy occurs
```

## Extensions  

### HeapCallable  
When `TaskStack` lacks sufficient space to store a complete task, it first encapsulates the task as a `HeapCallable` object for storage (the static assertion `TSIZE > 24` ensures storability). Users can also directly construct a `HeapCallable` to store tasks independently:  
```cpp  
auto callable = make_callable(func);  
TaskStack task(callable);  
```  

### HeapCallable_Async  
To address `TaskStack`'s lack of native support for return values, `HeapCallable_Async` returns an `std::future` object via `get_future()` after construction. During task execution, it automatically handles value/exception propagation logic:  
```cpp  
auto callable = make_callable_async(Sum, 333, 333);  
auto future = callable.get_future();  // Obtain the future object  
/* Task executes in a thread */  
auto result = future.get();           // Retrieve the result  
```  

### HeapCallable_Cancelable  
Extends `HeapCallable_Async` with cancelable functionality. Obtain the task controller via `get_controller()`:  
```cpp  
auto callable = make_callable_cancelable(func);  
auto controller = callable.get_controller();  // Obtain the controller  

/* Task executes in a thread */  

if (controller.cancel()) {  // Successfully canceled the task  
    // controller.get() will throw "Task canceled"  
} else {                   // Task has started execution  
    controller.get();       // Retrieve the result normally  
}  
```  

### Custom Memory Allocator  
All three HeapCallable types manage memory via `tp_smart_ptr`, whose memory allocation/deallocation can be optimized through a global allocator:  
```cpp  
class AllocatorBase{ // Allocator base class  
public:
  virtual void* allocate(size_t size) const{
    return malloc(size);
  }

  virtual void deallocate(void* p) const{
    free(p);
  }
};

// Set the global allocator (pass nullptr to restore default)  
void set_tp_smart_ptr_allocator(const AllocatorBase* allocator = nullptr);  
```  
**Important Notes**:  
1. Before replacing the allocator, release all smart pointers relying on the original allocator.  
2. The allocator instance must remain valid while any smart pointer is alive.  
3. Typical usage pattern: Set the allocator during initialization, and restore default before exit.