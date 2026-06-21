# Work-Stealing 线程池设计

> 关联 issue：#109  
> 设计日期：2026-06-21  
> 前置文档：[20-threadpool-analysis.md](./20-threadpool-analysis.md)

---

## 1. 现状与问题

经过 #92~#96 修复，当前线程池已解决 UB、内存管理、无锁队列、背压等问题。架构如下：

```
                     commit()
                        │
                        ▼
        ┌─────────────────────────────┐
        │  moodycamel::ConcurrentQueue │  ← 单个全局无锁 MPMC 队列
        └──────────────┬──────────────┘
                       │ try_dequeue
          ┌────────────┼────────────┐
          ▼            ▼            ▼
       Worker-0     Worker-1     Worker-2    (spin-yield 轮询)
```

**残留问题**：空闲 worker 全部自旋轮询同一个队列。即使 moodycamel 是无锁的，多个核同时写同一个 cache line（队列头指针）仍会产生 **cache line bouncing**。在 8+ worker、高并发 offload 场景下，这是性能天花板。

**Work Stealing 解决的核心矛盾**：  
每个 worker 有自己的本地队列 → 空闲时才去别人队列偷 → **绝大多数时候没有跨核竞争**。

---

## 2. 参考实现分析

### 2.1 Go Runtime：GMP + Per-P 本地队列（LRQ）

Go 运行时使用 G（goroutine）、M（OS 线程）、P（调度上下文）三层模型：

```
  GRQ (Global Run Queue)   ← overflow 和 1/61 轮询防饥饿
       ↓
  P0 [LRQ: 256]    P1 [LRQ: 256]    P2 [LRQ: 256]
     runnext (1)      runnext (1)      runnext (1)
        │                │                │
       M0               M1               M2
```

**LRQ 结构**：固定 256 容量的 ring buffer，`runqhead`/`runqtail` 是原子 `uint32`。

**操作语义**：

| 操作 | 端 | 执行者 | 实现 |
|------|----|--------|------|
| push | tail++ | 本 P（唯一写者）| release store，无竞争 |
| dequeue | CAS head++ | 本 P | 与 steal 竞争同一个 head |
| **steal** | **CAS head++，取 n/2** | **其他 P** | 与 dequeue 竞争同一个 head |

**关键点**：dequeue（owner 取任务）和 steal（其他线程偷任务）是**对称操作**，都 CAS `head`。push 独占 `tail`，push 与 dequeue/steal 天然分离在两端，零竞争。

**Go 完整调度行为伪码**（对照 `runtime/proc.go`）：

```
// ── 生产侧 ──────────────────────────────────────────────────────

// goroutine 在 P 上 spawn 新 goroutine
runqput(P, G):
    // runnext：1-slot LIFO，最刚解锁的 G 优先（cache 最热）
    old = P.runnext
    if CAS(P.runnext, old, G):
        if old == nil: return           // runnext 空，直接放进去
        G = old                         // 把被挤出的 old 推入 ring

    // 推入 ring 尾部
    t = P.runqtail                      // tail 只有本 P 写，无竞争
    P.runq[t % 256] = G
    store_release(P.runqtail, t + 1)

    // LRQ 满（tail - head == 256）→ 溢出到 GRQ
    if t - P.runqhead == 256:
        runqputslow(P, G)               // 把 LRQ 一半 + G 搬到 GRQ（加锁）

// ── 消费侧 ──────────────────────────────────────────────────────

// P 的主调度循环（findRunnable）
schedule(P):
    tick++

    // 1/61 强制从 GRQ 捞一个（防饥饿）
    if tick % 61 == 0 and GRQ.size > 0:
        G = globrunqget(P, 1)           // 从 GRQ 取 1 个，加锁
        if G: run(G)

    // 取 runnext（LIFO 特例）
    G = CAS(P.runnext, old, nil)
    if G: run(G)

    // 从 LRQ head 取（FIFO）
    G = runqget(P):
        h = load_acquire(P.runqhead)
        t = P.runqtail
        if h == t: return nil           // LRQ 空
        G = P.runq[h % 256]
        if CAS(P.runqhead, h, h+1): return G
        // CAS 失败 = thief 同时在 steal，重试

    if G: run(G)

    // LRQ 空 → steal
    G = stealWork(P):
        for try in 0..4:                // 最多 4 轮
            victim = random_P()
            G = runqgrab(victim, P):    // 偷 n/2 到本地 batch
                h = load_acquire(victim.runqhead)
                t = load_acquire(victim.runqtail)
                n = (t - h) - (t-h)/2  // 偷一半
                copy victim.runq[h..h+n) → batch
                if CAS(victim.runqhead, h, h+n): return n
                else: retry             // 1 次 CAS，无论偷几个
            if G: run(G)

    // steal 也失败 → 从 GRQ 捞（globrunqget）
    if GRQ.size > 0:
        G = globrunqget(P, GRQ.size/N + 1)   // 按比例批量捞
        if G: run(G)

    // 真空 → 休眠，等 M 唤醒
    stopm()
```

**与 Thunder 的对应关系**：

| Go | Thunder |
|----|---------|
| 每个 P 自己 spawn → push 自己 LRQ | 事件循环 → Power of Two Choices → push 负载最轻的 deque |
| runnext（1-slot LIFO）| 无（offload 任务无 LIFO 需求）|
| LRQ 满 → 搬一半到 GRQ | deque 满 → 全扫其他 deque → global_q |
| 1/61 从 GRQ 捞（防饥饿）| 61-tick 从 global_q 捞（防饥饿）|
| steal n/2，1 次 CAS | steal n/2，1 次 CAS（相同）|
| 随机受害者 + 4 次重试 | 随机受害者 + min(4, N-1) 次重试（相同）|
| steal 失败 → GRQ → stopm | steal 失败 → global_q → yield（相同）|

---

### 2.2 Intel TBB：Chase-Lev Work-Stealing Deque

TBB 每个 worker 维护一个**双端队列（deque）**，两端分别给 owner 和 thief：

```
      STEAL 端（FIFO，其他线程 CAS top）        PUSH/POP 端（LIFO，owner 操作 bottom）
      top →  [ T₀  T₁  T₂  T₃  T₄ ] ← bottom
              最老任务                   最新任务
```

Chase-Lev 的核心假设：**push 和 pop 是同一个线程（owner）**，owner 独占 bottom 端，无需 CAS。Steal 独占 top 端 CAS。仅在最后一个元素时两端才发生竞争。

**与 Go LRQ 的根本区别**：

| | Go LRQ | Chase-Lev |
|--|--------|-----------|
| push 端 | tail（owner 独占）| bottom（owner 独占）|
| dequeue/pop 端 | head（CAS，与 steal 竞争）| bottom（--bottom，与 push 同端，无 CAS）|
| steal 端 | head（CAS）| top（CAS）|
| push 与 pop 是否同线程 | 不要求 | **必须同线程**（核心前提）|
| 容量 | 固定 256 | 动态扩容 |

Chase-Lev 的 owner pop 无需 CAS（性能优势），代价是 push/pop 必须同线程，且动态扩容引入 epoch GC 复杂度。

---

### 2.3 两者对比

| 维度 | Go LRQ | TBB Chase-Lev |
|------|--------|--------------|
| 队列容量 | 固定 256 | 动态翻倍 |
| push/pop 是否同线程 | 不要求 | 必须同线程 |
| dequeue 是否需要 CAS | 是（与 steal 共享 head）| 否（独占 bottom）|
| 实现复杂度 | 低 | 高（epoch GC）|
| 适用场景 | 通用任务并发 | 递归任务分解（parallel_for）|

---

## 3. Thunder 的设计约束

```
事件循环 (单线程，每 Worker 进程一个)
    │
    co_await MakePoolOffloadAwaiter(...)
    │
    └─→ threadpool.commit(task)   ← 单一提交点
           │
       offload 任务（阻塞 IO / CPU 密集）
           │
       PostToEventLoop(resume)    ← 完成后回事件循环
```

**关键约束**：

1. **push 和 pop 是不同线程**：事件循环调用 `commit()` (push)，线程池 worker 执行任务 (pop/steal)。Chase-Lev 要求 push/pop 同线程，**前提不成立**。
2. **单一生产者**：push 只有事件循环一个线程，tail 端无竞争。
3. **任务独立**：offload 任务之间无依赖，不需要 TBB 的 Continuation Stealing。
4. **固定上限足够**：短任务（1ms~100ms），256 slot 足以吸收突发，不需要动态扩容。
5. **对外 API 不变**：`commit()` 接口保持兼容，上层 `PoolOffloadAwaiter` 无需修改。
6. **push 与 steal_into(dst) 不能共享同一 deque 的 tail**：`commit()` 调用 `push()`（写 tail），worker 的 `steal_into()` 作为目标端也写 tail，若二者指向同一个 deque，构成数据竞争（TSan 验证）。

**选型结论**：采用 **Go LRQ 风格**——固定 256-slot ring，push 独占 tail，所有 worker（包括 owner）统一 CAS head 取任务。需额外将 commit 写入目标与 steal 写入目标**分成两组 deque**，消除 tail 竞争。

---

## 4. 详细设计

### 4.1 整体架构

每个 worker 持有两个 WorkerDeque，各有唯一 tail 写者，消除 `push` 与 `steal_into` 的竞争：

```
事件循环线程 (唯一 push 者)
      │
      │ commit(task)  Power of Two Choices → push 到负载最轻的 _submit_deque
      │
      ├──────────────────┬──────────────────┐
      ▼                  ▼                  ▼
 ┌─────────────────────────────────────────────────────────────┐
 │                     WorkStealingPool                        │
 │                                                             │
 │  _submit_deques[0]  tail← [T T ...] →head                  │
 │  _submit_deques[1]  tail← [T T ...] →head   ← commit() 写  │
 │  _submit_deques[2]  tail← [T T ...] →head                  │
 │          worker dequeue() ↑  thief steal_into(src) ↑       │
 │                                                             │
 │  _local_deques[0]   tail← [T T ...] →head                  │
 │  _local_deques[1]   tail← [T T ...] →head   ← worker 写    │
 │  _local_deques[2]   tail← [T T ...] →head     (steal dst)  │
 │          worker dequeue() ↑  thief steal_into(src) ↑       │
 │                                                             │
 │  moodycamel::ConcurrentQueue  (global overflow)             │
 └─────────────────────────────────────────────────────────────┘
```

**两组 deque 的 tail 写者**：

| deque 组 | tail 写者 | head 读者 |
|----------|-----------|-----------|
| `_submit_deques[id]` | 仅 `commit()` 线程（push）| worker[id] dequeue；任意 thief steal_into(src) |
| `_local_deques[id]` | 仅 `worker[id]`（steal_into dst）| worker[id] dequeue；任意 thief steal_into(src) |

`push()` 与 `steal_into(dst)` 永远写不同 deque，tail 单写者约束在两组分别成立，无数据竞争。

**数据流向**：commit → `_submit_deques` → worker dequeue 直接消费；空闲时 steal_into 搬入 `_local_deques` → worker 继续消费；`_local_deques` 也可被其他 thief 进一步偷走。

---

### 4.2 WorkerDeque：Go LRQ 风格

```
push(tail++)  ←  该 deque 的唯一 tail 写者
                 （_submit_deques → commit 线程；_local_deques → 所属 worker）

  [  T₀   T₁   T₂  ...  T₂₅₅  ]

dequeue(CAS head++)  ←  所属 worker 或 thief（CAS 竞争 head）
```

`tail` 只有唯一写者，`head` 被多个 worker CAS。两个变量各占独立 cache line（`alignas(64)`），push 与 dequeue/steal 天然无竞争。

**push**（只有事件循环线程调用）：

```cpp
bool push(Task task) {
    uint32_t h = head.load(memory_order_acquire);
    uint32_t t = tail.load(memory_order_relaxed);
    if (t - h >= CAP) return false;        // 队列满，走 overflow
    ring[t % CAP] = std::move(task);
    tail.store(t + 1, memory_order_release);
    return true;
}
```

**dequeue**（worker 取自己 deque，1 个）：

```cpp
std::optional<Task> dequeue() {
    for (;;) {
        uint32_t h = head.load(memory_order_acquire);
        uint32_t t = tail.load(memory_order_acquire);
        if (h == t) return std::nullopt;   // 空
        Task task = ring[h % CAP];
        if (head.compare_exchange_weak(h, h + 1,
                memory_order_seq_cst,
                memory_order_relaxed)) {
            return task;
        }
        // CAS 失败 = 其他 worker 同时在取，重试
    }
}
```

**steal_into**（从 victim deque 批量偷取到自己 deque，1 次 CAS）：

```cpp
// 从 src 偷最多 max_n 个任务追加到本 deque 尾部，返回实际偷取数
// 调用前提：只有本 worker 调用（tail 单写者）
uint32_t steal_into(WorkerDeque& src, uint32_t max_n) {
    for (;;) {
        uint32_t sh = src.head.load(memory_order_acquire);
        uint32_t st = src.tail.load(memory_order_acquire);
        uint32_t avail = st - sh;
        if (avail == 0) return 0;

        // 偷 avail/2，上限 max_n，再受本 deque 剩余空间限制
        uint32_t n = std::min(max_n, avail - avail / 2);
        uint32_t my_h = head.load(memory_order_acquire);
        uint32_t my_t = tail.load(memory_order_relaxed);
        n = std::min(n, CAP - (my_t - my_h));  // 不超出本 deque 容量
        if (n == 0) return 0;

        // 先把任务复制进本 deque（tail 尚未更新，其他线程看不到）
        for (uint32_t i = 0; i < n; i++)
            ring[(my_t + i) % CAP] = src.ring[(sh + i) % CAP];

        // 1 次 CAS 推进 src.head，宣告这 n 个 slot 归我
        if (!src.head.compare_exchange_weak(sh, sh + n,
                memory_order_seq_cst, memory_order_relaxed))
            continue;   // 被其他 thief 抢了，重试

        // CAS 成功后再公开本 deque 的新 tail
        tail.store(my_t + n, memory_order_release);
        return n;
    }
}
```

> **为什么先复制再 CAS**：写入本 deque 的 ring 不需要同步（tail 还没更新，无人能读到这些 slot）；CAS 成功后一次 release store 更新 tail，让其他 worker 可见。整个偷取过程只有 **1 次 CAS**，与偷取数量 n 无关。

---

### 4.3 配置参数：steal_batch

```cpp
// 构造 threadpool 时可指定，默认 = CAP/2 = 128
// 0 表示每次只偷 1 个（退化为单个 dequeue）
size_t _stealBatch = WorkerDeque::CAP / 2;
```

| _stealBatch | 适用场景 |
|-------------|---------|
| 1 | 任务极长（秒级），偷多了没意义 |
| **CAP/2（默认）** | **通用：短任务突发时快速分散，长任务无害** |
| CAP | 激进分散，适合极短任务（<1ms）大量并发 |

**为什么默认偷 n/2 而不是全部或更少？**

```
假设 Victim 有 10 个任务：

全部偷走：
  Victim 立刻饿死 → 去 global 或反过来偷别人
  → A 偷 B，B 转头偷 A，产生 ping-pong，反复竞争

偷 1/4（2~3 个）：
  Thief 很快消费完，需要频繁发起新一轮 steal
  → CAS 次数多，均衡速度慢

偷 1/2（5 个）：
  Victim 还剩 5 个，短期不饿
  Thief 拿到 5 个，短期不需要再偷
  → 系统自然趋向均衡，steal 频率最低
```

`steal_into` 实现的实际偷取量 = `min(_stealBatch, avail - avail/2)`，即"不超过 victim 现有任务的一半"，与 Go LRQ 保持一致。`_stealBatch` 是额外上限，不会超过配置值。

---

### 4.4 偷取规则：选谁偷、怎么偷

**Step 1：随机起点**

```
start = xorshift32()  // 轻量随机，避免 % 的除法用位运算替代
```

不从固定位置（如 0）开始，否则所有空闲 worker 都先盯 Deque-0，造成热点。随机起点让每个 worker 从不同位置出发，天然分散压力。

**Step 2：顺序扫描，最多尝试 min(4, N-1) 次**

```
for i in 0 .. min(4, N-1):
    victim = (start + i) % N
    if victim == id: continue         // 跳过自己
    n = deques[id].steal_into(deques[victim], _stealBatch)
    if n > 0: break                   // 偷到就停
```

- **上限 4 次**：来自 Go 的经验值。N 很大时不全扫，避免 steal 本身成为开销；4 次足以覆盖大概率有任务的 victim。
- **跳过自己**：自己的 deque 在 Step 1 已经确认是空的。
- **偷到就停**：不贪，偷到 n 个后立刻执行，不再继续扫描。

**Step 3：steal 失败 → global queue → yield**

```
if 4 次 steal 均失败:
    if task = global_q.try_dequeue(): execute(task)
    else: this_thread::yield()
```

global_q 是兜底，存放 Deque-0 溢出的任务。yield 让出时间片，避免 busy-wait 空转。

**完整流程图**：

```
worker 空闲
    │
    ├─→ dequeue(自己 deque)
    │       成功 → 执行
    │       失败 ↓
    │
    ├─→ 随机起点，最多尝试 4 次
    │   victim = (start+i) % N
    │       steal_into(victim, _stealBatch)
    │           成功 → dequeue(自己 deque) → 执行
    │           失败 → 下一个 victim
    │       4 次全败 ↓
    │
    ├─→ global_q.try_dequeue()
    │       成功 → 执行
    │       失败 ↓
    │
    └─→ yield()
```

---

### 4.5 Worker 调度循环（伪码）

```
worker_loop(id):
    tick = 0
    while _run:
        tick++

        // 0. 61-tick：防高负载下 global_q 饥饿
        if tick % 61 == 0:
            if task = global_q.try_dequeue():
                execute(task); continue

        // 1. 先消费本地 steal buffer（无任何竞争）
        if task = local_deques[id].dequeue():
            execute(task); continue

        // 2. 再消费 commit 投递的 submit deque
        if task = submit_deques[id].dequeue():
            execute(task); continue

        // 3. 两个 deque 均空 → 退出检查
        if _excessThreads > 0 or not _run:
            drainToGlobal(id)   // 两个 deque 剩余任务全部 drain 到 global_q
            return

        // 4. steal：随机起点，最多 min(4, N-1) 次
        //    目标写入自己的 local_deque（commit 线程不碰 local_deque，无竞争）
        start = xorshift32() % N
        stole = false
        for i in 0..min(4, N-1):
            victim = (start + i) % N
            if victim == id: continue
            if local_deques[id].steal_into(submit_deques[victim], kStealBatch) > 0
               or local_deques[id].steal_into(local_deques[victim], kStealBatch) > 0:
                task = local_deques[id].dequeue()
                execute(task); stole = true; break
        if stole: continue

        // 5. idle 路径：steal 全败 → 捞 global_q
        if task = global_q.try_dequeue():
            execute(task); continue

        // 6. 真空 → yield
        this_thread::yield()
```

偷来的 n 个任务进入 `local_deques[id]` 后，剩余 n-1 个：
- 下一轮 step 1 继续消费
- 或被其他空闲 worker 继续 `steal_into(src=local_deques[id])` 偷走 → 自然扩散到全池

---

### 4.6 commit() 流程（生产侧）

**分发策略：三级 push，目标仅 `_submit_deques`**

| 级别 | 策略 | 开销 | 触发条件 |
|------|------|------|---------|
| 第一级 | Power of Two Choices | O(1)，2 次 relaxed load | 正常路径 |
| 第二级 | 顺序全扫 | O(N)，N≤16 | 第一级采样的 deque 恰好满了 |
| 第三级 | global_q | O(1) | 所有 submit_deque 同时满（极端 burst）|

commit() **只写 `_submit_deques`，不碰 `_local_deques`**。`_local_deques` 的 tail 由各 worker 独占写入，commit 线程永远不访问，从根本上消除 tail 竞争。

`size()` = `tail - head`（两次 relaxed load，极快，无锁）。

```
commit(task):
    // 背压检查（语义不变）
    sz = _queueSize.fetch_add(1)
    if sz >= _maxQueueSize:
        _queueSize.fetch_sub(1)
        throw "queue full"

    // 第一级：Power of Two Choices（仅采样 _submit_deques）
    a = xorshift32() % N
    b = xorshift32() % N
    idx = (submit_deques[a].size() <= submit_deques[b].size()) ? a : b
    if submit_deques[idx].push(task): return

    // 第二级：顺序扫全部 _submit_deques
    for i in 0..N:
        if submit_deques[i].push(task): return

    // 第三级：全部 submit_deque 都满 → global_q 蓄水
    global_q.enqueue(task)

    // task 执行完后 _queueSize.fetch_sub(1)
```

事件循环单线程，`xorshift32` 无需 atomic，第二级全扫极少触发。

---

### 4.7 global_q 消费路径（消费侧）

global_q 里的任务通过两条独立路径消费，覆盖所有场景：

| 场景 | 触发路径 | 说明 |
|------|---------|------|
| deque 持续有任务（高负载），global_q 也有积压 | **61-tick 定期捞** | 防饥饿：每执行 61 个任务强制检查一次 |
| 两个 deque 均为空，steal 也失败 | **idle 路径** | local + submit 空 → steal 失败 → 检查 global_q |

完整 worker 循环已合并在 §4.5。不存在漏掉的情况：
- 两 deque 持续非空 → 61-tick 定期检查 global_q
- 两 deque 消费完 → idle 路径自然检查 global_q

---

### 4.8 内存布局

```
┌──────────────────────────────────────────────────────────────────────┐
│  WorkStealingPool                                                    │
│                                                                      │
│  _submit_deques[0]: | head (64B) | tail (64B) | ring[256 × 48B] |   │
│  _submit_deques[1]: | head (64B) | tail (64B) | ring[256 × 48B] |   │
│  ...  (THREADPOOL_MAX_NUM = 16 个)                                   │
│                                                                      │
│  _local_deques[0]:  | head (64B) | tail (64B) | ring[256 × 48B] |   │
│  _local_deques[1]:  | head (64B) | tail (64B) | ring[256 × 48B] |   │
│  ...  (同上，16 个)                                                  │
│                                                                      │
│  global_q: moodycamel::ConcurrentQueue<Task>                        │
└──────────────────────────────────────────────────────────────────────┘
```

单个 deque：128B（head+tail）+ 256×48B ≈ 12.4 KB  
两组共 32 个 deque：32 × 12.4 KB ≈ **397 KB**，可接受。

---

### 4.9 数据竞争分析与两 deque 设计的必要性

**原始设计（单 deque 数组）的竞争**：

```
commit()（test/事件循环线程）   worker[id]（steal_into 作为 dst）
        │                              │
        │ push() → _ring[tail] = task  │ steal_into() → _ring[my_t] = stolen
        │ _tail.store(tail+1)          │ _tail.store(my_t+n)
        ▼                              ▼
        同一个 _deques[id] 的 _ring[] 和 _tail
        → 两线程同时写 → TSan: data race on std::function
        → shared_ptr<packaged_task> 内部指针损坏
        → 同一任务被执行两次 → "Promise already satisfied"
```

**两 deque 组如何消除竞争**：

```
commit()          →  _submit_deques[id].push()   ← 唯一写 tail 的线程：commit
worker[id]        →  _local_deques[id] steal_into dst  ← 唯一写 tail 的线程：worker[id]
                         ↓
                  两组 deque 的 tail 写者各不相同，永远不重叠
                  head 的竞争（dequeue + steal）由 CAS 正确处理
```

**TSan 验证**（`-fsanitize=thread`）：修复前报 `data race at worker_deque.h:51/116`，修复后零警告，18 个集成测试全绿。

---

## 5. 与现有实现对比

### 5.0 当前实现结构（`threadpool.h`）

```
                     commit()
                        │  enqueue (moodycamel 内部 CAS)
                        ▼
        ┌─────────────────────────────┐
        │  moodycamel::ConcurrentQueue │  ← 单个全局无锁 MPMC 队列，动态增长
        └──────────────┬──────────────┘
              try_dequeue │ (N 个 worker 同时 CAS 同一个 head)
          ┌─────────────┼─────────────┐
          ▼             ▼             ▼
       Worker-0      Worker-1      Worker-2   → yield() → 重试
```

**取任务路径**（当前）：
```
while true:
    if _tasks.try_dequeue(task):   // 所有 worker 竞争同一个队列 head
        task()
    else if 应退出: return
    else: yield()
```

**新 Work Stealing 取任务路径**：
```
while true:
    if tick % 61 == 0: try global_q          // 防饥饿
    if deques[id].dequeue(task): task()       // 自己的 deque，极少竞争
    else: steal_into(random_victim, n/2)      // steal 失败才竞争
    if global_q.try_dequeue(task): task()     // idle 兜底
    else: yield()
```

### 5.1 队列结构对比

| 维度 | 当前（单全局队列）| Work Stealing（Go LRQ 风格）|
|------|:-----------------:|:---------------------------:|
| 数据结构 | 1 个 moodycamel MPMC | N 个固定 ring deque + 1 个 global_q |
| worker 取任务 | N 核同时 CAS 同一个 head | 各取自己 deque，竞争极少 |
| cache line 争用 | 高（所有 worker 争同一 CL）| 低（head 只在被 steal 时被竞争）|
| commit 分发 | enqueue 到同一队列 | P2C 选负载最轻的 deque |
| 负载均衡 | 隐式（FIFO 先到先得）| 显式（steal n/2 主动拉平）|
| global_q 使用频率 | **每次**取任务都走它 | 仅极端 burst 或 idle 时才触发 |
| 动态扩容 | moodycamel 自动 | global_q 也是 moodycamel，同样自动 |
| 实现复杂度 | 简单 | 较复杂（WorkerDeque + steal 逻辑）|

> **global_q 的关键优势**不是"不扩容"（两者都用 moodycamel，都能动态增长），
> 而是**正常路径完全不碰它**——只在所有 deque 同时满（极端 burst）或 worker 空闲时才降级进去。
> 当前实现每次取任务都经过同一个 moodycamel head，N worker 高并发时这个 CAS 就是热点。

### 5.2 为什么 4+ worker 时 Work Stealing 值得

**cache line bouncing 原理**

CPU 读写数据以 64 字节（cache line）为单位。队列的 `head` 指针就住在某个 cache line 里：

```
Worker-0 读 head  → 自己 L1 有一份 head（~4ns）
Worker-1 CAS head → 硬件广播"此 cache line 已修改，其他核的副本作废"
Worker-0 再读 head → L1 失效，必须从 L3/内存重新拉（~40ns+）

4 个 worker 同时轮询 → head 这个 cache line 在 4 个核间来回"弹"
                      → 每次取任务变成跨核内存访问
```

**额外路径代价**

Work Stealing 多了"先查自己 deque"这一步：

```
当前：              try_dequeue(global_q)  → 跨核 CAS，~40ns+
Work Stealing：    deques[id].dequeue()   → 本地 L1，~4ns
                   ↓ 取不到才
                   steal_into(victim)     → 跨核，但低频
```

多出的本地检查代价约 4ns，换掉原来 40ns+ 的跨核竞争。**4+ worker 时 bouncing 变成常态，
用 `perf stat` 能观测到明显的 `LLC-load-misses` 上升，此时额外路径代价 < 竞争消除收益。**

**优缺点汇总**

| | 当前（单全局队列）| Work Stealing |
|--|:----------------:|:-------------:|
| 优点 | 实现简单、稳定、天然 FIFO | 本地取任务无争用、steal 自动负载均衡、扩展性好 |
| 缺点 | N worker 争同一 head，扩展性有天花板 | 实现复杂、worker 少时路径多余、调试难度增加 |
| 适用规模 | 1~2 worker | 4+ worker |

**结论**：Thunder 配置 4~8 个线程时，Work Stealing 是值得的。但属于性能优化，
建议先用 `bench_threadpool_queue.cpp` 跑基准确认瓶颈，有数据支撑再实施。

---

## 7. 与现有设计的关系

### 5.1 对外 API 不变

```cpp
pool.commit([](){ /* 耗时工作 */ });
pool.idlCount();
pool.queueSize();
pool.maxQueueSize();
pool.resize(n);
```

`PoolOffloadAwaiter` / `MakePoolOffloadAwaiter` 不感知队列结构，无需修改。

### 5.2 _queueSize 语义扩展

`_queueSize` 计 Deque-0 + global queue 的总 task 数，语义从"单队列深度"变为"池中待执行任务总数"，更准确。

### 5.3 resize() 行为

`_excessThreads` 标记机制不变。缩容退出的 worker 在退出前将自己 deque 中剩余任务 drain 到 global_q，避免任务丢失。

---

## 8. 性能预期

| 场景 | 当前（单全局队列）| Work Stealing（Go LRQ 风格）|
|------|:----------------:|:---------------------------:|
| N worker 空闲轮询 | N 核同时 CAS 同一个 head | 各取自己 deque，仅 CAS 失败时竞争 |
| cache line bouncing | 高（所有 worker 争一个 CL）| 低（deque 的 head 只被少量 worker CAS）|
| push 竞争 | 无（moodycamel 内部处理）| 无（tail 只有事件循环写）|
| 空闲 worker CPU | N 核全程轮询 global_q | 取自己 → steal → global → yield，递减搜索 |

预期：worker 数 ≥ 4、提交频率高时，P99 提交延迟降低 20~40%。

---

## 9. 实现计划

### Phase A：WorkerDeque 数据结构 ✅

- [x] 新文件 `code/Util/src/thread/worker_deque.h`
- [x] 实现 `WorkerDeque<T, N>` 模板（`push(Task&&)` / `dequeue` / `steal_into`）
- [x] 单元测试 13 个全绿（`test_util_worker_deque.cpp`）

### Phase B：WorkStealingPool 集成 ✅

- [x] 新文件 `code/Util/src/thread/work_stealing_pool.h`（不修改现有 `threadpool.h`）
- [x] 两组 deque：`_submit_deques`（commit 写）+ `_local_deques`（worker steal 写）
- [x] `commit()` 三级分发：P2C → 全扫 → global_q，仅写 `_submit_deques`
- [x] worker 循环：local → submit → steal → global_q → yield，61-tick 防饥饿
- [x] 缩容 drain：两个 deque 均 drain 到 global_q
- [x] 集成测试 18 个全绿（`test_util_work_stealing_pool.cpp`）
- [x] TSan 验证零数据竞争（`-fsanitize=thread`）
- [x] 全量 ctest 387 个零回归

### Phase C：测试验证

- [ ] `bench_work_stealing.cpp` 对比基准（WS vs 单全局队列）
- [ ] 填写 `docs/performance/04-work-stealing-bench.md` 实测数据
- [ ] E2E smoke 验证

---

## 10. 风险与权衡

| 风险 | 说明 | 缓解 |
|------|------|------|
| 内存占用增加 | 16 worker × 12 KB ≈ 200 KB | 可接受 |
| Deque-0 满时溢出 global | 提交 TPS 极高时 256 slot 可能不足 | global_q 兜底；256 对 ms 级任务已足够 |
| CAS 竞争（dequeue）| 多 worker 同时 dequeue 同一 deque | CAS 失败重试；竞争概率低于单全局队列 |
| 缩容时任务丢失 | worker 退出时 deque 可能有剩余 | 退出前 drain 到 global_q |

---

## 11. 参考资料

- Go runtime: `runtime/proc.go` `runqget` / `runqgrab` / `stealWork`
- Intel TBB: `src/tbb/task_dispatcher.h`（Chase-Lev 动态 deque）
- Chase & Lev, "Dynamic circular work-stealing deque", SPAA 2005
- Nhat Minh Lê et al., "Correct and efficient work-stealing for weak memory models", PPoPP 2013
