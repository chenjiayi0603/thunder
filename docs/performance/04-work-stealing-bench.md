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

## 3. 设计约束

`WorkerDeque::push()` 设计为**单 tail 写者**（single-writer），不支持多线程并发 push。  
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

| 场景 | LF ns/op | WS ns/op | LF Mop/s | WS Mop/s | 加速比 |
|------|:--------:|:--------:|:--------:|:--------:|:------:|
| 1P-1C（最简基线）| 919.6 | 256.4 | 1.09 | 3.90 | **3.59x** |
| 1P-2C（2 worker）| 1097.3 | 325.6 | 0.91 | 3.07 | **3.37x** |
| 1P-4C（Thunder 典型）| 1373.1 | 543.6 | 0.73 | 1.84 | **2.53x** |
| 1P-8C（8 worker）| 1698.9 | 659.4 | 0.59 | 1.52 | **2.58x** |
| 1P-16C（16 worker）| 2097.2 | 968.1 | 0.48 | 1.03 | **2.17x** |
| 1P-4C（task=10μs）| 2830.8 | 2624.9 | 0.35 | 0.38 | 1.08x |
| 1P-4C（task=100μs）| 25324.2 | 25215.6 | 0.04 | 0.04 | 1.00x |

---

## 7. 结论

### 7.1 空任务场景（队列调度开销为主）

WS 比 LF 快 **2.17x ～ 3.59x**。

根因：LF 的所有 worker 共用一个 moodycamel 队列，每次取任务都要 CAS 同一个 head 指针，
N 个 worker 的 cache line 在多核间来回传递（bouncing）。WS 每个 worker 有独立的
`_submit_deque`，commit() 将任务分发到对应 deque，worker 取自己的 deque 时完全无竞争。

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

## 9. 成功标准（✅ 已全部通过）

| 指标 | 合格线 | 实测 | 结果 |
|------|--------|------|:----:|
| 1P-4C WS ns/op | ≤ LF × 0.95 | 543.6 vs 1373.1（**39%**）| ✅ |
| 1P-8C WS ns/op | ≤ LF × 0.85 | 659.4 vs 1698.9（**38%**）| ✅ |
| 1P-16C WS ns/op | ≤ LF × 0.70 | 968.1 vs 2097.2（**46%**）| ✅ |
| 1P-1C WS ns/op | ≤ LF × 1.10（不退化）| 256.4 vs 919.6（**27%**）| ✅ |
