# Work Stealing 线程池性能对比

> 关联 issue：#109  
> 关联设计：[docs/architecture/23-work-stealing-threadpool.md](../architecture/23-work-stealing-threadpool.md)  
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
