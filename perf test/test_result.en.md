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
**hsll_tp**：   HSLL::ThreadPool  

| Thread Pool Type               | Threads | Task A (M/s) | Task B (M/s) | Task C (M/s) |  
|--------------------------------|---------|--------------|--------------|--------------|  
| **sp_tp**                      | 1       | 0.36         | 2.36         | 1.75         |  
|                                | 4       | 1.10         | 0.95         | 0.93         |  
|                                | 8       | 0.65         | 0.62         | 0.61         |  
| **hsll_tp**                    | 1       | 0.36         | 5.14         | 14.24        |  
|                                | 4       | 1.44         | 3.15         | 3.05         |  
|                                | 8       | 2.30         | 1.80         | 1.94         |  
| **hsll_tp (32 batch)**         | 1       | 0.40         | 14.75        | 23.54        |  
|                                | 4       | 1.53         | 4.32         | 5.87         |  
|                                | 8       | 2.63         | 2.85         | 3.62         |  
| **hsll_tp (32 submit + 32 batch)** | 1       | 0.42         | 26.27        | 33.29        |  
|                                | 4       | 1.57         | 62.63        | 84.29        |  
|                                | 8       | 2.65         | 44.34        | 52.79        |  

## 3. Test Environment  
| Component       | Configuration Details              |  
|-----------------|------------------------------------|  
| Compiler        | MSVC v143 x64                      |  
| Optimization    | /O2 Maximum optimization           |  
| Processor       | Intel i7-11800H (8C/16T)           |  
| Base Frequency  | 2.3 GHz                            |  
| Operating System| Windows 10/11 x64 19045.5737       |