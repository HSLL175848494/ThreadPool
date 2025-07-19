# TaskStack - Stack-Based Task Container  

## Overview  
`TaskStack` is a template class for stack-based task containers that stores tasks and their parameters without using heap memory. It employs perfect argument forwarding and compile-time calculations to ensure all task storage is allocated entirely on the stack.  

## Parameter Passing Mechanism  

### Storage Phase  
- **Perfect Forwarding Storage**: Preserves lvalue/rvalue semantics when constructing stored copies  
- **Parameter Type Handling**: Uses `std::decay` to remove references and cv-qualifiers  
- **Example**:  
  ```cpp  
  std::string s = "hello";  
  // Move-constructs parameter copy  
  TaskStack<> task([](std::string& s){ /*...*/ }, std::move(s));   
  ```  

### Execution Phase  
- **Lvalue Passing**: Task parameters are always passed as lvalues during execution  
- **Critical Limitation**:  
  ```cpp  
  // Error: rvalue reference parameters unsupported  
  TaskStack<> task([](std::string&& s){}, "tmp");  
  ```  

## Compile-Time Queries  
```cpp  
// Get required storage size for task  
constexpr size_t size_c11 = task_stack<F, Args...>::size;  
constexpr size_t size_c14 = task_stack_V<F, Args...>;  

// Check if task can be stored  
using MyTaskStack = TaskStack<1024, 16>;  
constexpr bool valid_c11 = MyTaskStack::task_invalid<F, Args...>::value;  
constexpr bool valid_c14 = MyTaskStack::task_invalid_v<F, Args...>;  
```  

## Key Considerations  
1. **Rvalue Parameter Restriction**: Task function signatures must not contain rvalue reference parameters  
2. **Object Lifetime**:  
   - Reference-captured parameters must remain valid during task execution  
   - Prefer reference capture for long-lived objects  
3. **Storage Overflow**: Tasks exceeding available stack space cause compile-time errors  
4. **Move Semantics**:  
   - Source objects become invalid after move construction  
   - Task objects themselves support move semantics  
5. **Alignment Requirements**:  
   - Storage space must satisfy `ALIGN` alignment requirement  
   - Default alignment is `alignof(std::max_align_t)`  
6. **Exception Safety**:  
   - Copy/move constructors of callable objects and parameters must not throw exceptions  

## Critical Warning: Copy/Move Requirements for Task Parameters  

### Core Issue  
Since the design requires enabling copy/move constructors:  
1. Task copy/move capability **can only be detected at runtime**  
2. When storing non-copyable/non-movable objects:  
   - Compilation succeeds  
   - Constructor fails at runtime  

### Dangerous Example  
```cpp  
std::packaged_task<void()> task_func([]{});
TaskStack<> task1(std::move(task_func));  // Correct: move construction  

// Compiles but crashes at runtime!  
TaskStack<> task2(task1);  // Attempts to copy non-copyable object  
```  
---  

## HeapCallable - Heap-Allocated Task Wrapper  

### Purpose  
Supplements stack-based tasks by providing automatic heap memory management for large tasks  

### Core Features  
1. **Automatic Memory Management**: Parameters stored on heap with reference-counted lifetime  
2. **Dynamic Space Allocation**: Suitable for storing large task objects  
3. **Exception Safety**: Construction may throw `std::bad_alloc`  

### Construction Methods  
```cpp  
// Factory function construction  
auto callable = make_callable([](int x){}, 42);  

// Direct construction (requires explicit typing)  
auto p = [](int x){}；  
HeapCallable<decltype(p),int> callable(p, 42);  

// Execute task  
callable();  
```  

### Integration with TaskStack  
```cpp  
// Explicit heap allocation  
TaskStack<> task(make_callable(heavy_task, large_data));  

// Automatic storage strategy selection (stack/heap)  
auto task = TaskStack<>::make_auto(  
    [](const BigData& data){},  
    std::move(big_data)  // Automatically uses heap if exceeds stack capacity  
);  

task.execute();  // Unified execution interface  
```  

# Extensions: HeapCallable_Cancelable & HeapCallable_Async  

## HeapCallable_Async - Asynchronous Task Wrapper  

### Purpose  
Provides asynchronous execution mechanism using `std::promise/std::future` for result retrieval  

### Core Features  
1. **Asynchronous Result Passing**: Returns task results via promise/future mechanism  
2. **Exception Capture**: Automatically captures task exceptions and propagates to future  
3. **Type Safety**: Strict matching between return type and promise type  
4. **Resource Management**: Uses unique_ptr to ensure single-execution semantics  

### Construction  
```cpp  
// Factory construction (recommended)  
auto async_task = make_callable_async<ReturnType>(  
    [](int x) -> ReturnType { /* ... */ },  
    42  
);  

// Direct construction  
HeapCallable_Async<ReturnType, decltype(func), int>  
    task_async(std::move(func), 42);  
```  

### Execution & Result Retrieval  
```cpp  
// Get future (must be called before execution)  
std::future<ReturnType> fut = async_task.get_future();  

// Execute task (typically in separate thread)  
async_task();  

// Get result (blocks until completion)  
ReturnType result = fut.get();  
```  

### Exception Handling  
```cpp  
try {  
    fut.get();  
}  
catch (const std::exception& e) {  
    // Handle task-thrown exceptions  
}  
```  

### Lifetime Rules  
1. **Move Semantics**: Supports move construction, prohibits copying  
2. **Single Execution**: Each object can only execute once  
3. **Single Future Access**: get_future() can only be called once  

---  

## HeapCallable_Cancelable - Cancelable Asynchronous Task  

### Purpose  
Adds task cancellation capability to asynchronous tasks  

### Core Features  
1. **Atomic Cancellation Flag**: Implements thread-safe cancellation using std::atomic<bool>  
2. **Cancellation Controller**: Provides cancellation API via Controller object  
3. **Cancellation Exception**: Sets std::runtime_error exception upon cancellation  
4. **Shared State**: Uses shared_ptr for state sharing between controller and task  

### Construction  
```cpp  
// Factory construction  
auto cancelable_task = make_callable_cancelable<ReturnType>(  
    [](int x) { /* ... */ },  
    42  
);  

// Direct construction  
HeapCallable_Cancelable<ReturnType, decltype(func), int>  
    task_cancelable(func, 42);  
```  

### Controller Usage  
```cpp  
// Get controller  
auto controller = cancelable_task.get_controller();  

// Attempt cancellation  
if (controller.cancel()) {  
    std::cout << "Task canceled\n";  
} else {  
    std::cout << "Cancellation failed (task already started)\n";  
}  
```  

### Cancellation Mechanism  

```cpp  
   bool cancel() // Cancel task  
   {  
      assert(storage); // Task container  
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

   void operator()() // Execute task  
   {  
      assert(storage); // Task container  
      bool expected = false;  
      auto& flag = std::get<1>(*storage);  

      if (flag.compare_exchange_strong(expected, true))  
         invoke();  
   }  
```  
1. **Cancellation Result**:  
   - Success: Returns true, sets "Task canceled" exception in promise  
   - Failure: Returns false (task already executing or completed)  

2. **Execution Check**: Skips execution if cancellation flag is already set  

## Comparison of Three HeapCallable Types  

| Feature               | HeapCallable | HeapCallable_Async | HeapCallable_Cancelable |  
|-----------------------|--------------|--------------------|-------------------------|  
| Return Value Retrieval| ❌           | ✅ (future)         | ✅ (future)             |  
| Exception Propagation | ❌           | ✅                  | ✅                      |  
| Task Cancellation     | ❌           | ❌                  | ✅                      |  
| Thread-Safe Execution | ✅           | ✅                  | ✅ (atomic ops)         |  
| Memory Management     | unique_ptr   | unique_ptr          | shared_ptr              |  
| Typical Use Case      | Simple tasks | Result-dependent tasks | Interruptible long tasks |