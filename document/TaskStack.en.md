# TaskStack - Stack-Allocated Task Container

## Overview
`TaskStack` is a stack-based task container template class that stores tasks and their parameters without using heap memory. It employs perfect argument forwarding and compile-time computation to ensure complete stack allocation of task storage space.

## Parameter Passing Mechanism

### Storage Stage
- **Perfect Forwarding**: Preserves lvalue/rvalue semantics of original values when constructing stored copies
- **Parameter Type Handling**: Uses `std::decay` to remove references and cv-qualifiers
- **Example**:
  ```cpp
  std::string s = "hello";
  // Move-constructs stored parameter copy
  TaskStack<> task([](std::string& s){ /*...*/ }, std::move(s)); 
  ```

### Execution Stage
- **Lvalue Passing**: Task parameters are always passed as lvalues during execution
- **Critical Limitation**:
  ```cpp
  // Error example: rvalue reference parameters not supported
  TaskStack<> task([](std::string&& s){}, "tmp");
  ```

### Efficiency Recommendations
```cpp
// 1. Explicit ownership transfer when move semantics needed
task.execute([](std::string& s) {
    auto local_str = std::move(s); // Safe ownership transfer
});

// 2. Capture large long-lived objects by reference
LargeObject obj;
auto lambda = [&obj]{ /* Use obj */ };
TaskStack<> task(lambda); // Only stores lambda object
```

## Compile-Time Queries
```cpp
// Get required storage size for task
constexpr size_t size_c11 = task_stack<F, Args...>::size;
constexpr size_t size_c14 = task_stack_V<F, Args...>;

// Verify task storability
using MyTaskStack = TaskStack<1024, 16>;
constexpr bool valid_c11 = MyTaskStack::task_invalid<F, Args...>::value;
constexpr bool valid_c14 = MyTaskStack::task_invalid_v<F, Args...>;
```

## Critical Considerations
1. **Rvalue Parameter Ban**: Task function signatures must not contain rvalue reference parameters
2. **Object Lifetime**:
   - Captured references must remain valid during task execution
   - Use reference capture for long-lived objects
3. **Storage Overflow**: Tasks exceeding available stack space cause compile-time errors
4. **Move Semantics**:
   - Source objects become invalid after move construction
   - Task objects themselves support move semantics
5. **Alignment Requirements**:
   - Storage space must satisfy `ALIGN` requirements
   - Default alignment: `alignof(std::max_align_t)`
6. **Exception Safety**:
   - Copy/move operations of callables/parameters must not throw exceptions

## Critical Warning: Parameter Copy/Move Requirements

### Core Issue
Since copy/move constructors must be enabled:
1. Task copy/move capability can **only be detected at runtime**
2. When storing non-copyable/movable objects:
   - Compilation succeeds
   - Constructor fails at runtime

### Hazard Example
```cpp
std::packaged_task<void()> task_func([]{});
TaskStack<> task1(std::move(task_func));  // Correct: move construction

// Compiles but crashes at runtime!
TaskStack<> task2(task1);  // Attempts to copy non-copyable object
```

---

## HeapCallable - Heap-Allocated Task Wrapper

### Purpose
Complements stack-based tasks with automatic heap memory management for large tasks

### Key Features
1. **Automatic Memory Management**: Parameters stored on heap with reference-counted lifetime
2. **Dynamic Space Allocation**: Suitable for large task objects
3. **Exception Safety**: Construction may throw `std::bad_alloc`

### Construction Methods
```cpp
// Factory construction (recommended)
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

// Automatic storage selection (stack or heap)
auto task = TaskStack<>::make_auto(
    [](const BigData& data){}, 
    std::move(big_data)  // Automatically uses heap if exceeds stack capacity
);

task.execute();  // Unified execution interface
```

### Usage Restrictions
```cpp
// Nesting prohibition
auto c1 = make_callable([]{});
auto c2 = make_callable(c1);  // Compile-time error!
```

## Best Practices Summary
1. **Small Tasks**: Prefer direct storage in `TaskStack`
2. **Large Tasks**: Use `make_auto()` for automatic storage selection
3. **Parameter Ownership**:
   - Short-term: Value storage + internal `std::move`
   - Long-term: Reference capture
4. **Error Handling**:
   - Compile-time: Verify with `task_invalid_v`
   - Runtime: Validate object movability