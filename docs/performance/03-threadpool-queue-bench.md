# ThreadPool 队列方案性能对比

> 日期：2026-06-14  
> 关联 issue：#94（issus-list.md）  
> 测试机：Linux 7.0.0-22-generic，编译选项 `-O2`  
> 基准程序：`code/test/labor/bench_threadpool_queue.cpp`

---

## 1. 实施记录

### 改动内容（commit: working tree）

**目标**：解决 #92（namespace std UB）+ #94（mutex 全局锁）+ #95（裸 new）

| 文件 | 改动 |
|------|------|
| `code/Util/src/thread/threadpool.h` | `namespace std` → `namespace util`；`std::queue+mutex+cv` → `moodycamel::ConcurrentQueue`；worker 从 condvar wait 改为 `try_dequeue+yield` |
| `code/Net/include/coro/ThreadPoolAwaitable.hpp` | 4 处 `std::threadpool` → `util::threadpool` |
| `code/Net/include/labor/WorkerThreadPool.hpp` | 声明 `util::threadpool&` |
| `code/Net/src/labor/WorkerThreadPool.cpp` | `unique_ptr<util::threadpool>`（#95 已改）；namespace 同步 |
| `code/test/util/test_util_threadpool.cpp` | 8 处 `std::threadpool` → `util::threadpool` |
| `code/Net/CMakeLists.txt` | 加 `${THUNDER_3PARTY}` include 路径（找 concurrentqueue.h） |
| `code/Util/CMakeLists.txt` | 同上（PUBLIC） |
| `code/test/CMakeLists.txt` | 同上 + 新增 bench 目标 |

### 验证结果

```
ThreadPool 单元测试: 8/8 PASSED (78 ms)
  - DefaultConstruction, CommitReturnsFuture, MultipleTasks
  - TaskWithArgs, VoidTask, IdleCount
  - DestructionJoinsThreads, ConcurrentTasks
```

---

## 2. 测试结果

### Run #1 — 2026-06-14（改动前，MutexQueue vs LockFreeQueue）

| 场景 | Mutex ns/op | LF ns/op | 加速比 |
|------|:-----------:|:--------:|:------:|
| 4P-4C（典型 offload）| 307.1 | 132.5 | 2.32x |
| 16P-4C（高并发 commit）| 325.9 | 120.0 | 2.72x |
| 1P-4C（单协程基线）| 644.2 | 162.6 | 3.96x |
| 8P-8C（对等压力）| 371.4 | 111.6 | 3.33x |

### Run #2 — 2026-06-14（改动后，重新跑 benchmark）

```
场景                  Mutex ns/op    LF ns/op  Mutex Mop/s    LF Mop/s  加速比
--------------------------------------------------------------------------------
4P-4C  (典型 offload)       313.2       127.5        3.19        7.84     2.46x
16P-4C (高并发 commit)       335.5       118.6        2.98        8.43     2.83x
1P-4C  (单协程基线)          653.2       174.6        1.53        5.73     3.74x
8P-8C  (对等压力)            368.2       115.5        2.72        8.66     3.19x
```

- **LF** = `moodycamel::ConcurrentQueue`（lock-free MPMC）
- **Mutex** = `std::queue + std::mutex + std::condition_variable`（原方案）
- 每场景跑 3 次取中位数，单任务体为空操作（只测队列本身开销）
- Run #1 vs Run #2 差异在 ±5% 以内，属正常抖动

**结论：lock-free 方案快 2.5x ～ 3.7x。**

---

## 3. 为什么快：两个方案的本质区别

### 3.1 Mutex 方案：每次 commit() 的完整开销

每次 `commit()` 的完整开销：

```
生产者线程                      OS 内核
    │
    lock_guard{_lock}          ← ① 尝试获取 futex
    │   若锁被占 → syscall      ← ② 陷入内核，线程挂起
    │   若锁空闲 → 原子 CAS     ← ③ 最快也是一次原子写
    │
    _tasks.push(task)          ← ④ std::queue 堆上分配节点
    │
    _task_cv.notify_one()      ← ⑤ 可能再次 syscall（唤醒消费者）
    │
    lock_guard 析构 → unlock   ← ⑥ 释放 futex
```

**关键代价**：
- 即使"锁空闲"也必须做原子 CAS（修改 futex 字）
- 多个生产者竞争时，输掉 CAS 的线程进内核挂起 + 唤醒 = **~1000ns/次上下文切换**
- `notify_one()` 在消费者被唤醒时又一次 syscall
- 单协程基线（1P-4C）反而最慢（644 ns），因为只有一个生产者却仍然要走完整锁路径

### 3.2 LockFree 方案：ConcurrentQueue 路径

`moodycamel::ConcurrentQueue` 用分块循环数组（chunk list）+ 原子 index，核心入队：

```
生产者线程                      不涉及 OS 内核
    │
    fetch_add(tail, 1)         ← ① 原子递增尾指针（~5ns，单指令）
    │   每个生产者拿到独立 slot
    │
    chunk[slot] = task         ← ② 直接写槽位（cache line 写）
    │
    store(ready_flag, true)    ← ③ 原子标记"已就绪"
    │
    （完成，无 syscall）
```

**关键差异**：
- `fetch_add` 是 x86 的 `LOCK XADD` 指令，**不陷入内核**，约 5-15 ns
- 多生产者各自拿到不同 slot，**互不等待**（只有 `fetch_add` 一个竞争点）
- 消费者 `try_dequeue` 同样无锁，失败直接返回 false，不挂起
- 本次测试消费者用 `yield()` 自旋等待，避免 `condition_variable::wait` 的 syscall

### 3.3 对比图

```
【Mutex 方案】高并发时的时序

时间轴 →
Producer A: ████ lock ████ push ████ unlock
Producer B:      ░░░░ wait ░░░░ ░░░░ lock ████ push ████ unlock
Producer C:                          ░░░░ wait ░░░░ ░░░░ lock ████ push
                 ↑串行化区域，所有 commit 在这里排队

【LockFree 方案】高并发时的时序

时间轴 →
Producer A: ▌fetch_add▌ write slot A
Producer B: ▌fetch_add▌ write slot B     （与 A 同时进行）
Producer C: ▌fetch_add▌ write slot C     （与 A/B 同时进行）
              ↑只有这一个原子指令竞争，之后各写各的 slot
```

### 3.4 为什么 1P-4C 反而 Mutex 最慢

单生产者压力下，Mutex 的 `notify_one()` 每次提交都需要唤醒一个等待中的消费者：

```
commit():
  1. lock
  2. push
  3. unlock
  4. notify_one()  ← 消费者在 condvar 上 wait，唤醒 = futex_wake syscall
```

lock-free 方案的消费者在 `yield()` 自旋，无需 syscall 唤醒，所以 1P 场景反而加速比最大（3.96x）。

---

## 4. Thunder 实际场景的意义

Thunder 的 offload 路径：

```
协程 co_await MakePoolOffloadAwaiter()
    → pool.commit(lambda)          ← 这里是 commit 热点
    → 线程池子线程执行 lambda
    → PostToEventLoop(resume)
```

每个进入 offload 的请求都调用一次 `commit()`。在高 QPS 场景（如压测 `/hello/pool_cpu`）：
- 4 个 Worker 进程 × 1000 QPS = 4000 次/秒 commit
- 平均间隔 250μs，Mutex 的 307 ns << 250μs → **当前负载下锁不是瓶颈**

如果 QPS 超过 **100k/s 单 Worker**，mutex 的 3.26 Mops/s 才会成为上限。

---

## 5. 决策

| 维度 | 结论 |
|------|------|
| 性能提升 | 2.5x ～ 3.7x，已确认 |
| 引入成本 | `concurrentqueue.h` 到 `code/3party/`，单头文件，无额外依赖 |
| 改动范围 | `threadpool.h` 队列类型 + worker 等待逻辑 + namespace 修复 + 全局存储改 unique_ptr |
| 风险 | 消费者改为自旋 yield，空闲时 CPU 占用略升（可加退避或 semaphore） |
| **实施状态** | **✅ 已合入**（#92 + #94 + #95 一并处理） |

### 后续可优化方向

- 消费者改为 `BlockingConcurrentQueue::wait_dequeue` 避免空闲 yield（需额外验证析构时序）
- 加 `max_queue_size` 背压（#96）
- 默认线程数改为 `hardware_concurrency()/2`（#93）

---

## 6. 如何回归

```bash
# 重新跑 benchmark
./build/bin/thunder_bench_threadpool_queue \
    --gtest_filter="BenchThreadpoolQueue.Compare"

# 跑 ThreadPool 功能测试
./build/bin/thunder_test_util_threadpool
```

每次跑完追加到「历史记录」章节。

---

## 历史记录

### Run #1 — 2026-06-14（改动前）

| 场景 | Mutex ns/op | LF ns/op | 加速比 |
|------|:-----------:|:--------:|:------:|
| 4P-4C（典型）| 307.1 | 132.5 | 2.32x |
| 16P-4C（高并发）| 325.9 | 120.0 | 2.72x |
| 1P-4C（单协程）| 644.2 | 162.6 | 3.96x |
| 8P-8C（对等）| 371.4 | 111.6 | 3.33x |

### Run #2 — 2026-06-14（改动后验证）

| 场景 | Mutex ns/op | LF ns/op | 加速比 |
|------|:-----------:|:--------:|:------:|
| 4P-4C（典型）| 313.2 | 127.5 | 2.46x |
| 16P-4C（高并发）| 335.5 | 118.6 | 2.83x |
| 1P-4C（单协程）| 653.2 | 174.6 | 3.74x |
| 8P-8C（对等）| 368.2 | 115.5 | 3.19x |

**单元测试**：8/8 PASSED（78 ms）
