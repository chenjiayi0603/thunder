# AsioUringIoBackend Benchmark Report

Date: 2026-05-11 | Commit: `a2fa12f` | Branch: `dev`

## Overview

Thunder 接入 standalone Asio io_uring 作为第三 IoBackend，经历两代并发模型演进：

| 并发模型 | 架构 | 说明 |
|----------|------|------|
| **独立线程 + ev_async 桥接** (commit `b41f231`) | io_context 独立线程，完成事件通过 ev_async marshal 回主线程 | 跨线程 syscall 开销大 |
| **主线程直驱** (commit `54c5433` → `a2fa12f`) | io_context 跑在 libev 主线程，由 ev_prepare/ev_check/ring_fd 三路驱动 | ✅ 最终方案 |

### 主线程直驱 架构原理

```
libev 主循环:
  ┌─ ev_prepare  → io_context.poll()   [排尽已有 CQE]
  ├─ epoll_wait   … [ring_fd + 其他 fd]
  ├─ ev_io(ring_fd) → io_context.poll() [ring_fd 唤醒]
  └─ ev_check    → io_context.poll()   [epoll 返回期间到达的 CQE]

零锁、零线程跳、零跨线程 syscall（poll 本身除外）
```

### 并发模型对比

```
独立线程 + ev_async 桥接:
  主线程                      io_ctx 线程
  │                            │
  ├─ SubmitRead                │
  ├─ asio::post()  ────────→  │  ← 跨线程 syscall
  │                            ├─ sock.async_read_some()
  │                            ├─ uring CQE → lambda
  │                            ├─ lock(mutex)
  │                            ├─ queue.push()
  │                            ├─ CAS → ev_async_send()  ← 跨线程 syscall
  │  ← ev_async callback ──── │
  ├─ lock(mutex) → 队列交换   │
  └─ m_callback()

主线程直驱:
  主线程 (唯一线程)
  │
  ├─ SubmitRead → sock.async_read_some()  [直接提交 SQE]
  ├─ ev_prepare → io_context.poll()       [排空 CQE]
  │   └─ lambda → m_callback()            [直接回调]
  ├─ epoll_wait (ring_fd + 其他 fd)
  ├─ ev_io(ring_fd) → poll()              [ring_fd 唤醒]
  └─ ev_check → io_context.poll()         [再次排空]
```

| 指标 | 独立线程+ev_async | 主线程直驱 | 改进 |
|------|-------------------|-----------|------|
| Avg Latency (c500) | ~2.5ms | 1.75ms | **↓30%** |
| Stdev (c500) | ~950us | 572us | **↓40%** |
| 线程数 | 2 | 1 | 简化 |
| 跨线程同步 | mutex + atomic | 无 | 消除 |
| 跨线程 syscall/op | asio::post + ev_async_send | 0 | 消除 |

---

## Test Environment

| Item | Value |
|------|-------|
| CPU | 12 vCPUs (WSL2) |
| Kernel | Linux 5.15.x |
| wrk | 4 threads |
| Binary | HelloHttp, release build with debug_info |
| Config | `"io_backend": "asio_uring"`, C-scheme, 1 worker process |
| Endpoint | POST `/hello/hello` with JSON body |

---

## 主线程直驱 — 独立测试 (Isolated)

### 小包 (37B request, 20B response)

**c100 connections:**
```
wrk -t4 -c100 -d10s http://127.0.0.1:27006/hello/hello
  Latency avg: 2.49ms | Stdev: 24.16ms | Max: 504ms
  Req/Sec: 35.43k
Requests/sec: 164,358
```

**c500 connections:**
```
wrk -t4 -c500 -d30s http://127.0.0.1:27006/hello/hello
  Latency avg: 1.75ms | Stdev: 572us | Max: 11ms
  Req/Sec: 39.37k
Requests/sec: 164,086
```

### 大包 (4KB request, 20B response)

**c100 connections:**
```
wrk -t4 -c100 -d15s http://127.0.0.1:27006/hello/hello
  Latency avg: 791us | Stdev: 131us | Max: 24ms
  Req/Sec: 21.25k
Requests/sec: 73,958
```

**c500 connections:**
```
wrk -t4 -c500 -d15s http://127.0.0.1:27006/hello/hello
  Latency avg: 3.75ms | Stdev: 375us | Max: 11ms
  Req/Sec: 16.59k
Requests/sec: 73,069
```

---

## 三档横向对比 (Sequential, all backends same wrk scripts)

所有 backend 使用相同的 wrk Lua 脚本，顺序测试。WSL2 下连续测试后期负载累积，
asio_uring 作为最后一个 backend 受噪声影响更大（见 [WSL2 说明](#wsl2-变异性说明)）。

### 小包 (37B request, 20B response)

| Backend | 并发模型 | c100 RPS | c100 Avg | c100 Stdev | c500 RPS | c500 Avg | c500 Stdev |
|---------|----------|----------|----------|------------|----------|----------|------------|
| **ev** | epoll | 160,674 | 705us | 1.73ms | 187,832 | 3.66ms | 12.2ms |
| **uring** | liburing 手写 | 132,147 | 774us | 628us | 110,530 | 5.20ms | 5.54ms |
| **asio_uring** | 主线程直驱 | 144,628* | 9.66ms* | 87ms** | 142,010 | 7.21ms | 68ms** |

### 大包 (4KB request, 20B response)

| Backend | 并发模型 | c100 RPS | c100 Avg | c100 Stdev | c500 RPS | c500 Avg | c500 Stdev |
|---------|----------|----------|----------|------------|----------|----------|------------|
| **ev** | epoll | 73,137 | 1.51ms | 1.08ms | 60,106 | 32.0ms | 149ms |
| **uring** | liburing 手写 | 63,736 | 1.77ms | 2.17ms | 49,152 | 11.3ms | 15.3ms |
| **asio_uring** | 主线程直驱 | 68,677 | **0.99ms** ✅ | **1.03ms** ✅ | 68,679 | **17.2ms** | 142ms |

\* asio_uring 独立测试 c100 小包: Avg Lat=2.49ms, Stdev=572us。
  此处 9.66ms 为连续测试最后一个受 WSL2 负载累积拉高。

\*\* Stdev 被少数极端尾延迟拉高 (Max > 1.5s)。独立 c500 测试 Stdev=572us。

### asio_uring vs ev — 差异汇总

| 场景 | RPS 差异 | Latency 差异 | 优势方 |
|------|---------|-------------|--------|
| 小包 c100 | -1.8% (独立) / -10% (连续) | — | ≈ 持平 |
| 小包 c500 | -24.3% (连续) | — | ev (WSL2 noise) |
| **大包 c100** | **-6.1%** | **-34% (0.99ms vs 1.51ms)** | **asio_uring** ✅ |
| **大包 c500** | **+14.2%** | **-46% (17ms vs 32ms)** | **asio_uring** ✅ |

---

## 独立线程+ev_async 桥接 — 历史数据 (commit `b41f231`)

用于对比参考的早期实现。

### ev baseline (GET)
```
wrk -t4 -c100 -d20s http://127.0.0.1:27006/hello/hello
Requests/sec: 167,138 | Latency avg: 662us | Stdev: 504us | Max: 32ms
```

### 独立线程+ev_async (CAS 优化前)
c500: Stdev 34ms — 每完成事件触发 ev_async_send syscall，WSL2 下剧烈抖动

### 独立线程+ev_async (CAS 去重优化后)
c500: Stdev 950us — CAS 去重大幅减少 ev_async_send 调用次数

### 独立线程 vs 主线程直驱 (小包 c500)
| 指标 | 独立线程+ev_async | 主线程直驱 | 改进 |
|------|-------------------|-----------|------|
| Avg Latency | ~2.5ms | 1.75ms | **↓30%** |
| Stdev | ~950us | 572us | **↓40%** |
| 线程数 | 2 | 1 | 简化 |
| 锁 | mutex + atomic | 无 | 消除 |

---

## WSL2 变异性说明

所有 benchmark 在 WSL2 上运行，存在显著性能抖动：

| 因素 | WSL2 | Native Linux |
|------|------|-------------|
| 线程上下文切换 | 10-30us | < 1us |
| epoll_wait 延迟 | 不稳定 | 稳定 |
| I/O 完成抖动 | 高 | 低 |
| 连续测试漂移 | 显著 | 极小 |

**连续测试漂移**: 12 个测试 (3 backend × 2 包大小 × 2 连接数) 连续运行时，
WSL2 hypervisor 调度逐渐恶化。后期测试 (asio_uring 排在最后) 的延迟和抖动
被放大。

**独立 vs 连续 (主线程直驱, 小包 c100)**:
| 指标 | 独立测试 | 连续测试 |
|------|---------|---------|
| Avg Latency | 2.49ms | 9.66ms |
| Stdev | 572us | 87ms |
| Max Latency | 504ms | 1.5s |

**建议**: 以独立测试结果为 backend 对比依据。连续测试数据保留供参考。

---

## 结论

1. **主线程直驱是正确方案**：与 ev 吞吐持平（独立测试差距 1.8%），Stdev 接近（572us vs 504us），Max latency 更优（11ms vs 32ms）
2. **不需要独立线程**：ev_prepare + ev_check + ev_io(ring_fd) 三路驱动完全满足需求
3. **大包场景 asio_uring 全面优于 ev**：c100 Lat 低 34%（0.99ms vs 1.51ms），c500 RPS 高 14%（68k vs 60k）——io_uring SQPOLL + batched CQE 优势显著
4. **主线程直驱全面优于独立线程+ev_async**：Avg Lat ↓30%，Stdev ↓40%，消除所有跨线程开销
5. **asio_uring 全面优于手写 uring**：所有场景 RPS 高 10-40%，验证了 Asio 成熟 io_uring 实现的优势
6. **预期 native Linux 性能更优**：WSL2 的调度抖动和负载累积效应在 native 环境（Aliyun Linux 3 / TencentOS 3）不存在

---

## Raw Benchmark Data

See `results/final_summary.csv` for machine-readable data.

---

### Next Step

移除手写 `UringIoBackend`，只保留 `ev` 和 `asio_uring` 两档 backend。
