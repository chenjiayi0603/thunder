# Work-Stealing 线程池设计

> 源码: `code/Util/src/thread/threadpool.h` (WS 实现), `code/test/labor/bench_work_stealing.cpp` (基准)
> 承接事件循环的阻塞/CPU 密集任务，每 worker 独立队列 + 空闲偷取负载均衡

---

## 核心设计

### 三块结构总览

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

每个 worker 持有**两组** deque（Go/TBB 只有一组）。拆成两组的原因：
commit 线程只写 `_submit_deques` 的 tail，worker 只写自己 `_local_deques` 的 tail——两个写者永远不碰同一个 tail，TSan 零警告。

### 数据流路径

| 路径 | 触发条件 | 意图 |
|------|---------|------|
| commit → `_submit_deques[i]` → worker[i] 直接取 | P2C 选中 deque i（>99%） | 零竞争，最快 |
| `_submit_deques` → steal → `_local_deques` → thief 取走 | thief 自己的 deque 空 | 负载均衡 |
| `_local_deques` → steal → 其他 `_local_deques` | 其他 thief 空闲，再偷 | 再扩散，自然拉平 |
| commit → `global_q` (L3 溢出) | 所有 submit_deque 全满 | 兜底，不阻塞事件循环 |
| `global_q` → worker (61-tick) | worker 每 61 次循环强制检查 | 防饥饿 |
| `global_q` → worker (idle 兜底) | worker 五级级联全部落空 | 最后兜底 |
| `_submit + _local` → drain → `global_q` | worker 缩容退出 | 零丢失 |

### WorkerDeque — 固定 256-slot ring buffer

head/tail 各占独立 cache line (`alignas 64B`)，单个 deque ≈12.4 KB。

```cpp
// push — 仅由唯一 tail 写者调用，无需 CAS
bool push(Task t):
    if tail - head >= 256: return false       // 满
    ring[tail % 256] = t; tail.store(tail+1, release)

// dequeue — owner 或 thief 取 1 个，CAS head
optional<Task> dequeue():
    loop: if head == tail: return nullopt     // 空
          t = ring[head % 256]
          if CAS(head, head, head+1): return t

// steal_into — 从 src 批量偷取，1 次 CAS
uint32_t steal_into(src, max_n):
    loop: avail = src.tail - src.head
          n = min(max_n, avail - avail/2)     // 偷一半
          for i in 0..n: my_ring[(my_t+i)%256] = src_ring[(src_head+i)%256]
          if !CAS(src.head, src_head, src_head+n): continue
          my_tail.store(my_t + n, release)
```

### commit() — 三级分发

```
commit(task):
    // L1: Power of Two Choices — 随机采 2 个 deque，选任务少的
    idx = min(size(submit[a]), size(submit[b]))
    if submit[idx].push(task): return           ← >99% 命中

    // L2: 顺序全扫
    for i in 0..N: if submit[i].push(task): return

    // L3: global_q — 所有 deque 全满
    global_q.enqueue(task)
```

### Worker 调度循环

```
worker_loop(id):
    tick = 0
    while _run:
        tick++
        if tick % 61 == 0 and global_q.try_dequeue(): 执行; continue
        if local_deques[id].dequeue(): 执行; continue
        if submit_deques[id].dequeue(): 执行; continue
        if excessThreads > 0 || !_run: drainToGlobal(id); return
        // steal: 最多试 4 个，优先偷 submit_deque
        if active > 1:
            start = xorshift32() % active
            for i in 0..min(4, active-1):
                victim = (start + i) % active; if victim == id: continue
                if steal_into(submit_deques[victim]) > 0
                   || steal_into(local_deques[victim]) > 0:
                    task = local_deques[id].dequeue(); 执行; continue
        if global_q.try_dequeue(): 执行; continue
        _cv.wait_for(10ms, [有任务 || 需退出])
```

### Steal 策略

- 随机起点，最多试 4 个
- 偷一半：全偷 victim 也空→互相偷死循环；偷一个→频繁 steal
- worker 自己的两个 deque 都为空 → **立刻** steal，不等

### Drain

缩容/析构时 worker 退出，deque 里未执行完的任务→`global_q`→其他 worker 61-tick/idle 取走。不能放回别人 deque（退出的 worker 不是任何 deque 的合法 tail 写者）。

---

## 为什么必须是两组 deque

```
commit 线程 ──写 tail──→ 同一个 deque 的 tail ←──写 tail── thief worker
                              ↓
                          data race
```

一组 deque：commit 的写入和 thief 的 steal 写入并发操作同一个 tail。

```
commit 线程 ──写 tail──→ _submit_deques[*]
                               │
                        相互独立，写者不重叠
                               │
thief worker ──写 tail──→ _local_deques[id]
```

两组 deque：写者物理隔离。

---

## 并发模型

```
WorkStealingPool
├── _submit_deques[0..N]   SPMC  ← commit 写 tail；worker[i] 取，其他 worker 可偷
├── _local_deques[0..N]    SPMC  ← worker[i] steal_into 写 tail；worker[i] 取，其他 worker 可偷
└── global_q               MPMC  ← commit/drain 写；任意 worker 取 (moodycamel)
```

旧 LF 方案：单一 MPMC，所有 worker 争一个 head。
WS 方案：N 个 SPMC + 1 个兜底 MPMC，正常路径零争用。

---

## 性能实测

> 基准: `code/test/labor/bench_work_stealing.cpp`, Linux 7.0, `-O2`, 500K 空任务
> **LF** = 旧单队列线程池 (moodycamel MPMC) | **WS** = Work-Stealing 线程池

### 吞吐对比

| 场景 | LF ns/op | WS ns/op | 加速比 |
|------|:-------:|:-------:|:-----:|
| 1P-1C | 919.6 | 256.4 | **3.59x** |
| 1P-2C | 1097.3 | 325.6 | **3.37x** |
| **1P-4C（Thunder 典型配置）** | **1373.1** | **543.6** | **2.53x** |
| 1P-8C | 1698.9 | 659.4 | **2.58x** |
| 1P-16C | 2097.2 | 968.1 | **2.17x** |
| 1P-4C, task=10μs | 2830.8 | 2624.9 | 1.08x |
| 1P-4C, task=100μs | 25324.2 | 25215.6 | 1.00x |

```
LF: 1C→16C 增长 +128%（单队列争用线性恶化）
WS: 1C→16C 增长 +278%（绝对值仍比 LF 低 2x+）
```

### 端到端延迟 + payload 大小

| 场景 | LF P50 ns | WS P50 ns | LF avg ns | WS avg ns | avg 加速比 |
|------|:-------:|:-------:|:-------:|:-------:|:--------:|
| 4 worker, 0B | 1586 | 1227 | 1394 | 700 | **1.99x** |
| 4 worker, 64B | 2434 | 1076 | 1789 | 694 | **2.58x** |
| 4 worker, 256B | 2066 | 1209 | 1668 | 770 | **2.17x** |
| 4 worker, 1KB | 1787 | 1207 | 1594 | 823 | **1.94x** |
| 4 worker, 4KB | 2040 | 1737 | 1794 | 1326 | **1.35x** |

### 结论

- **WS 在任何场景下不慢于 LF。** Thunder 典型配置（4 worker）：吞吐快 **2.53x**，avg 延迟快 **1.99x**
- 任务越轻优势越大（瓶颈在队列调度开销，WS 自有 deque 不需争抢同一队列头）
- 重任务（≥100μs）瓶颈在任务体自身，队列差异被淹没
- payload 64B~256B 时 WS 优势最大（2.17x~2.58x avg）
- worker ≤ 2 时用旧实现即可

### 1P-1C 零争用仍快 3.59x 的原因

数据结构本身的效率差距，与争用无关：

```
moodycamel MPMC                  WorkerDeque ring buffer
─────────────────                ──────────────────────
链表式 block，按需分配           固定 256-slot 连续数组
enqueue: 4~5 次原子操作          push: 1 次原子 store
dequeue: 4~5 次原子操作          dequeue: 1 次原子 CAS
动态扩容                         固定容量，满→global_q
所有消费者争同一队列              各消费者有自己的 deque
```

---

## LF 队列基准（附录 — moodycamel vs Mutex）

> `code/test/labor/bench_threadpool_queue.cpp` | **Mutex** = `std::queue + mutex + cv` | **LF** = `moodycamel::ConcurrentQueue`

| 场景 | Mutex ns/op | LF ns/op | 加速比 |
|------|:-----------:|:--------:|:------:|
| 4P-4C（典型 offload）| 313.2 | 127.5 | 2.46x |
| 16P-4C（高并发 commit）| 335.5 | 118.6 | 2.83x |
| 1P-4C（单协程基线）| 653.2 | 174.6 | 3.74x |
| 8P-8C（对等压力）| 368.2 | 115.5 | 3.19x |
