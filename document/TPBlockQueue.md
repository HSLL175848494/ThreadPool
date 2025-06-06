### HSLL::TPBlockQueue 高性能阻塞队列组件

#### 设计目标
专为线程池内部设计的高性能阻塞队列，通过以下核心机制优化并发性能：

---

#### 关键性能优化

1. **预分配连续内存池**
   - 初始化时一次性分配对齐内存块（64字节对齐）
   - 节点组织为循环链表，消除运行时内存分配开销
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

#### 核心特性

1. **双模式元素转移**
   ```cpp
   enum POP_METHOD { ASSIGN, PLACE };
   ```
   - `ASSIGN`：移动赋值
   - `PLACE`：原地构造（适用于无默认构造的对象）

2. **灵活的构造语义**
   ```cpp
   enum BULK_CMETHOD { COPY, MOVE };
   template<BULK_CMETHOD METHOD>
   void pushBulk(TYPE*, uint);
   ```
   - 批量操作可选择拷贝/移动语义

3. **多维度阻塞控制**
   | 操作类型       | 立即返回 | 无限等待       | 超时等待                |
   |----------------|----------|----------------|-------------------------|
   | 单元素操作     | `push()` | `wait_push()`  | `wait_push(timeout)`    |
   | 单元素提取     | `pop()`  | `wait_pop()`   | `wait_pop(timeout)`     |
   | 批量操作       | `pushBulk()` | `wait_pushBulk()` | `wait_pushBulk(timeout)`|

4. **优雅停止机制**
   ```cpp
   void stopWait();
   ```
   - 标记停止状态
   - 广播通知所有等待线程退出
   - 避免线程永久阻塞
---

#### 同步机制

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

---

#### 内存管理

| 操作         | 行为                              |
|--------------|-----------------------------------|
| `init()`     | 对齐分配内存+构造循环链表         |
| `release()`  | 析构有效元素+释放内存块           |
| 元素操作     | placement new+条件析构            |

> 通过`ALIGNED_MALLOC`/`ALIGNED_FREE`实现跨平台（Windows/Linux）对齐内存管理

---

#### 适用场景
- 线程池任务队列
- 高吞吐生产者-消费者场景
- 要求低延迟的固定大小缓冲池
- 需要避免动态内存分配的实时系统