## HSLL::TPBlockQueue 高性能阻塞队列组件

### 概述
专为线程池设计的高性能阻塞队列，通过预分配内存池、批量操作优化和双端插入机制实现高吞吐量。核心特性包括双端插入支持、原位构造优化和批量操作控制。

---

### 性能优化

1. **预分配连续内存池**
   - 初始化时一次性分配对齐内存块（64字节对齐）
   - 节点组织为循环缓冲区，消除运行时内存分配开销
   - 内存布局优化缓存局部性（Cache Locality）

2. **零动态内存分配**
   - 所有节点在`init()`阶段预创建
   - 元素操作使用placement new构造/析构
   - 避免运行时内存分配导致的线程竞争

3. **平凡析构优化**
   ```cpp
   template <typename T, bool IsTrivial>
   struct DestroyHelper { /* 条件跳过析构 */ };
   ```
   - 对`std::is_trivially_destructible`类型跳过显式析构
   - 减少无意义的析构调用开销

4. **批量操作优化**
   - 支持`pushBulk()`/`popBulk()`批量处理
   - 单次锁持有期内处理多个元素
   - 减少锁竞争和条件变量通知频率

5. **分支预测优化**
   ```cpp
   #define LIKELY(x) __builtin_expect(!!(x), 1)  // GCC/Clang
   ```
   - 标记高频路径（如队列非空/非满）
   - 降低CPU流水线中断概率

---

### 核心特性

#### 1. 双端插入支持
```cpp
enum INSERT_POS { TAIL, HEAD };  // 插入位置枚举
```
- **尾插(TAIL)**：默认模式，标准FIFO行为
- **头插(HEAD)**：高优先级插入，实现LIFO语义
- 支持所有插入操作：`emplace()`, `push()`, `pushBulk()`等

#### 2. 原位构造优化
```cpp
template<INSERT_POS POS = TAIL, typename... Args>
bool emplace(Args&&... args);
```
- 避免临时对象创建
- 完美转发参数直接构造元素
- 支持阻塞/非阻塞双版本：
  - `wait_emplace()`：无限等待
  - `wait_emplace(timeout)`：超时等待

#### 3. 批量操作控制
```cpp
enum BULK_CMETHOD { COPY, MOVE };  // 批量构造方法
template<BULK_CMETHOD, INSERT_POS>
unsigned pushBulk(TYPE*, unsigned);
```
- 支持拷贝/移动语义
- 可选择头插或尾插模式
- 三种等待模式：
  - 立即返回：`pushBulk()`
  - 无限等待：`wait_pushBulk()`
  - 超时等待：`wait_pushBulk(timeout)`

#### 4. 双模式元素提取
```cpp
enum POP_METHOD { ASSIGN, PLACE };
template<POP_METHOD M = ASSIGN>
bool pop(TYPE& element);
```
- **ASSIGN模式**：移动赋值（默认）
- **PLACE模式**：原地构造（适用于无默认构造的对象）

#### 5. 优雅停止机制
```cpp
void stopWait();
```
- 标记停止状态
- 广播通知所有等待线程退出
- 避免线程永久阻塞

---

### 接口说明

#### 初始化与销毁
| 方法          | 说明                          |
|---------------|-------------------------------|
| `init()`      | 初始化队列（分配内存）        |
| `release()`   | 销毁所有元素并释放内存        |

#### 单元素操作
| 方法                          | 说明                          |
|-------------------------------|-------------------------------|
| `emplace<POS>(args...)`       | 原位构造（非阻塞）            |
| `wait_emplace<POS>(args...)`  | 原位构造（无限等待）          |
| `wait_emplace<POS>(timeout, args...)` | 原位构造（超时等待）      |
| `push<POS>(element)`          | 插入元素（非阻塞）            |
| `wait_push<POS>(element)`     | 插入元素（无限等待）          |
| `wait_push<POS>(element, timeout)` | 插入元素（超时等待）      |
| `pop<METHOD>(element)`        | 提取元素（非阻塞）            |
| `wait_pop<METHOD>(element)`   | 提取元素（无限等待）          |
| `wait_pop<METHOD>(element, timeout)` | 提取元素（超时等待）      |

#### 批量操作
| 方法                                        | 说明                          |
|---------------------------------------------|-------------------------------|
| `pushBulk<CMETHOD, POS>(elements, count)`   | 批量插入（非阻塞）            |
| `wait_pushBulk<CMETHOD, POS>(elements, count)` | 批量插入（无限等待）        |
| `wait_pushBulk<CMETHOD, POS>(elements, count, timeout)` | 批量插入（超时等待）  |
| `popBulk<METHOD>(elements, count)`          | 批量提取（非阻塞）            |
| `wait_popBulk<METHOD>(elements, count)`     | 批量提取（无限等待）          |
| `wait_popBulk<METHOD>(elements, count, timeout)` | 批量提取（超时等待）    |

---

### 使用示例

#### 高优先级任务插入
```cpp
// 头部插入紧急任务（LIFO）
queue.wait_emplace<HSLL::HEAD>(/* 构造参数 */);  

// 头部批量插入高优先级任务
Task highPriTasks[5];
queue.pushBulk<HSLL::MOVE, HSLL::HEAD>(highPriTasks, 5);
```

#### 标准任务处理
```cpp
// 尾部添加普通任务（FIFO）
queue.emplace(/* 构造参数 */);  

// 批量提取任务处理
Task tasks[10];
unsigned count = queue.wait_popBulk(tasks, 10);
for (unsigned i = 0; i < count; ++i) {
    process_task(tasks[i]);
}
```

#### 带超时的操作
```cpp
// 超时等待插入
if (!queue.wait_push<HSLL::TAIL>(task, std::chrono::milliseconds(100))) {
    handle_timeout();
}

// 超时等待提取
Task output;
if (queue.wait_pop(output, std::chrono::seconds(1))) {
    process_task(output);
}
```

---

### 内存管理

| 操作         | 行为                              |
|--------------|-----------------------------------|
| `init()`     | 对齐分配内存+构造循环缓冲区       |
| `release()`  | 析构有效元素+释放内存块           |
| 元素操作     | placement new+条件析构            |

通过`ALIGNED_MALLOC`/`ALIGNED_FREE`实现跨平台（Windows/Linux）对齐内存管理

---

### 同步机制

1. **单锁设计** 
   - 使用`std::mutex`保护所有状态变更
   - 避免细粒度锁的开销和死锁风险

2. **双条件变量**
   ```cpp
   std::condition_variable notEmptyCond;  // 消费者等待
   std::condition_variable notFullCond;   // 生产者等待
   ```
   - 精确唤醒：单元素操作触发`notify_one()`
   - 批量操作触发`notify_all()`或`notify_one()`