# Thread Pool Performance Test Report  

## 1. Reference Thread Pool  
**simple_threadpool**: A basic standard open-source thread pool implementation from GitHub ([Source Code](https://github.com/progschj/ThreadPool.git))  

## 2. Task Types  
The test includes three computation tasks with different densities:  

```cpp  
unsigned int k; // Use global variable to prevent loop optimization  

// High-computation-density task  
void A() {  
   for (int i = 0; i < 10000; i++)  
      k = k * 2 + i; // ~10,000 operations  
}  

// Medium-computation-density task  
void B() {  
   for (int i = 0; i < 100; i++)  
      k2 = k2 * 2 + i; // ~100 operations  
}  

// Empty task (extremely lightweight)  
void C() {  

}  
```  

## 3. Test Results (Throughput: Million tasks/s)  

使用```hsll_tp```代表```HSLL::ThreadPool```


| 线程池类型               | 线程数 | 任务A (M/s) | 任务B (M/s) | 任务C (M/s) |
|--------------------------|--------|-------------|-------------|-------------|
| **simple_threadpool**    | 1      | 0.36        | 2.36        | 1.75        |
|                          | 4      | 1.10        | 0.95        | 0.93        |
|                          | 8      | 0.65        | 0.62        | 0.61        |
| **hsll_tp**              | 1      | 0.36        | 5.14        | 14.24       |
|                          | 4      | 1.44        | 3.15        | 3.05        |
|                          | 8      | 2.30        | 1.80        | 1.94        |
| **hsll_tp(32批处理)**     | 1      | 0.40        | 14.75       | 23.54       |
|                          | 4      | 1.53        | 4.32        | 5.87        |
|                          | 8      | 2.63        | 2.85        | 3.62        |
| **hsll_tp(32批提交+32批处理)** | 1      | 0.42        |26.27       | 33.29   |
|                          | 4      | 1.57        | 62.63        | 84.29      |
|                          | 8      | 2.65        | 44.34        | 52.79      |

## 4. Test Environment  
| Component       | Configuration Details             |  
|-----------------|-----------------------------------|  
| Compiler        | MSVC v143 x64                     |  
| Optimization    | /O2 Maximize Speed                |  
| Processor       | Intel i7-11800H (8C/16T)          |  
| Base Frequency  | 2.3 GHz                           |  
| OS              | Windows 10/11 x64 19045.5737      |  
