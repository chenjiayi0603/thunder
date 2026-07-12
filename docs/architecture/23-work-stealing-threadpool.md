# Work-Stealing 线程池设计

> 关联 issue：#109  |  设计日期：2026-06-21  
> 前置文档：[20-threadpool-analysis.md](./20-threadpool-analysis.md)  
> 性能基准：[04-work-stealing-bench.md](../performance/04-work-stealing-bench.md)

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

> 表注：task=0，延迟 = commit() 前 → worker 取出执行。payload 按值捕获在 lambda 中。完整数据见 [04-work-stealing-bench.md §10](../performance/04-work-stealing-bench.md#10-端到端延迟--payload-大小run-2----2026-07-07)

### 5.3 结论

> **WS 在任何场景下不慢于 LF。** Thunder 典型配置（4 worker）：吞吐快 **2.53x**，端到端延迟快 **1.99x**（avg）/**1.29x**（P50）。任务越轻优势越大——轻任务瓶颈在队列调度开销，WS 每个 worker 自有 deque、不用争抢同一个队列头；重任务（≥100μs）瓶颈在任务体自身，队列差异被淹没。payload 64B~256B 时 WS 优势最大（2.17x~2.58x avg）。worker ≤ 2 时用旧实现即可。

---

## 6. 参考资料

- Go runtime: `runtime/proc.go` — `runqget` / `runqgrab` / `stealWork`
- Intel TBB: `src/tbb/task_dispatcher.h` — Chase-Lev 动态 deque
- Chase & Lev, "Dynamic circular work-stealing deque", SPAA 2005
- Lê et al., "Correct and efficient work-stealing for weak memory models", PPoPP 2013
- Mitzenmacher, "The Power of Two Choices in Randomized Load Balancing", TPDS 2001
