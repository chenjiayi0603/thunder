# Thunder 线程池使用分析

> 分析日期：2026-06-14  
> 关联 issues：#91–#95（issus-list.md）

---

## 1. 背景

Thunder 是单线程事件循环模型（每个 Worker 进程一个 libev/io_uring 事件循环）。当业务逻辑需要执行**阻塞 IO**（同步 SDK、文件读写）或**CPU 密集计算**时，若直接在事件循环中执行会阻塞整个 Worker，导致所有连接的延迟飙升。

为此，Thunder 引入了一个线程池 + C++20 协程的 offload 机制：

```
Event Loop (Worker 单线程)
    │
    co_await MakePoolOffloadAwaiter(&step, [](Args...) { /* 耗时工作 */ }, args...)
    │                    │
    协程挂起              ▼
                   threadpool 子线程执行耗时工作
                   工作完成 → PostToEventLoop(resumeOnWorker)
                    │
    协程 resume ◄──┘  (回到 Worker 线程，可安全调用框架 API)
```

---

## 2. 涉及文件

| 文件 | 职责 |
|------|------|
| `code/Util/src/thread/threadpool.h` | 线程池实现（来自 lzpong/threadpool，注入 `namespace std`） |
| `code/Net/include/labor/WorkerThreadPool.hpp` | 全局池封装：`ThunderWorkerThreadPool()` |
| `code/Net/src/labor/WorkerThreadPool.cpp` | 懒初始化：默认 4 线程 |
| `code/Net/include/coro/ThreadPoolAwaitable.hpp` | `PoolOffloadAwaiter`、`MakePoolOffloadAwaiter`、`RunOnThreadPool` |
| `code/Net/src/labor/Worker.cpp:2499` | 读配置 `thread_pool_size` 显式初始化 |

### 实际使用点

| 文件 | 场景 |
|------|------|
| `HelloHttp/ModuleHello/HelloPoolBlock.cpp` | 阻塞 IO 模拟（sleep） |
| `HelloHttp/ModuleHello/HelloPoolCpu.cpp` | CPU 密集：256KB checksum |
| `HelloHttps/ModuleHello/ModuleHello.cpp` | 同上两种场景 |
| `HelloWs/CmdHello/CmdHello.cpp` | 同上两种场景 |

---

## 3. 现有实现分析

### 3.1 `threadpool.h` 实现结构

```cpp
namespace std {                    // ← 问题①：向 std 注入用户类
class threadpool {
    vector<thread> _pool;          // 固定大小线程数组
    queue<Task> _tasks;            // ← 问题③：单队列
    mutex _lock;                   // ← 问题③：全局锁
    condition_variable _task_cv;
    atomic<bool> _run{true};
    atomic<int>  _idlThrNum{0};

    template<class F, class... Args>
    auto commit(F&& f, Args&&... args) -> future<decltype(f(args...))>
    {
        // packaged_task + future 封装
        lock_guard<mutex> lock{_lock};   // ← 每次入队都加锁
        _tasks.emplace([task]{ (*task)(); });
        _task_cv.notify_one();
        return future;
    }
};
}
```

来源：https://github.com/lzpong/threadpool（2017 年作品，无维护）

### 3.2 全局池初始化

```cpp
// WorkerThreadPool.cpp
std::threadpool* g_thunderWorkerPool = nullptr;   // ← 问题④：裸指针

void InitThunderWorkerThreadPool(unsigned short threadCount) {
    if (g_thunderWorkerPool != nullptr) return;
    unsigned short n = threadCount == 0 ? 4 : threadCount;  // ← 问题②：默认 4
    g_thunderWorkerPool = new std::threadpool(n);  // ← 问题④：裸 new，从不 delete
}

std::threadpool& ThunderWorkerThreadPool() {
    if (g_thunderWorkerPool == nullptr)
        InitThunderWorkerThreadPool(4);   // ← 懒初始化也是硬编码 4
    return *g_thunderWorkerPool;
}
```

Worker.cpp:2499 会读配置 `thread_pool_size` 显式调用，但如果 `ThunderWorkerThreadPool()` 在配置读取前被调用（e.g. 某个静态初始化）仍会走 4 线程。

### 3.3 `PoolOffloadAwaiter` 设计

```
await_suspend:
  1. 把协程 handle 存入 step_->m_context.handle
  2. pool->commit(lambda):
       a. 执行 work(body, out)
       b. PostToEventLoop(resumeOnWorker)
  3. future<ResultT> 存入 fut_

await_resume:
  - fut_.get()  ← 此时已就绪（PostToEventLoop 在 get 前返回），不会阻塞

resumeOnWorker（在 Event Loop 线程执行）:
  - IsRegisteredStep(seq, step)  ← 防悬空：step 被销毁后返回 false
  - h.resume()
```

**这部分设计是正确且必要的**：`IsRegisteredStep` 保护了 step 在 co_await 等待期间被取消/超时销毁的场景。

---

## 4. 问题详解

### 问题① `namespace std` 注入 — 未定义行为

**C++ 标准（[namespace.std]/1）**：用户不得向 `namespace std` 添加声明，除非是对标准模板的特化。

```cpp
namespace std {
    class threadpool { ... };  // UB：违反 [namespace.std]/1
}
```

**实际风险**：
- 若未来某个 C++ 版本/编译器在 `std` 中定义了同名符号（如 C++26 的 `std::thread_pool`），会产生 ODR 违反，表现为编译失败或更糟糕的运行时未定义行为
- 现在用 `-Wpedantic` 或严格模式会警告

**修法**：将 `threadpool` 移入 `namespace util` 或 `namespace thunder`，全局替换使用点。

---

### 问题② 默认线程数硬编码 4，未读 CPU 核数

```cpp
InitThunderWorkerThreadPool(4);  // 16 核机器上只用 4 个线程
```

**影响**：
- CPU 密集型 offload（如 `HelloPoolCpu`）在高核数服务器上严重浪费算力
- 8 核 = 最多 4 个并发 offload；16 核 = 同样最多 4 个

**现代 C++ 做法**：
```cpp
unsigned hw = std::max(1u, std::thread::hardware_concurrency());
// 对于 offload 池，通常取核数的一半，留事件循环 + 其他线程用
unsigned n = std::max(2u, hw / 2);
```

Worker.cpp 有读配置的路径，问题在于没有合理默认值。

---

### 问题③ `std::queue<Task> + mutex`：高并发下锁竞争

当多个协程几乎同时 `co_await MakePoolOffloadAwaiter`（如压测），每次 `commit` 都会：

```
Thread A: lock_guard{_lock} → emplace → unlock → notify_one
Thread B: 等待 _lock ...
Thread C: 等待 _lock ...
```

所有入队操作串行化在一把锁上。线程数越多、提交越频繁，竞争越剧烈。

**对比：`parallel_for` / `std::execution::par`**  
这两者用于**数据并行**（对集合每个元素独立处理），底层通常使用 work-stealing 双端队列（每个线程一个本地队列 + 全局窃取），无需全局锁。但其使用场景是"一次提交 N 个任务处理同一数据"，不适合 Thunder 的"每请求提交一个独立任务"模式。

**修法选项**：

| 方案 | 复杂度 | 效果 |
|------|:------:|------|
| `moodycamel::ConcurrentQueue`（lock-free MPMC） | 低（header-only）| ✅ 高并发无锁入队 |
| `boost::lockfree::queue` | 低 | ✅ 类似 |
| Intel oneTBB `task_arena` | 高 | ✅ work-stealing，过重 |
| C++23 `std::execution`（P2300） | 高（实验性）| 暂不成熟 |

Thunder 的用量（最多 16 核 × 每秒几千次 offload）中，`moodycamel::ConcurrentQueue` 替换成本最低、收益最稳定。

---

### 问题④ 裸 `new std::threadpool`，从不 `delete`

```cpp
g_thunderWorkerPool = new std::threadpool(n);  // 进程退出由 OS 回收
```

进程退出时 OS 会回收，所以**不会造成资源泄漏**，但：
- ASan/Valgrind 会误报为泄漏，干扰 #85 的泄漏检测
- 单元测试中多次 `InitThunderWorkerThreadPool` 会因 `if != nullptr return` 而无法重置

**修法**：

```cpp
// 选项 A：static local（C++11 保证线程安全初始化）
std::threadpool& ThunderWorkerThreadPool() {
    static std::threadpool pool(std::max(2u, std::thread::hardware_concurrency() / 2));
    return pool;
}

// 选项 B：unique_ptr
static std::unique_ptr<std::threadpool> g_pool;
```

---

### 问题⑤ 无队列长度上限，高负载下无背压

```cpp
_tasks.emplace(...)；  // 无界队列，内存无上限增长
```

如果 offload 任务产生速度 > 线程消费速度（如 I/O 慢路径被大量触发），队列会无界增长，最终 OOM。

**修法**：加 `max_queue_size`，超限时 `commit` 返回 `std::nullopt` 或 `throw`，让协程走降级路径。

---

## 5. parallel_for / std::execution 适用性评估

| 机制 | 适用场景 | Thunder offload 场景匹配度 |
|------|---------|--------------------------|
| `std::for_each(par, ...)` | 对容器元素并行处理 | ❌ 每个 offload 是独立异步任务，不是批量元素 |
| `std::transform_reduce(par, ...)` | 并行归约 | ❌ 同上 |
| `std::execution::par_unseq` | SIMD + 多线程 | ❌ 不适用异步 offload |
| Thread pool + future | 任务并行 + 异步等待 | ✅ 当前场景 |
| C++23 `std::execution`（senders） | 可组合异步任务图 | ⚠️ 理论上最优，但编译器支持不成熟 |

**结论**：Thunder 的 offload 是**任务并行**（每请求提交一个独立工作单元），而非**数据并行**。`parallel_for` 类 API 不适用于此场景。当前使用线程池 + future 的方向正确，需要改进的是实现质量而非设计方向。

---

## 6. 改进路线（按优先级）

### P0（正确性）

**修 `namespace std` UB**

```diff
- namespace std {
-     class threadpool { ... };
- }
+ namespace util {
+     class threadpool { ... };
+ }
```

全局替换 `std::threadpool` → `util::threadpool`。

### P1（稳定性 + 可测试性）

**改为 static local，消除裸 new**

```cpp
std::threadpool& ThunderWorkerThreadPool() {
    static util::threadpool pool(
        std::max(2u, std::thread::hardware_concurrency() / 2));
    return pool;
}
void InitThunderWorkerThreadPool(unsigned short) {}  // 保留接口，变空操作
```

### P2（性能）

**换 lock-free 队列（若压测发现 commit 是瓶颈）**

引入 `moodycamel::ConcurrentQueue`（header-only），替换 `queue<Task> + mutex + condition_variable`。

### P3（可靠性）

**加队列上限 + 背压**

```cpp
template<class F, class... Args>
std::optional<future<...>> commit_bounded(F&& f, Args&&... args) {
    if (_tasks.size() >= _max_queue) return std::nullopt;  // 背压
    // ... 正常入队
}
```

---

## 7. 总结

| 维度 | 现状 | 建议 |
|------|------|------|
| **设计方向** | ✅ 正确（offload + 协程恢复） | 保持 |
| **安全防护** | ✅ `IsRegisteredStep` 防悬空 | 保持 |
| **UB** | ✅ `namespace util`，已修复 #92 | 已修复（2026-06-14）|
| **线程数** | ✅ 1 线程起步 + `resize(n)` 动态增减，已修复 #93 | 已修复（2026-06-14）|
| **内存管理** | ✅ `unique_ptr`，已修复 #95 | 已修复（2026-06-14）|
| **并发性能** | ✅ `moodycamel::ConcurrentQueue`，已修复 #94 | 已修复（2026-06-14），加速比 2.5x~3.7x |
| **背压** | ✅ `_queueSize`+`_maxQueueSize`，已修复 #96 | 已修复（2026-06-14），默认 = size × 64 |
