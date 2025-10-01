# PriorityTaskManager

A lightweight priority-based task manager for C++  

---


## Features
- Submit tasks with a **priority** (lower value = higher priority)
- Manage tasks by **name** (join by name supported)
- Returns `std::future` for asynchronous result retrieval
- **Automatic cleanup**: finished jobs are removed immediately
- `join(name)` waits until all tasks with the given name complete
- `joinAll()` waits until all tasks finish
- **C++11 compatible** (uses `std::result_of` fallback)
- Thread-per-task model (not a worker pool)

---


## Installation
Simply add **PriorityTaskManager.h** to your project.  
No external dependencies required (only C++ standard library).

```bash
git clone https://github.com/audrms950/PriorityTaskManager.git
```

---


## Usage Example

```cpp
#include "PriorityTaskManager.h"
#include <iostream>

int task_c(int a, int b)
{
    std::cout << "Task C running (higher priority)\n";
    return a + b;
}

int main()
{
    PriorityTaskManager manager;

    // Submit tasks
    auto fut1 = manager.submit(1, "task_a", [] {
        std::cout << "Task A running\n";
    });

    auto fut2 = manager.submit(3, "task_b", [] {
        std::cout << "Task B running (higher priority)\n";
    });

    auto fut3 = manager.submit(2, "task_c", task_c, 1, 2);
    
    int task_c_result = fut3.get();
    std::cout << "task_c result : " << task_c_result << "\n";
    
    // Wait for specific job
    manager.join("task_b");

    // Wait for all jobs
    manager.joinAll();

    std::cout << "All tasks finished!\n";
}
```

---


## Design Notes
- **Thread-per-task**: Each submitted task spawns a new thread.
- **Not a pool**: Unlike thread pools, this avoids keeping idle worker threads.
- Suitable for **smaller modules or medium-sized workloads** where thread overhead is acceptable.
- For very large numbers of tasks, consider a worker pool for better scalability.

---


## License
MIT License.  
Free to use in personal and commercial projects.
