### Thread Pool Performance Test Report  
Test Version: v1.0.1  

---

#### 1. Task Types  
Three tasks with varying computational densities were tested:  

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
void C() { }  
```

---

#### 2. Test Results (Throughput: Million Tasks/Sec)  

- **hsll_tp**: `HSLL::ThreadPool`  
  - Queue Length: 8192  
  - Container: `TaskStack<24,8>`  
  - queue partitioning： enabled

| Thread Pool Type                     | Threads | Task A (M/s) | Task B (M/s) | Task C (M/s) |  
|--------------------------------------|---------|--------------|--------------|--------------|  
| **hsll_tp**                          | 1       | 0.40         | 6.42         | 13.68        |  
|                                      | 4       | 1.41         | 6.60         | 7.23         |  
|                                      | 8       | 2.50         | 6.08         | 7.61         |  
| **hsll_tp (32-batch processing)**    | 1       | 0.42         | 16.82        | 20.26        |  
|                                      | 4       | 1.52         | 9.28         | 10.89        |  
|                                      | 8       | 2.65         | 9.81         | 11.09        |  
| **hsll_tp (32-batch submit+process)**| 1       | 0.41         | 24.95        | 61.07        |  
|                                      | 4       | 1.51         | 45.43        | 64.88        |  
|                                      | 8       | 2.69         | 44.98        | 66.20        |  

> **Notes**:  
> - Throughput unit: **Million tasks per second (M/s)**.  
> - **Batch processing**: Tasks executed in groups (e.g., 32 tasks/batch).  
> - **Batch submit+process**: Tasks submitted *and* executed in batches.  

---

#### 3. Test Environment  
| Component        | Configuration                   |  
|------------------|---------------------------------|  
| Compiler         | MSVC v143 x64                   |  
| Optimization     | `/O2` (Maximize Speed)          |  
| CPU              | Intel i7-11800H (8C/16T)       |  
| Base Frequency   | 2.3 GHz                         |  
| OS               | Windows 10/11 x64 (Build 19045.5737) |