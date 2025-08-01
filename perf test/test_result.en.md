### Test Report v1.0.2  

---

## 1. Task Types  
Testing covers three computational density task models:  

```cpp
unsigned int k; // Global variable to prevent computation optimization

void A() {  
   for (int i = 0; i < 10000; i++)  
      k = k * 2 + i; // High computational density
}  

void B() {  
   for (int i = 0; i < 100; i++)  
      k = k * 2 + i; // Medium computational density
}  

void C() { }  // Empty task (extreme scenario)
```

## 2. Test Configuration  

### Thread Pool Parameters  
- **Thread Pool**: HSLL::ThreadPool  
  - Queue capacity: 8,192  
  - Task container: `TaskStack<24,8>`  (with sufficient space to store tasks on the stack without falling back to heap storage)
  - Queue grouping: Enabled  
- **Batch Processing Modes**:  
  - **32-Batch Processing**: Worker threads attempt to fetch 32 tasks per batch  
  - **32-Batch Submit + 32-Batch Processing**: Producers submit 32-task batches + workers process 32-task batches  

### Test Parameters  
| Parameter         | Value                     |  
|-------------------|---------------------------|  
| Total Tasks       | 10,000,000 (10 million)   |  
| Result Statistics | Average of 10 test runs   |  
| Throughput Unit   | Million tasks/sec (M/s)   |  

---

## 3. Performance Test Results  
### 3.1 Single-Producer Mode (1 producer thread)  
| Thread Pool Configuration    | Worker Threads | Task A (M/s) | Task B (M/s) | Task C (M/s) |
|------------------------------|----------------|--------------|--------------|--------------|
| **HSLL::ThreadPool**         | 1              | 0.40         | 6.42         | 13.68        |
|                              | 4              | 1.41         | 6.60         | 7.23         |
|                              | 8              | 2.50         | 6.08         | 7.61         |
| **32-Batch Processing**      | 1              | 0.42         | 16.82        | 20.26        | 
|                              | 4              | 1.52         | 9.28         | 10.89        |
|                              | 8              | 2.65         | 9.81         | 11.09        |
| **32-Batch Submit+Processing** | 1              | 0.41         | 24.95        | 61.07        |
|                              | 4              | 1.51         | 45.43        | 64.88        |
|                              | 8              | 2.69         | 44.98        | 66.20        |
---

### 3.2 Multi-Producer Mode (Producer threads = Worker threads)  
| Thread Pool Configuration    | Total Threads | Task A (M/s) | Task B (M/s) | Task C (M/s) |
|------------------------------|---------------|--------------|--------------|--------------|
| **HSLL::ThreadPool**         | 2             | 0.81         | 11.50        | 17.23        |
|                              | 4             | 1.45         | 11.44        | 19.39        |
|                              | 8             | 2.44         | 12.24        | 21.19        |
| **32-Batch Processing**      | 2             | 0.86         | 22.18        | 20.44        | 
|                              | 4             | 1.55         | 33.15        | 35.38        |
|                              | 8             | 2.55         | 39.15        | 45.39        |
| **32-Batch Submit+Processing** | 2             | 0.85         | 45.13        | 110.14       |
|                              | 4             | 1.59         | 78.78        | 193.42       |
|                              | 8             | 2.54         | 112.92       | 237.42       |

---

## 4. Test Environment  
| Component    | Specification               |  
|--------------|-----------------------------|  
| Compiler     | MSVC v143 x64               |  
| Optimization | /O2 (Maximize Speed)        |  
| CPU          | Intel i7-11800H (8C/16T)    |  
| Base Clock   | 2.3 GHz                     |  
| OS           | Windows 10/11 x64 19045.5737|