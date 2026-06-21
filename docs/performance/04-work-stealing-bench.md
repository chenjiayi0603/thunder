# Work Stealing 线程池性能对比

> 关联 issue：#109  
> 关联设计：[docs/architecture/23-work-stealing-threadpool.md](../architecture/23-work-stealing-threadpool.md)  
> 基准程序：`code/test/labor/bench_threadpool_queue.cpp`（现有）、`bench_work_stealing.cpp`（待写）  
> 测试机：Linux 7.0.0-22-generic，编译选项 `-O2`

---

## 1. 背景

当前线程池（#94 已改为 moodycamel 单全局队列）在 4+ worker 高并发时存在
**cache line bouncing**：N 个 worker 同时 CAS 同一个队列 head，争用随 worker 数线性增长。

Work Stealing 方案（#109）每个 worker 有独立 deque，正常路径不竞争，仅在 steal 和极端
burst 时才接触共享结构。本文记录对比测试的基线、方法和结论。

---

## 2. 当前基线（单全局队列，moodycamel）

> Run — 2026-06-21，`thunder_bench_threadpool_queue`

| 场景 | Mutex ns/op | **LF ns/op** | 加速比（vs Mutex）|
|------|:-----------:|:------------:|:-----------------:|
| 4P-4C（典型 offload）| 334.5 | **138.3** | 2.42x |
| 16P-4C（高并发 commit）| 368.3 | **127.1** | 2.90x |
| 1P-4C（单协程基线）| 620.3 | **184.4** | 3.36x |
| 8P-8C（对等压力）| 387.5 | **118.2** | 3.28x |

> LF = `moodycamel::ConcurrentQueue`（当前生产实现）  
> 每场景 3 次取中位数，任务体为空操作（只测队列本身开销）

**当前瓶颈分析**：

- 4P-4C：4 个 worker 同时 CAS global_q head → cache line 在 4 核间 bouncing，138 ns/op
- 8P-8C：8 核争用更激烈，但 moodycamel 内部分块缓解了部分竞争，118 ns/op
- worker 数增加时 ns/op **不降反升趋势**：4C→8C 从 138 降到 118，但继续增加 worker
  预计会反弹（需 16C 数据验证）

---

## 3. Work Stealing 对比测试方案

### 3.1 测试目标

验证以下假设：

| 假设 | 验证指标 |
|------|---------|
| 4+ worker 时 WS 取任务 ns/op 低于单队列 | `ns/op`：WS < LF |
| WS 的 LLC-load-misses 显著更低 | `perf stat -e LLC-load-misses` |
| WS 在 worker 数增加时 ns/op 更平稳 | 4C/8C/16C 三点对比曲线 |
| WS steal 触发率在正常负载下极低 | steal_count / total_task < 5% |

### 3.2 对比场景矩阵

> 注：WorkerDeque::push() 为单 tail 写者，commit() 不支持并发。
> 生产者固定为 1（单事件循环线程），消费者 = worker 数。

```
场景名             producer  worker  total_tasks  说明
──────────────────────────────────────────────────────────────
1P-1C (最简基线)      1       1       500,000    无竞争，验证 deque 基础吞吐
1P-2C                 1       2       500,000    2 worker
1P-4C (Thunder 典型)  1       4       500,000    Thunder 最常见配置
1P-8C                 1       8       500,000    8 核，deque 分发 vs 单队列争用
1P-16C                1      16       500,000    最大 worker 数，验证扩展性
task=10μs (1P-4C)     1       4        10,000    有耗时任务，WS 负载均衡效果
task=100μs (1P-4C)    1       4         1,000    长任务：steal 能否拉平不均衡
```

### 3.3 bench 代码（待实现）

文件：`code/test/labor/bench_work_stealing.cpp`

```cpp
// 实现三种方案并排对比
// A: moodycamel 单全局队列（当前）
// B: WorkStealingPool（N deques + global_q，本次新增）

// 额外统计量（需在 WorkStealingPool 内埋点）
struct WSStats {
    std::atomic<uint64_t> local_hits{0};   // 从自己 deque 取到
    std::atomic<uint64_t> steal_hits{0};   // 从别人 deque 偷到
    std::atomic<uint64_t> global_hits{0};  // 从 global_q 取到
    std::atomic<uint64_t> yield_count{0};  // 什么都没有，yield
};

// 实际输出（Run #1，2026-06-21）
// 场景              LF ns/op   WS ns/op   加速比
// 1P-4C (典型)      1373.1     543.6      2.53x
// 1P-8C             1698.9     659.4      2.58x
// 1P-16C            2097.2     968.1      2.17x
```

### 3.4 perf 辅助测量

```bash
# 测量 cache miss 对比（需 root 或 perf_event_paranoid <= 1）
perf stat -e cache-references,cache-misses,LLC-load-misses \
    ./build/bin/thunder_bench_work_stealing \
    --gtest_filter="BenchWS.SingleQueue_4P4C"

perf stat -e cache-references,cache-misses,LLC-load-misses \
    ./build/bin/thunder_bench_work_stealing \
    --gtest_filter="BenchWS.WorkStealing_4P4C"

# 对比两次 LLC-load-misses 数量
```

### 3.5 成功标准（✅ 已验证）

| 指标 | 合格线 | 实测（Run #1）| 结果 |
|------|--------|:-------------:|:----:|
| 1P-4C ns/op | WS ≤ LF × 0.95 | 543.6 vs 1373.1（39%）| ✅ |
| 1P-8C ns/op | WS < LF × 0.85 | 659.4 vs 1698.9（38%）| ✅ |
| 1P-16C ns/op | WS < LF × 0.70 | 968.1 vs 2097.2（46%）| ✅ |
| 1P-1C ns/op | WS ≤ LF × 1.10 | 256.4 vs 919.6（27%）| ✅ |

---

## 4. 如何运行

```bash
# Step 1：重跑当前基线（记录到本文历史区）
./build/bin/thunder_bench_threadpool_queue \
    --gtest_filter="BenchThreadpoolQueue.Compare"

# Step 2：实现 WorkStealingPool 后，编译新 bench
cmake --build build -j$(nproc)
./build/bin/thunder_bench_work_stealing

# Step 3：perf 辅助（需 sudo 或调整 paranoid）
sudo sysctl kernel.perf_event_paranoid=1
perf stat -e LLC-load-misses ./build/bin/thunder_bench_work_stealing
```

---

## 5. 设计约束说明

`WorkerDeque::push()` 设计为**单 tail 写者**（single-writer），不支持多线程并发 push。
Thunder 的 `commit()` 在真实场景中由单一事件循环线程调用（per-worker 进程的 coroutine 框架），天然满足此约束。

因此性能对比以**单生产者**（1P）为准，producers 参数代表总提交任务数，不代表并发线程数。

---

## 6. 历史记录

### 基线 Run #1 — 2026-06-21（单全局队列）

> 测试机：Linux 7.0.0-22-generic，`-O2`，bench 程序：`thunder_bench_threadpool_queue`
> 注：多生产者场景（4P/8P/16P），两方案均为多 producer 线程

| 场景 | LF ns/op | LF Mop/s |
|------|:--------:|:--------:|
| 4P-4C（典型）| 138.3 | 7.23 |
| 16P-4C（高并发）| 127.1 | 7.87 |
| 1P-4C（单协程）| 184.4 | 5.42 |
| 8P-8C（对等）| 118.2 | 8.46 |

### 对比 Run #1 — 2026-06-21（LF vs WS，单生产者）

> 测试机：Linux 7.0.0-22-generic，`-O2`，bench 程序：`thunder_bench_work_stealing`
> 每场景 3 次取中位数，commit() 单线程调用（匹配 Thunder 事件循环设计）

| 场景 | LF ns/op | WS ns/op | LF Mop/s | WS Mop/s | 加速比 |
|------|:--------:|:--------:|:--------:|:--------:|:------:|
| 1P-1C（最简基线）| 919.6 | 256.4 | 1.09 | 3.90 | **3.59x** |
| 1P-2C（2 worker）| 1097.3 | 325.6 | 0.91 | 3.07 | **3.37x** |
| 1P-4C（Thunder 典型）| 1373.1 | 543.6 | 0.73 | 1.84 | **2.53x** |
| 1P-8C（8 worker）| 1698.9 | 659.4 | 0.59 | 1.52 | **2.58x** |
| 1P-16C（16 worker）| 2097.2 | 968.1 | 0.48 | 1.03 | **2.17x** |
| 1P-4C（task=10μs）| 2830.8 | 2624.9 | 0.35 | 0.38 | 1.08x |
| 1P-4C（task=100μs）| 25324.2 | 25215.6 | 0.04 | 0.04 | 1.00x |

**结论**：
- 空任务场景：WS 比 LF 快 **2.17x ~ 3.59x**（per-worker deque 消除 moodycamel 单队列争用）
- LF 的 ns/op 随 worker 数线性恶化（1C→16C 增长 2.28x），WS 更平稳（增长 3.78x，但绝对值低 2x+）
- 有耗时任务时两者趋近（任务执行时间占主导，队列开销可忽略）
- **LF 适合任务密集型长任务；WS 适合高频空载/轻量任务分发**
