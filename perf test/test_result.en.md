# Thread Pool Performance Test Report  
Test Version: v1.0.1  

---

## 1. Test Task Types  
Three types of tasks with varying computational densities were tested:  

```cpp
unsigned int k; // Global variable to prevent loop optimization

// High computational density task
void A() {
   for (int i = 0; i < 10000; i++)
      k = k * 2 + i; // ~10,000 operations
}

// Medium computational density task
void B() {
   for (int i = 0; i < 100; i++)
      k2 = k2 * 2 + i; // ~100 operations
}

// Empty task (extremely lightweight)
void C() {

}
```

---

## 2. Test Results (Throughput: Million tasks/s)  
**sp_tp**: [Simple open-source thread pool](https://github.com/progschj/ThreadPool.git)  
**boost_tp**: boost::asio::thread_pool  
**hsll_tp**: HSLL::ThreadPool (Queue size: 8192, Container: TaskStack<24,8>, Dynamic Threads: Disabled)  

| Thread Pool Type               | Threads | Task A (M/s) | Task B (M/s) | Task C (M/s) |
|--------------------------------|---------|--------------|--------------|--------------|
| **sp_tp**                      | 1       | 0.36         | 2.36         | 1.75         |
|                                | 4       | 1.10         | 0.95         | 0.93         |
|                                | 8       | 0.65         | 0.62         | 0.61         |
| **boost_tp**                   | 1       | 0.40         | 2.87         | 2.70         |
|                                | 4       | 1.31         | 0.94         | 0.90         |
|                                | 8       | 0.80         | 0.65         | 0.64         |
| **hsll_tp**                    | 1       | 0.42         | 5.56         | 13.81        |
|                                | 4       | 1.45         | 3.44         | 3.59         |
|                                | 8       | 2.56         | 2.81         | 3.24         |
| **hsll_tp (32-batch)**         | 1       | 0.41         | 12.02        | 22.49        |
|                                | 4       | 1.53         | 6.95         | 8.48         |
|                                | 8       | 2.63         | 8.76         | 6.31         |
| **hsll_tp (32-submit+32-batch)**| 1       | 0.41         | 22.06        | 57.89        |
|                                | 4       | 1.51         | 40.72        | 74.34        |
|                                | 8       | 2.66         | 74.62        | 51.66        |

---

## 3. Test Environment  
| Component      | Configuration                  |
|----------------|--------------------------------|
| Compiler       | MSVC v143 x64                  |
| Optimization   | /O2 (Maximize Speed)           |
| Processor      | Intel i7-11800H (8C/16T)       |
| Base Frequency | 2.3 GHz                        |
| OS             | Windows 10/11 x64 19045.5737   |