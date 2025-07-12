# Thread Pool Performance Test Report  

## 1. Task Types  
The test includes three tasks with different computational densities:  

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

## 2. Test Results (Throughput: Million tasks/second)  

**sp_tp**：  [Open-source simple thread pool implementation](https://github.com/progschj/ThreadPool.git)  

**boost_tp**：  boost::asio::thread_pool

**hsll_tp**：   HSLL::ThreadPool  queuelen:10000 container:TaskStack<24,8>


| Thread Pool Type               | Thread Count | Task A (M/s) | Task B (M/s) | Task C (M/s) |
|--------------------------------|--------------|--------------|--------------|--------------|
| **sp_tp**                      | 1            | 0.36         | 2.36         | 1.75         |
|                                | 4            | 1.10         | 0.95         | 0.93         |
|                                | 8            | 0.65         | 0.62         | 0.61         |
| **boost_tp**                   | 1            | 0.40         | 2.87         | 2.70         |
|                                | 4            | 1.31         | 0.94         | 0.90         |
|                                | 8            | 0.80         | 0.65         | 0.64         |
| **hsll_tp**                    | 1             | 0.42        | 5.41        | 13.81       |
|                                | 4             | 1.45        | 3.40        | 3.55        |
|                                | 8             | 2.56        | 1.85        | 2.48        |
| **hsll_tp (32 Batch Processing)** | 1            | 0.42         | 17.01        | 22.50        |
|                                | 4            | 1.60         | 5.26         | 6.24         |
|                                | 8            | 2.72         | 2.94         | 3.82         |
| **hsll_tp (32 Batch Submission + 32 Batch Processing)** | 1      | 0.41         | 24.22        | 62.49        |
|                                | 4            | 1.53         | 50.09        | 60.17        |
|                                | 8            | 2.65         | 36.89        | 42.92        |  


## 3. Test Environment  
| Component       | Configuration Details              |  
|-----------------|------------------------------------|  
| Compiler        | MSVC v143 x64                      |  
| Optimization    | /O2 Maximum optimization           |  
| Processor       | Intel i7-11800H (8C/16T)           |  
| Base Frequency  | 2.3 GHz                            |  
| Operating System| Windows 10/11 x64 19045.5737       |