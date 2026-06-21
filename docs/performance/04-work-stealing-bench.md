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

```
场景名               生产者  消费者  tasks/P    说明
──────────────────────────────────────────────────────
1P-4C (单协程基线)      1      4    1,000,000  WS 优势最小，验证无退化
4P-4C (典型 offload)    4      4      250,000  Thunder 最常见配置
4P-8C (双倍 worker)     4      8      250,000  8 核时 cache bouncing 明显
4P-16C (16 worker)      4     16      125,000  极端 worker 数，验证扩展性
8P-8C (对等压力)        8      8      125,000  生产者也多时的争用
任务耗时 10μs (4P-4C)   4      4       50,000  真实任务有耗时，WS 负载均衡效果
任务耗时 100μs (4P-4C)  4      4        5,000  长任务：steal 能否拉平不均衡
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

// 输出格式（目标）
// 场景            LF ns/op   WS ns/op   加速比   steal率   global率
// 4P-4C           138.3      ???        ???x      ???%      ???%
// 4P-8C           ???        ???        ???x      ???%      ???%
// 4P-16C          ???        ???        ???x      ???%      ???%
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

### 3.5 成功标准

| 指标 | 合格线 |
|------|--------|
| 4P-4C ns/op | WS ≤ LF × 1.05（允许 5% 以内退化）|
| 4P-8C ns/op | WS < LF × 0.85（至少 15% 提升）|
| 4P-16C ns/op | WS < LF × 0.70（至少 30% 提升）|
| steal 触发率 | < 10%（正常负载下本地命中为主）|
| LLC-load-misses | WS < LF × 0.60（减少 40%+）|
| 1P-4C ns/op | WS ≤ LF × 1.10（单 worker 不能明显退化）|

> 若 4P-4C 场景 WS 反而更慢（超过 5% 退化），说明 deque 额外路径代价超过竞争消除收益，
> 需重新评估是否实施。

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

## 5. 历史记录

### 基线 Run #1 — 2026-06-21（单全局队列，实施前）

| 场景 | LF ns/op | LF Mop/s |
|------|:--------:|:--------:|
| 4P-4C（典型）| 138.3 | 7.23 |
| 16P-4C（高并发）| 127.1 | 7.87 |
| 1P-4C（单协程）| 184.4 | 5.42 |
| 8P-8C（对等）| 118.2 | 8.46 |

> Work Stealing 实施后，在此追加对比数据。
