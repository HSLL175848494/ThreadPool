# Test Report v1.1.3  

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
  - Queue capacity: 8,192  
  - Task container: `TaskStack<24,8>`  
  - Queue grouping: Enabled  

### Test Parameters  
| Parameter          | Configuration Value         |  
|--------------------|-----------------------------|  
| Total tasks        | 10,000,000 (10 million tasks)|  
| Result statistics  | Average of 10 test runs     |  
| Throughput unit    | Million tasks/second (M/s) |  

---  

## 3. Performance Test Results  

- **Single-producer mode**: 1 producer thread  
- **Multi-producer mode**: Producer threads = Worker threads  

### Single-Producer Mode  

**Tasks allocated on stack**:  

| Worker Threads | Task A (M/s) | Task B (M/s) | Task C (M/s) |  
|----------------|--------------|--------------|--------------|  
| 1              | 0.40         | 6.42         | 13.68        |  
| 4              | 1.41         | 6.60         | 7.23         |  
| 8              | 2.50         | 6.08         | 7.61         |  

**Tasks allocated on heap**:  

| Worker Threads | Task A (M/s) | Task B (M/s) | Task C (M/s) |  
|----------------|--------------|--------------|--------------|  
| 1              | 0.40         | 4.43         | 4.20         |  
| 4              | 1.49         | 2.88         | 2.94         |  
| 8              | 2.62         | 2.80         | 2.84         |  

### Multi-Producer Mode  

**Tasks allocated on stack**:  

| Worker Threads | Task A (M/s) | Task B (M/s) | Task C (M/s) |  
|----------------|--------------|--------------|--------------|  
| 2              | 0.81         | 11.50        | 17.23        |  
| 4              | 1.45         | 11.44        | 19.39        |  
| 8              | 2.44         | 12.24        | 21.19        |  

**Tasks allocated on heap**:  

| Worker Threads | Task A (M/s) | Task B (M/s) | Task C (M/s) |  
|----------------|--------------|--------------|--------------|  
| 2              | 0.77         | 5.35         | 5.91         |  
| 4              | 1.39         | 7.40         | 8.92         |  
| 8              | 2.29         | 9.91         | 11.34        |  

---  

## 4. Test Environment  
| Component      | Specifications               |  
|----------------|------------------------------|  
| Compiler       | MSVC v143 x64                |  
| Optimization   | /O2 (Maximize Speed)         |  
| CPU            | Intel i7-11800H (8C/16T)     |  
| Base frequency | 2.3 GHz                      |  
| OS             | Windows 10/11 x64 19045.5737 |  