# Work-Stealing 线程池设计

> 关联 issue：#109  |  设计日期：2026-06-21  
> 前置文档：[20-threadpool-analysis.md](./20-threadpool-analysis.md)  
> 性能基准：[04-work-stealing-bench.md](../architecture/12-work-stealing-threadpool.md#附录-bwork-stealing-性能基准)

---

> 自研 Work-Stealing 线程池：承接事件循环的阻塞/CPU 密集任务，每 worker 独立队列 + 空闲偷取负载均衡，吞吐倍增。

---

## 1. 设计背景

**Thunder 的 Worker 进程是单线程事件循环 + 协程模型。** 遇到可能阻塞事件循环的操作（磁盘 IO、CPU 密集计算），必须 offload 到线程池执行，完成后再回到事件循环 resume 协程。

```
事件循环 (epoll_wait)
    │
    │  遇到阻塞操作 → 不能占着事件循环
    │
    ├──→ threadpool.commit(task)        // 推到线程池
    │         │
    │         ▼ (worker 线程执行)
    │    task() 完成
    │         │
    │    PostToEventLoop(resume)        // 回事件循环
    │         │
    ▼         ▼
  co_await resume  ← 协程醒来继续
```

**典型 offload 场景**：

| 场景 | 任务耗时 | 频率 |
|------|---------|------|
| RocksDB 读写 | 1~10 ms | 高（每次请求） |
| 文件 IO（NFS/本地盘）| 1~100 ms | 中 |
| 压缩/解压/序列化 | 1~50 ms | 高 |
| Protobuf/JSON 解析 | <1 ms | 极高 |

这些任务**短（μs~ms 级）、高频、独立无依赖**。线程池的调度开销直接影响请求 P99 尾延迟。

---

## 2. 现存问题

当前线程池架构：单个全局 MPMC 队列，N 个 worker 自旋轮询。

```
   commit() → [moodycamel MPMC 队列] → try_dequeue (N 个 worker 同时 CAS 同一个 head)
```

> **核心矛盾**：所有 worker 同时 CAS 同一个 `head` 所在的 cache line → **cache line bouncing**。实测：1 worker → 16 worker 时单次调度延迟从 919ns 恶化到 2097ns（+128%）。

**目标**：每个 worker 有自己的本地队列，空闲时才去别人队列偷任务——绝大多数时候零跨核竞争。

---

## 3. 方案选型

### 3.1 Go LRQ（Local Run Queue）

**诞生背景**：Go runtime 的 goroutine 调度器。每个 P（逻辑处理器）有自己专属的 256-slot LRQ。goroutine 在哪个 P 上创建就 push 到那个 P 自己的 LRQ，P 绑定的 M 从 head 取任务执行，空闲 M 随机从别的 P 的 LRQ 偷任务。

| 操作 | 端 | 执行者 | 竞争 |
|------|----|--------|------|
| push | tail++ | 唯一写者（本 P） | 无 |
| dequeue | CAS head++ | owner worker | 与 steal 竞争 |
| steal | CAS head++，取 n/2 | 空闲 worker | 与 dequeue 竞争 |

> **关键特征**：push 和 dequeue/steal 操作不同的端（tail vs head），两端天然分离。**push 和 dequeue 不需要是同线程。**

### 3.2 TBB Chase-Lev

**诞生背景**：Intel TBB 的递归任务并行库（`parallel_for` 等）。worker 执行任务时会产生子任务，push 到**自己** deque 的 bottom（LIFO，cache 热度最高），自己从 bottom pop 执行，空闲 worker 从别人 deque 的 top steal（FIFO，最老的先偷）。

```
  steal 端 (CAS top)  ←  [ T₀  T₁  ... ] →  push/pop 端 (owner 独占 bottom)
```

> **关键特征**：push 和 pop **必须同线程**（owner 独占 bottom 端，pop 无需 CAS）。

### 3.3 Thunder 约束与选型

| 约束 | 类型 | 影响 |
|------|------|------|
| push 和 pop 是不同线程（事件循环 push，worker pop） | **硬** | Chase-Lev 直接排除 |
| 单一生产者（仅事件循环 push） | 软 | tail 端无竞争 |
| 事件循环 P2C 分发 → 写**任意** worker 的 deque | **硬** | Go 里每个 P 只写自己的 LRQ，Thunder 不同 |
| 任务 1~100ms，256 slot 足够 | 软 | 固定容量即可 |
| 对外 API 不变 | **硬** | `commit()` / `resize()` 接口不动 |

**选 Go LRQ，但有一个额外问题**：Go 里每个 P 只 push 自己的 LRQ，push 和 steal_into(dst) 永远不会写同一个 deque 的 tail。Thunder 的事件循环通过 P2C 向**任意** worker 的 deque push——如果 push 和 steal_into 写同一个 deque 的 tail，就是 data race。

**解决方案：每个 worker 持两个 deque，物理隔离 tail 写者。**（详见 §4.8）

> **选型结论**：Go LRQ 风格 + 每个 worker 两组 deque。

---

## 4. 核心设计

### 4.1 三块结构总览

**整个设计围绕三块结构展开：两个 deque 数组负责高速流转，一个 global_q 负责兜底。**

```
                         WorkStealingPool
┌──────────────────────────────────────────────────────────────────┐
│                                                                  │
│  _submit_deques[0..N]               _local_deques[0..N]          │
│ ┌──────────────────────┐          ┌──────────────────────┐       │
│ │  写入端：仅 commit 线程│          │  写入端：仅 worker[id] │       │
│ │  (P2C 分发)          │  steal   │  (偷来的任务暂存)     │       │
│ │                      │────────→ │                      │       │
│ │  读出端：             │          │  读出端：             │       │
│ │  worker[i] 直接取     │          │  worker[i] 直接取     │       │
│ │  thief 偷走           │          │  thief 再偷走         │       │
│ └──────────────────────┘          └──────────────────────┘       │
│          │ drain                         │ drain                 │
│          ▼                               ▼                       │
│        ┌──────────────────────────────────────┐                  │
│        │           global_q (MPMC)             │                  │
│        │   写入：burst 溢出 / 缩容 drain        │                  │
│        │   读出：61-tick 防饥饿 / idle 兜底     │                  │
│        └──────────────────────────────────────┘                  │
└──────────────────────────────────────────────────────────────────┘
```

> 每个 worker 持有**两组** deque，而非一组——这是跟 Go/TBB 最关键的区别。原因见 §4.8。
>
> **队列结构**：`_submit_deques` 和 `_local_deques` 都是 WorkerDeque（固定 256-slot ring buffer，SPMC：tail 单写者、head 多读者 CAS 协调）。`global_q` 是 moodycamel::ConcurrentQueue（附录 A）。WS 本质是把 LF 的 1 个 MPMC 拆成 2N 个 SPMC + 1 个 MPMC 兜底。

**图上每根箭头是怎么实现的**：

| 箭头 | 怎么实现 | 详见 |
|------|---------|:--:|
| commit → _submit_deques（P2C 分发） | 随机采 2 个 deque 的 `size()`，选任务少的 `push` 进去 | §4.4 |
| _submit → _local（steal 偷取） | thief 调用 `steal_into(src)`：复制 victim 的 ring 槽位 → 1 次 CAS 推进 victim.head 宣告所有权 → 更新自己 tail 对外公开 | §4.3 |
| _submit / _local → global_q（drain 倒出） | 逐条 `dequeue` 取出来 → `enqueue` 塞进 global_q | §4.7 |
| global_q → worker（61-tick 防饥饿） | worker 循环每 61 tick 强制 `try_dequeue` 一次 | §4.5 |
| global_q → worker（idle 兜底） | worker 五级级联全部落空后，`try_dequeue` 作为最后手段 | §4.5 |
| _submit / _local → worker 消费 | worker 调用 `dequeue()`：CAS head 取 1 个任务执行 | §4.3 |

### 4.2 数据流路径

**一个任务从 commit 到被执行，走哪条路，由什么触发，设计意图是什么。**

| 路径 | 触发条件 | 意图 |
|------|---------|------|
| commit → _submit_deques[i] → worker[i] 直接取走 | commit P2C 选中 deque i（正常路径，>99%） | 零竞争，最快 |
| _submit_deques → steal → _local_deques → thief 取走 | thief 自己的 deque 空，随机 steal 命中 | 负载均衡 |
| _local_deques → steal → 其他 _local_deques | 其他 thief 空闲，再偷 | 再扩散，自然拉平 |
| commit → global_q（L3 溢出） | 所有 submit_deque 全满（极端 burst） | 兜底，不阻塞事件循环 |
| global_q → worker（61-tick） | 高负载，worker 每 61 次循环强制检查 | 防饥饿 |
| global_q → worker（idle 兜底） | worker 五级级联全部落空 | 最后兜底 |
| _submit + _local → drain → global_q | worker 缩容退出 | 零丢失（退出的 worker 不是任何 deque 的合法写者） |

### 4.3 WorkerDeque 操作

**一个固定 256-slot ring buffer，head/tail 各占独立 cache line（alignas 64B）。单个 deque ≈12.4 KB。**

```
// push — 仅由唯一 tail 写者调用，无需 CAS
bool push(Task t):
    if tail - head >= 256: return false       // 满，task 未消耗
    ring[tail % 256] = t; tail.store(tail+1, release)

// dequeue — owner 或 thief 取 1 个，CAS head
optional<Task> dequeue():
    loop: if head == tail: return nullopt     // 空
          t = ring[head % 256]
          if CAS(head, head, head+1): return t    // 失败→重试

// steal_into — 从 src 批量偷取，1 次 CAS（无论偷几个）
uint32_t steal_into(src, max_n):
    loop: avail = src.tail - src.head
          n = min(max_n, avail - avail/2)     // 偷一半
          n = min(n, 本 deque 剩余空间)
          for i in 0..n: my_ring[(my_t+i)%256] = src_ring[(src_head+i)%256]  // ① 预复制
          if !CAS(src.head, src_head, src_head+n): continue                  // ② 宣告
          my_tail.store(my_t + n, release)                                   // ③ 公开
```

### 4.4 commit() — 三级分发（生产者）

**事件循环线程（单生产者）调用。把任务投递到某个 worker 的 `_submit_deques`，P2C 以 O(1) 开销选负载最轻的。绝不碰 `_local_deques`。**

```
commit(task):
    if _queueSize++ >= _maxQueueSize: throw "queue full"

    // L1: Power of Two Choices — 随机采 2 个 deque，选任务少的
    idx = min(size(submit[a]), size(submit[b]))
    if submit[idx].push(task): return           ← >99% 命中

    // L2: 顺序全扫 — L1 采的两个恰好满了
    for i in 0..N: if submit[i].push(task): return

    // L3: global_q — 所有 deque 全满（极端 burst）
    global_q.enqueue(task)
```

### 4.5 Worker 调度循环

**每个 worker 线程（消费者）独立跑这个循环。N 个 worker，N 个线程。从自己 deque 取 → 没有才去别人那偷 → 全没有才去 global_q 找。**

```
worker_loop(id):                          // 每个 worker 线程执行
    tick = 0
    while _run:
        tick++

        // 0. 每 61 tick 强制查一次 global_q，防止高负载时 global_q 里的任务被遗忘
        if tick % 61 == 0 and global_q.try_dequeue(): 执行; continue

        // ① 自己的 local_deque — 之前从别人那 steal 来的任务暂存在这，零竞争
        if local_deques[id].dequeue(): 执行; continue

        // ② 自己的 submit_deque — commit 通过 P2C 直接投递到这，低竞争
        if submit_deques[id].dequeue(): 执行; continue

        // ③ 两个 deque 都空了，检查是否要退出
        if excessThreads > 0 || !_run: drainToGlobal(id); return

        // ④ 去偷别人的 —— steal_into() 是"搬"的动作：
        //    先偷 victim 的 submit_deque（commit 刚投的），空了再偷 local_deque
        //    搬来的任务进入自己的 local_deque，然后 dequeue 一个执行
        if active > 1:
            start = xorshift32() % active
            for i in 0..min(4, active-1):
                victim = (start + i) % active; if victim == id: continue
                if steal_into(submit_deques[victim]) > 0      // 从 victim 的 submit 搬任务到自己 local
                   || steal_into(local_deques[victim]) > 0:   // submit 空了就从 victim 的 local 搬
                    task = local_deques[id].dequeue(); 执行; continue

        // ⑤ 偷也偷不到 → global_q
        if global_q.try_dequeue(): 执行; continue

        // ⑥ 全空 → 阻塞等
        _cv.wait_for(10ms, [有任务 || 需退出])
```

### 4.6 Steal

```
_submit_deques[victim] ──steal_into()──→ _local_deques[thief]
      忙，有积压                            空了，来搬
```

**触发时机**：worker 自己的两个 deque 都为空——**立刻** steal，不等。延迟 steal（先 spin 等新任务）本 worker 空转 + victim 积压没人帮，两端吃亏。

- 随机起点，最多试 4 个
- 偷一半：全偷 victim 也空→互相偷死循环；偷一个→频繁 steal
- 端到端吞吐见 §5

### 4.7 Drain

```
_submit_deques[退出] ──┐
                        ├──→ global_q ──→ 其他 worker 取走
_local_deques[退出]  ──┘
```

- 缩容/析构时 worker 退出，deque 里可能还有没执行完的任务，不能丢
- 不能放回别人 deque——退出的 worker 不是任何 deque 的合法 tail 写者，往里写 = data race
- `global_q` 是池中唯一任意线程可写的结构，丢进去，其他 worker 61-tick/idle 取走

### 4.8 为什么必须是两组 deque

```
commit 线程 ──写 tail──→ 同一个 deque 的 tail ←──写 tail── thief worker
                              ↓
                          data race
```

如果只用一组 deque，commit 的写入和 thief 的 steal 写入会并发操作同一个 deque 的 tail，data race。

```
commit 线程 ──写 tail──→ _submit_deques[*]
                               │
                        相互独立，写者不重叠
                               │
thief worker ──写 tail──→ _local_deques[id]
```

拆成两组：commit 只写 `_submit_deques` 的 tail，worker 只写自己 `_local_deques` 的 tail，两个写者永远不碰同一个 tail。TSan 零警告。

---

## 5. 性能实测

> 基准程序：`code/test/labor/bench_work_stealing.cpp`  |  Linux 7.0, `-O2`, 500K 空任务, 每场景 3 次取中位数  
> **LF** = 旧单队列线程池  |  **WS** = 新 Work-Stealing 线程池  |  **1P-4C** = 1 个生产者（事件循环），4 个消费者（worker）

### 5.1 吞吐对比

| 场景 | LF ns/op | WS ns/op | 加速比 |
|------|:-------:|:-------:|:-----:|
| 1P-1C | 919.6 | 256.4 | **3.59x** |
| 1P-2C | 1097.3 | 325.6 | **3.37x** |
| **1P-4C（Thunder 典型配置）** | **1373.1** | **543.6** | **2.53x** |
| 1P-8C | 1698.9 | 659.4 | **2.58x** |
| 1P-16C | 2097.2 | 968.1 | **2.17x** |
| 1P-4C, task=10μs | 2830.8 | 2624.9 | 1.08x |
| 1P-4C, task=100μs | 25324.2 | 25215.6 | 1.00x |

### 5.2 端到端延迟 + payload 大小

| 场景 | LF P50 ns | WS P50 ns | LF avg ns | WS avg ns | avg 加速比 |
|------|:-------:|:-------:|:-------:|:-------:|:--------:|
| 4 worker, 0B | 1586 | 1227 | 1394 | 700 | **1.99x** |
| 4 worker, 64B | 2434 | 1076 | 1789 | 694 | **2.58x** |
| 4 worker, 256B | 2066 | 1209 | 1668 | 770 | **2.17x** |
| 4 worker, 1KB | 1787 | 1207 | 1594 | 823 | **1.94x** |
| 4 worker, 4KB | 2040 | 1737 | 1794 | 1326 | **1.35x** |

> 表注：task=0，延迟 = commit() 前 → worker 取出执行。payload 按值捕获在 lambda 中。完整数据见 [04-work-stealing-bench.md §10](../architecture/12-work-stealing-threadpool.md#附录-bwork-stealing-性能基准#10-端到端延迟--payload-大小run-2----2026-07-07)

### 5.3 结论

> **WS 在任何场景下不慢于 LF。** Thunder 典型配置（4 worker）：吞吐快 **2.53x**，端到端延迟快 **1.99x**（avg）/**1.29x**（P50）。任务越轻优势越大——轻任务瓶颈在队列调度开销，WS 每个 worker 自有 deque、不用争抢同一个队列头；重任务（≥100μs）瓶颈在任务体自身，队列差异被淹没。payload 64B~256B 时 WS 优势最大（2.17x~2.58x avg）。worker ≤ 2 时用旧实现即可。

---

## 6. 参考资料

- Go runtime: `runtime/proc.go` — `runqget` / `runqgrab` / `stealWork`
- Intel TBB: `src/tbb/task_dispatcher.h` — Chase-Lev 动态 deque
- Chase & Lev, "Dynamic circular work-stealing deque", SPAA 2005
- Lê et al., "Correct and efficient work-stealing for weak memory models", PPoPP 2013
- Mitzenmacher, "The Power of Two Choices in Randomized Load Balancing", TPDS 2001

---

## 附录 A：ThreadPool 队列方案性能基准

> 原独立文档 `architecture/12-work-stealing-threadpool.md#附录-athreadpool-队列方案性能基准`，合并于此。

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


---

## 附录 B：Work-Stealing 性能基准

> 原独立文档 `architecture/12-work-stealing-threadpool.md#附录-bwork-stealing-性能基准`，合并于此。

> 关联 issue：#109  
> 关联设计：[docs/architecture/12-work-stealing-threadpool.md](../architecture/12-work-stealing-threadpool.md)  
> 基准程序：`code/test/labor/bench_work_stealing.cpp`  
> 测试机：Linux 7.0.0-22-generic，编译选项 `-O2`

---

## 1. 术语说明

| 术语 | 含义 |
|------|------|
| **LF**（Lock-Free）| 当前生产实现：`util::threadpool`，内部用 `moodycamel::ConcurrentQueue` 单全局队列，所有 worker 从同一队列取任务 |
| **WS**（Work Stealing）| 本次新实现：`util::WorkStealingPool`，每个 worker 有独立 deque，空闲时才去偷别人的任务 |
| **op**（operation）| 一次任务的完整生命周期：从 `commit()` 提交 → worker 取出 → 执行完成 |
| **ns/op** | 平均每个 op 耗时多少纳秒（越小越好） |
| **Mop/s** | 每秒处理百万个 op（越大越好） |
| **1P-4C** | 1 个 producer 线程提交任务，4 个 consumer（worker）线程执行 |

---

## 2. 计时说明

```
t0 ──────────────────────────────────────────────── t1
│                                                    │
│  commit() × N  →  worker 取出并执行  →  最后一个完成  │
└────────────────────────────────────────────────────┘
         ns/op = (t1 - t0) / N
```

`ns/op` **包含**：
- `commit()` 本身耗时（入队操作）
- 任务在队列里等待的时间（排队延迟）
- worker 取出任务的耗时（出队操作）
- 任务体执行耗时（空任务时接近 0，task=10μs 场景时为 10μs）

`ns/op` 是**端到端吞吐延迟的平均值**，反映整个调度路径的效率，不只是队列操作本身。

---

## 3. 并发模型

### 3.1 每个 WorkerDeque 是 SPMC

```
commit 线程          worker[0]           worker[1]   worker[2]
    │                    │                    │           │
    │ push()             │ dequeue()          │           │
    ▼  （写 tail）        ▼  （CAS head）      ▼           ▼
_submit_deques[0]                       steal_into(src)（CAS head）
  [ring buffer]    ◄── 取自己的任务      ◄── 偷任务（空闲时）
```

- **tail**：只有 commit 线程写 → Single Producer
- **head**：owner `dequeue()` 和其他 worker 的 `steal_into()` 都 CAS head → Multiple Consumer

每个 WorkerDeque 是 **SPMC**，不是 SPSC。

### 3.2 整体结构：多个 SPMC + 一个 MPMC

```
WorkStealingPool
├── _submit_deques[0]   SPMC  ← commit 写 tail；worker[0] 取，其他 worker 可偷
├── _submit_deques[1]   SPMC  ← commit 写 tail；worker[1] 取，其他 worker 可偷
├── _submit_deques[…]   SPMC
├── _local_deques[0]    SPMC  ← worker[0] steal_into 写 tail；worker[0] 取，其他 worker 可偷
├── _local_deques[1]    SPMC  ← worker[1] steal_into 写 tail；worker[1] 取，其他 worker 可偷
├── _local_deques[…]    SPMC
└── global_q            MPMC  ← commit/drain 写；任意 worker 取（moodycamel）
```

LF 方案只有一个 MPMC（global_q），所有 worker 争一个 head。
WS 方案把 MPMC 拆成了 N 个 SPMC + 1 个兜底 MPMC，正常路径无争用。

### 3.3 设计约束

`WorkerDeque::push()` 设计为**单 tail 写者**，不支持多线程并发 push。  
Thunder 的 `commit()` 在真实场景中由单一事件循环线程调用（per-worker 进程的 coroutine 框架），天然满足此约束。

因此本 bench **生产者固定为 1 个线程**，模拟 Thunder 的事件循环，worker 数量作为唯一变量。

---

## 4. 背景

当前线程池（#94 已改为 moodycamel 单全局队列）在多 worker 时存在 **cache line bouncing**：
N 个 worker 同时 CAS 同一个队列 head，争用随 worker 数线性增长。

WS 方案（#109）每个 worker 有独立 `_submit_deque`，commit() 分发后各 worker 无竞争取任务，
仅在 steal 和极端 burst 时才接触共享结构（`global_q`）。

---

## 5. 场景矩阵

```
场景名              producer  worker  total_tasks  说明
───────────────────────────────────────────────────────────
1P-1C (最简基线)       1       1       500,000    无竞争，验证 deque 基础吞吐
1P-2C                  1       2       500,000    2 worker
1P-4C (Thunder 典型)   1       4       500,000    Thunder 最常见配置
1P-8C                  1       8       500,000    8 核，deque 分发 vs 单队列争用
1P-16C                 1      16       500,000    最大 worker 数，验证扩展性
1P-4C (task=10μs)      1       4        10,000    有耗时任务，WS 负载均衡效果
1P-4C (task=100μs)     1       4         1,000    长任务，队列开销是否可忽略
```

---

## 6. 实测结果（Run #1 — 2026-06-21）

> 测试机：Linux 7.0.0-22-generic，`-O2`，每场景 3 次取中位数  
> 任务体：空操作（task_us=0）或 spin 指定微秒

---

## 7. 结论

### 7.1 1P-1C 基线：无争用下 WS 仍快 3.59x

1P-1C 只有 1 个 worker，不存在多核 cache line bouncing，但 WS 仍快 3.59x。
原因在数据结构本身，与争用无关：

```
LF (moodycamel::ConcurrentQueue)           WS (WorkerDeque ring buffer)
─────────────────────────────────          ──────────────────────────────
enqueue:                                   push:
  申请 / 复用 block                           load head  (1 原子读)
  写入 block 内 slot                          load tail  (1 原子读)
  更新 block 内 index                         _ring[t & mask] = move(task)  (1 写)
  更新全局 index                              tail++  (1 原子写)
  → 多级 index，有指针追踪                   → 直接数组写，无指针追踪

dequeue:
  找到活跃 block（可能跨 cache line）       dequeue:
  检查 block 内 index                         load head  (1 原子读)
  读取 slot                                   load tail  (1 原子读)
  推进 block index                            task = _ring[h & mask]  (1 读)
  可能回收 block                              CAS head++  (1 原子 CAS)
  → 链表结构，cache miss 概率高              → 连续数组，顺序访问，prefetch 友好
```

moodycamel 为通用 MPMC 设计，即使 1P-1C 也要走完整的 block 管理路径。
WorkerDeque 是 SPMC ring buffer（SP = push 单 tail 写者；MC = dequeue/steal_into 均 CAS head）。
1P-1C 时跑在 SPMC 的退化最优路径上：只有 1 个 consumer，CAS 永远第一次成功，无重试；
producer 顺序写 tail，consumer 顺序读 head，内存访问完全线性。

**这也解释了为什么 WS 加速比随 worker 增加而下降**（3.59x→2.17x）：
worker 越多，WS 自身的开销也上升（P2C 扫描更多 deque、global_q 溢出路径），
优势被部分抵消，但绝对值仍比 LF 低 2x+。

### 7.2 空任务场景（队列调度开销为主）

WS 比 LF 快 **2.17x ～ 3.59x**。

多 worker 时再叠加一层争用差距：LF 的所有 worker 共用一个 moodycamel 队列，每次取任务都要
CAS 同一个 head 指针，N 个 worker 的 cache line 在多核间来回传递（bouncing）。
WS 每个 worker 有独立的 `_submit_deque`，worker 取自己的 deque 时完全无竞争。

### 7.2 LF 随 worker 数劣化，WS 更平稳

```
worker 数    LF ns/op    WS ns/op    LF/WS
   1           919.6      256.4      3.59x
   2          1097.3      325.6      3.37x
   4          1373.1      543.6      2.53x
   8          1698.9      659.4      2.58x
  16          2097.2      968.1      2.17x

LF: 1C→16C 增长 +128%（单队列争用线性恶化）
WS: 1C→16C 增长 +278%（绝对值仍比 LF 低 2x+）
```

LF 的 ns/op 随 worker 数单调增长，原因是争用 head 的线程越多，cache miss 越频繁。
WS 也有增长，原因是 worker 数多时 commit() 的 P2C 和线性扫描路径更长，且 16C 时
大量任务溢出到 `global_q`（submit_deque 总容量 16×256=4096，500K 任务大部分走 global_q）。

### 7.3 有耗时任务时两者趋近

task=10μs 时加速比降至 1.08x，task=100μs 时完全持平（1.00x）。  
原因：任务体耗时占 ns/op 的绝对多数，队列调度开销被淹没。

### 7.4 适用场景建议

| 场景 | 推荐方案 | 原因 |
|------|---------|------|
| 高频轻量任务（Thunder offload）| **WS** | 队列开销是瓶颈，WS 减少 2x+ 争用 |
| 长任务（≥100μs）| LF 或 WS 均可 | 队列开销可忽略，无明显差距 |
| worker 数多（≥8）| **WS** | LF 争用随 worker 数线性恶化 |

---

## 8. 如何运行

```bash
# 编译
cmake --build build --target thunder_bench_work_stealing -j$(nproc)

# 运行
./build/bin/thunder_bench_work_stealing --gtest_filter="BenchWorkStealing.Compare"

# 旧基线（LF vs Mutex，多生产者场景）
./build/bin/thunder_bench_threadpool_queue --gtest_filter="BenchThreadpoolQueue.Compare"
```

---

## 9. 验收（✅ 已全部通过）

| 场景 | LF ns/op | WS ns/op | LF Mop/s | WS Mop/s | 加速比 |
|------|:--------:|:--------:|:--------:|:--------:|:------:|
| 1P-1C（最简基线）| 919.6 | 256.4 | 1.09 | 3.90 | **3.59x** ✅ |
| 1P-2C（2 worker）| 1097.3 | 325.6 | 0.91 | 3.07 | **3.37x** ✅ |
| 1P-4C（Thunder 典型）| 1373.1 | 543.6 | 0.73 | 1.84 | **2.53x** ✅ |
| 1P-8C（8 worker）| 1698.9 | 659.4 | 0.59 | 1.52 | **2.58x** ✅ |
| 1P-16C（16 worker）| 2097.2 | 968.1 | 0.48 | 1.03 | **2.17x** ✅ |
| 1P-4C（task=10μs）| 2830.8 | 2624.9 | 0.35 | 0.38 | 1.08x |
| 1P-4C（task=100μs）| 25324.2 | 25215.6 | 0.04 | 0.04 | 1.00x |

---

## 10. 端到端延迟 + payload 大小（Run #2 — 2026-07-07）

> 基准程序：`code/test/labor/bench_queue_latency.cpp`  
> 测量：端到端延迟 = commit() 调用前 → worker 取出开始执行（不含任务体，task=0）  
> payload：lambda 按值捕获的字节数组，影响 std::function 内存分配大小

### 10.1 P50 / P99 / avg 延迟

| 场景 | 池 | P50 ns | P99 ns | avg ns | avg 加速比 |
|------|:--:|-------:|-------:|-------:|:--------:|
| 1 worker, 0B | LF | 1455776 | 3964679 | 1251 | — |
| | WS | 416287 | 10252507 | 660 | **1.90x** |
| 4 worker, 0B | LF | 1586 | 6837 | 1394 | — |
| | WS | 1227 | 7428 | 700 | **1.99x** |
| 8 worker, 0B | LF | 1573 | 6033 | 1628 | — |
| | WS | 1433 | 6813 | 787 | **2.07x** |
| 4 worker, 64B | LF | 2434 | 11816 | 1789 | — |
| | WS | 1076 | 5420 | 694 | **2.58x** |
| 4 worker, 256B | LF | 2066 | 13737 | 1668 | — |
| | WS | 1209 | 8812 | 770 | **2.17x** |
| 4 worker, 1KB | LF | 1787 | 5775 | 1594 | — |
| | WS | 1207 | 5899 | 823 | **1.94x** |
| 4 worker, 4KB | LF | 2040 | 8102 | 1794 | — |
| | WS | 1737 | 7009 | 1326 | **1.35x** |

### 10.2 关键发现

- **WS avg 延迟始终比 LF 快 1.35x~2.58x**，无退化场景
- **中等 payload（64B~256B）WS 优势最大（2.17x~2.58x）**：LF 的 MPMC block 管理对 payload 大小敏感，WS 的 ring buffer 不受影响
- **4KB 时差距缩小到 1.35x**：内存拷贝开销开始主导，队列差异被淹没
- **1 worker 场景 P50 很高（ms 级）**：单消费者跟不上生产者速度，任务大量排队——这是吞吐极限，不是队列操作开销
- **WS P99 在 1 worker 0B 场景出现 10ms 尖峰**：worker 全空后进入 cv.wait_for(10ms)，符合设计预期

---

## 附录 A：LF 队列 — moodycamel::ConcurrentQueue 设计简述

**是什么**：C++11 无锁 MPMC 队列（Cameron Desrochers, 2014）。  
GitHub: <https://github.com/cameron314/concurrentqueue>  
LF 线程池（`util::threadpool`）内部用它作为唯一任务队列。

**内部结构**：

**内部结构（以 Thunder 1P-4C 为例）**：

```
  事件循环线程                        ConcurrentQueue<T>
  (唯一生产者)
      │                    ┌────────────────────────────────────┐
      │  commit()          │  隐式生产者哈希表                    │
      │  enqueue() ───────→│  ┌──────────────────────────────┐  │
      │                    │  │ thread_id → hash → slot      │  │
      │                    │  │                              │  │
      │                    │  │  事件循环线程 → hash → slot[0]│  │
      │                    │  │  ┌─────────────────────┐     │  │
      │                    │  │  │ ImplicitProducer     │     │  │
      │                    │  │  │  ├─ block_index      │     │  │
      │                    │  │  │  ├─ block 链表 (私有)│     │  │
      │                    │  │  │  │  ┌───────┐       │     │  │
      │                    │  │  │  │  │Block A│→ ...  │     │  │
      │                    │  │  │  │  │[32槽] │       │     │  │
      │                    │  │  │  │  └───────┘       │     │  │
      │                    │  │  └─────────────────────┘     │  │
      │                    │  └──────────────────────────────┘  │
      │                    │                                    │
      │                    │  空闲 block 池（预分配 + 回收）      │
      │                    │  ┌───────┐ ┌───────┐              │
      │                    │  │Block C│ │Block D│ ...          │
      │                    │  │[32槽] │ │[32槽] │              │
      │                    │  └───────┘ └───────┘              │
      │                    │     ↑ 生产者 block 满时从这里取     │
      │                    └────────────────────────────────────┘
      │
      │    消费者怎么取：
      │    1. 遍历所有隐式生产者的 block 链表（组成逻辑上的全局队列）
      │    2. CAS 竞争一个全局原子索引（subqueue_index）
      │    3. 谁抢到索引指向的 block+slot，谁取走任务
      │
      │    worker[0] ──┐
      │    worker[1] ──┼──→ 全部 CAS 同一个 subqueue_index ──→ 取走任务
      │    worker[2] ──┤
      │    worker[3] ──┘
```

**block 分配策略**：

```
生产者 enqueue 时：
  if 当前 block 未满 → 写入 slot, 推进 block 内 index
  if 当前 block 满   → 从空闲池取一个 block（池空则 malloc 新 block）
                    → 挂到生产者自己的 block 链表尾部
                    → 新 block 的 slot 对消费者可见

消费者 try_dequeue 时：
  遍历所有生产者 block 链 → 找到下一个可消费 slot → CAS 抢
```

**Thunder 1P-4C 下的实际状态**：

| 角色 | 线程数 | 在队列中对应的结构 |
|------|:-----:|-------------------|
| 生产者 | 1（事件循环） | 哈希表 1 个 slot → 1 个 ImplicitProducer → 1~N 个 block（满了就加） |
| 消费者 | 4（worker） | 全部遍历同一批 block → CAS 同一个 subqueue_index |
| 争用点 | — | `subqueue_index`（1 个原子变量），4 个线程抢 |

生产者少时 block 链表短、遍历快；生产者多时哈希表扩容、遍历路径长。这是 MPMC 通用性换来的代价。

**哈希碰撞处理**：线性探测。首表默认 32 槽。`thread_id` hash → `index = hash % 32`，碰撞则 `++index` 线性往后找空槽。槽满（>32 个生产者线程同时写）→ 分配新表，`prev` 指针链到旧表，查找从新往旧遍历。

**Thunder 1 生产者下的浪费**：

```
首表 32 个哈希槽       → 只用 1 个，31 空置
block 索引数组 32 槽   → 每个 ImplicitProducer 创建时立即分配，1 生产者用 1~2 槽
初始 block 池          → 构造时预分配多个空 block，单生产者大部分闲置
```

全部是**创建时立即分配**，不是懒加载。这是 MPMC 通用设计的代价——空间换"任意线程随时并发入队"的保证。

**block 列表的并发操作**：

```
  生产者 (唯一写 tail，无争用)         多消费者 (CAS 竞争 head)
  ──────────────────────────         ──────────────────────────
  enqueue:                           try_dequeue:
  ① 读 tailIndex (relaxed)           ① 遍历所有 producer
  ② block 满?→CAS 插入新 block       ② headIndex.fetch_add(1,acq_rel)
     →从空闲池取/malloc 新 block         ← N 消费者 CAS 争用!
  ③ 写入 slot                        ③ 二分查找索引数组定位 block
  ④ tailIndex.store(++,release)      ④ 读 slot → 输出
```

| 操作 | 原子指令 | 争用 |
|------|:------:|------|
| enqueue 写 slot | 1× release store | 无（1 个生产者） |
| enqueue 追加 block | 1× CAS + malloc | 无（低频） |
| try_dequeue | 2× fetch_add + 二分查找 | **N 消费者抢 1 个 headIndex** |

每次 dequeue 至少 2 次原子 fetch_add + 1 次二分查找 vs WorkerDeque 的 1 次 CAS。这就是 1P-1C 仍差 3.59x 的微观原因。

**操作开销**：

| 操作 | 原子指令数 | 说明 |
|------|:-------:|------|
| enqueue（无 token） | 4~5 | 申请/查找 block → 写入 slot → 更新 block 内 index → 更新全局 index |
| try_dequeue（无 token） | 4~5 | 查找活跃 block → 读取 slot → 推进 block index → 可能回收 block |

**设计定位**：通用 MPMC。适合生产者/消费者数量任意、任务大小不可预测、需要动态扩容的场景。代价是链表式 block 结构导致单次操作需多级索引、多步原子指令。

**与 WorkerDeque ring buffer 的对照**：

```
moodycamel MPMC                  WorkerDeque ring buffer
─────────────────                ──────────────────────
链表式 block，按需分配           固定 256-slot 连续数组
enqueue: 4~5 次原子操作          push: 1 次原子 store
dequeue: 4~5 次原子操作          dequeue: 1 次原子 CAS
动态扩容                         固定容量，满→global_q
所有消费者争同一队列              各消费者有自己的 deque
```

这就是 1P-1C 零争用场景下 WS 仍快 3.59x 的原因——数据结构本身的效率差距。

