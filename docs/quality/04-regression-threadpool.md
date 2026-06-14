# 回归测试记录

> 关联：ThreadPool 全套改动（#92～#96）
> 日期：2026-06-14
> 用例总数每次运行追加到「运行记录」章节

---

## 1. 回归范围

### 影响域分析

本次改动涉及的文件：

| 文件 | 改动内容 | 影响模块 |
|------|---------|---------|
| `code/Util/src/thread/threadpool.h` | namespace util、ConcurrentQueue、背压、动态 resize | 所有使用线程池的模块 |
| `code/Net/include/coro/ThreadPoolAwaitable.hpp` | namespace 同步 + commit 异常 resume 修复 | 协程 offload 路径 |
| `code/Net/include/labor/WorkerThreadPool.hpp` | 新增 ResizeThunderWorkerThreadPool | 全局线程池管理 |
| `code/Net/src/labor/WorkerThreadPool.cpp` | 1 线程起步、resize 实现 | Worker 初始化 |
| `code/Net/src/labor/Worker.cpp` | 默认线程数 4→0(auto) | Worker 启动 |

### 测试套件

| 套件 | 用例数 | 覆盖内容 |
|------|:------:|---------|
| **ThreadPool** | 10 | 构造、commit、多任务、参数传递、void 任务、空闲计数、析构 join、并发、背压、动态 resize |
| **WssCodec** | 12 | TLS/WSS 编解码（依赖线程池 offload 路径） |
| **HttpCodec** | 45 | HTTP/HTTPS 编解码 |
| **Coroutine** | 15 | 协程基础机制（含 pool offload awaiter） |
| **StepCo20** | 7 | 步骤协程执行（通过 PoolOffloadAwaiter 使用线程池） |
| **Util Buffer** | 21 | 工具层缓冲区（回归基线，确保未受 includes 影响） |
| **Util Json** | 53 | 工具层 JSON（回归基线） |

---

## 2. 回归命令

```bash
# 全量编译
cd /home/tommychen/thunder
cmake build
cmake --build build -j$(nproc)

# 关键回归
./build/bin/thunder_test_util_threadpool
./build/bin/thunder_test_codec_wss
./build/bin/thunder_test_codec_http
./build/bin/thunder_test_coroutine
./build/bin/thunder_test_step_co20

# 基线回归（确保工具层无退化）
./build/bin/thunder_test_util_buffer
./build/bin/thunder_test_util_json

# Benchmark（非回归必需，但用于性能基线对比）
./build/bin/thunder_bench_threadpool_queue \
    --gtest_filter="BenchThreadpoolQueue.Compare"
```

---

## 3. 通过标准

- [ ] 所有测试用例 PASSED（而非 SKIPPED 或 FAILED）
- [ ] 编译 0 error
- [ ] ThreadPool 专用 10 用例全部通过
- [ ] 依赖线程池的 WssCodec（12）/ Coroutine（15）/ StepCo20（7）全部通过

---

## 4. 运行记录

> 每次回归在此追加。保留历史方便回溯。

---

### Run #1 — 2026-06-14

**改动内容**：#92（namespace 修复）+ #94（ConcurrentQueue）+ #95（unique_ptr）+ #96（背压）+ #93（动态 resize + 1 线程起步）

**编译**：0 error

| 套件 | 用例数 | 结果 | 备注 |
|------|:------:|:----:|------|
| ThreadPool | 10 | ✅ PASSED | 含新增 BackpressureQueueMax + ResizeDynamic |
| WssCodec | 12 | ✅ PASSED | 线程池使用路径正常 |
| HttpCodec | 45 | ✅ PASSED | 含 HttpsCodec |
| Coroutine | 15 | ✅ PASSED | 含协程 offload 路径 |
| StepCo20 | 7 | ✅ PASSED | 步骤协程执行 |
| Util Buffer | 21 | ✅ PASSED | 基线回归 |
| Util Json | 53 | ✅ PASSED | 基线回归 |
| **合计** | **163** | **✅ 全通过** | |

**结论**：回归通过，无退化。

---

### Run #2 — ⏳ 待执行
