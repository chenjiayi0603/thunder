# 静态代码阅读 vs Profiler

> 写作动机：#94 ThreadPool 锁竞争问题是靠"看代码"发现的，不是跑工具测出来的。  
> 这两种发现问题的方式性质完全不同，值得区分清楚。

---

## 1. 静态代码阅读

### 是什么

"静态"= 代码**不运行**，只看文本。  
用眼睛（或 LSP 工具辅助）读源码，根据代码结构**推断**在某种场景下会发生什么。

### 发现 #94 的完整过程

**Step 1：找到入口**

```bash
grep -rn "ThunderWorkerThreadPool" code/Net/
# → code/Net/include/coro/ThreadPoolAwaitable.hpp 调用 pool->commit(...)
```

**Step 2：跳到 commit() 实现**

用 LSP `goToDefinition` 跳到 `code/Util/src/thread/threadpool.h`：

```cpp
template<class F, class... Args>
auto commit(F&& f, Args&&... args) -> future<...> {
    lock_guard<mutex> lock{_lock};        // ← 注意这行
    _tasks.emplace([task]{ (*task)(); });
    _task_cv.notify_one();
    return future;
}
```

**Step 3：推断并发场景**

脑子里模拟 100 个协程同时 `co_await MakePoolOffloadAwaiter(...)` 时发生什么：

```
协程 A: commit() → lock_guard{_lock} ← 拿到锁，入队
协程 B: commit() → 等 _lock ...
协程 C: commit() → 等 _lock ...
协程 D: commit() → 等 _lock ...
         ↑ 全部排队，串行入队
```

`_lock` 只有一把，所有入队操作都要轮流等它。线程越多、同时提交越多，等待时间越长。

**Step 4：记录为待验证问题**

这只是推断，不是测量结果，所以标 🟡（低优先级），等压测数据确认再处理。

---

### 静态阅读能发现什么

| 可以发现 | 发现不了 |
|---------|---------|
| 设计缺陷（UB、单锁串行、裸指针泄漏）| 实际慢多少（需要测量） |
| 代码逻辑错误（条件写反、边界越界）| 这个路径是否高频（需要 profiler）|
| 安全漏洞（未校验输入、缓冲区溢出）| 锁竞争是否实际发生（可能根本没并发）|
| 架构问题（`namespace std` UB）| 内存占用实际值 |

**静态阅读给的是"嫌疑人"，不是"证据"。**

---

## 2. Profiler（性能分析器）

### 是什么

让程序**真实运行**，同时记录"哪段代码占用了多少时间/内存"。  
输出是有数字的报告，而不是推断。

### 常见类型

#### 2.1 采样型 Profiler（最常用）

每隔固定时间（如 1ms）"拍一张快照"，记录当前调用栈，跑完后统计哪些函数被拍到最多次。

Thunder 可用工具：

```bash
# Linux perf（系统级，无需修改代码）
perf record -g ./deploy/thunder_hello
perf report --sort=dso,sym

# 或生成火焰图（Brendan Gregg FlameGraph）
perf script | stackcollapse-perf.pl | flamegraph.pl > out.svg
```

输出示例（火焰图 / perf report）：

```
Overhead  Symbol
  31.2%   std::mutex::lock()          ← 31% 的 CPU 时间在等锁
  18.4%   SSL_read()
  12.1%   epoll_wait()
   8.3%   net::WssCodec::Decode()
```

如果 `std::mutex::lock()` 出现在前三，说明 #94 是真实瓶颈，值得优化。  
如果排在第 20 位占 0.3%，那 #94 根本不是问题，不用动。

#### 2.2 插桩型 Profiler

在代码里加计时点，精确测量指定函数。

```cpp
// 手动插桩示例
auto t0 = std::chrono::steady_clock::now();
pool.commit(work);
auto t1 = std::chrono::steady_clock::now();
// 记录 (t1 - t0) 到直方图
```

精确但有开销，通常在怀疑某个具体路径时用。

#### 2.3 内存 Profiler（ASan / Valgrind）

跟踪每次 `new`/`delete`，程序退出时报告未释放的内存。  
`docs/quality/01-asan-lsan.md` 记录的就是这类。

```
==ERROR: LeakSanitizer: detected memory leaks
Direct leak of 4096 byte(s) allocated from:
    #1 SSL_CTX_new
    #2 net::HttpsCodec::EnsureState
```

---

## 3. 两者的正确配合顺序

```
静态阅读                     Profiler
  │                            │
  ├─ 发现"可能的问题"          ├─ 跑压测，生成火焰图
  ├─ 记录到 issus-list.md      ├─ 确认"确实慢"
  └─ 标优先级（低/中/高）       └─ 决定要不要改
                                │
                           只有 Profiler 确认之后
                           才值得花时间优化
```

**反模式**：看到代码有锁就立刻换 lock-free 队列。  
有时锁根本不是瓶颈，换了之后性能没变化，还引入了新的复杂度。

**Thunder 对 #94 的实际处理**：  
1. 静态阅读发现 `mutex` 全局锁（2026-06-14）
2. 写 benchmark 实测对比（`MutexQueue vs LockFreeQueue`）：确认 **2.5x~3.7x 加速比**
3. 认为提升足够大，不等火焰图，直接实施替换 + namespace 修复 + 裸指针修复
4. 单元测试 8/8 通过，全量 build 0 error  
→ 详情见 `docs/performance/03-threadpool-queue-bench.md`

---

## 4. Thunder 里如何跑 Profiler

### 前置条件

CMake 四种构建模式对比：

| 模式 | 编译参数 | 优化 | 符号表 | 适用场景 |
|------|---------|:----:|:------:|---------|
| `Debug` | `-O0 -g` | ❌ 无 | ✅ 有 | 单步调试、gdb |
| `Release` | `-O3` | ✅ 最强 | ❌ **无** | 生产部署 |
| **`RelWithDebInfo`** | **`-O2 -g`** | ✅ 中等 | ✅ **有** | **Profiler 首选** |
| `MinSizeRel` | `-Os` | ✅ 体积优先 | ❌ 无 | 嵌入式/镜像瘦身 |

"符号表"= 函数名 + 行号信息。没有它，perf 输出的是内存地址：

```
# Release（无符号表）→ 看不出是什么
31.2%   0x00007f8a3b2c4d10

# RelWithDebInfo（有符号表）→ 一眼看出是锁
31.2%   std::mutex::lock()
```

为什么不用 `Debug` 跑 profiler：`-O0` 完全不优化，程序比生产慢 3~5 倍，
测出来的热点不代表真实场景。`RelWithDebInfo` 用 `-O2`，行为接近生产，同时保留函数名。

> ⚠️ `-O2` 会内联小函数，火焰图里内联掉的函数不会单独出现，只看到它的调用方。这是 profiling 的已知局限。

```bash
cmake -B build_perf -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build_perf -j$(nproc)
```

### 采样压测流程

```bash
# 1. 启动服务（RelWithDebInfo 构建）
./deploy.sh up

# 2. wrk 施压（触发线程池 offload 路径）
wrk -t4 -c100 -d30s http://127.0.0.1:27006/hello/pool_cpu

# 3. 同时采样
sudo perf record -g -p $(pgrep robot_W0) -- sleep 30

# 4. 生成报告
sudo perf report --sort=dso,sym | head -30
```

### 判断 #94 是否真实瓶颈

```bash
# 从 perf report 过滤 mutex 相关
sudo perf report --sort=sym | grep -i mutex
```

- 占比 > 5%：值得换 lock-free 队列
- 占比 < 1%：不优化，维持现状

---

## 5. 总结

| 维度 | 静态代码阅读 | Profiler |
|------|:----------:|:--------:|
| 运行代码？ | ❌ 不运行 | ✅ 必须运行 |
| 发现时机 | 写代码时、review 时 | 有真实负载时 |
| 结果 | "可能有问题" | "确实慢了 X ms" |
| 成本 | 低（读代码即可）| 高（需要压测环境）|
| 误报率 | 高（嫌疑人≠罪犯）| 低（数字说话）|
| Thunder 中用途 | 找 #92~#96 这类设计缺陷 | 确认优先级、决策是否改 |
