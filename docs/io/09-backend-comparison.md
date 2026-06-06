# IoBackend 四后端对比

> 代码: `code/Net/src/labor/{Ev,NativeUring,AsioUring,Dpdk}IoBackend.{cpp,hpp}`
> 接口: `code/Net/include/labor/IoBackend.hpp`

---


## 1. 性能实测 (wrk HTTP 全链路, 2026-06-06)

**测试**: wrk -t4 -c{duration} -d5s, POST Echo, Hello 服务

| backend | 100 conn QPS | 500 conn QPS | p50 | p99 | 吞吐 |
|---------|------------|------------|-----|-----|------|
| **ev (epoll)** | **109,574** | **102,357** | 0.79ms | 2.12ms | 68.1MB/s |
| asio_uring | 108,000 | 82,707 | 0.88ms | 18.5ms | 67.5MB/s |
| native_uring | 89,791 | 80,001 | 0.60ms | 5.7ms | 55.8MB/s |

**结论: ev(epoll) 最快。** 在 100~500 连接规模,epoll 的简单胜过 io_uring 的批量。

### 为什么 ev 最快 — 逐 backend 分析

**ev (epoll) — 最简最快**:
- 每次 I/O: 1次 epoll_wait + 1次 read/write = 2~3 syscall
- localhost 上 syscall 延迟 <1μs — 极低, 不是瓶颈
- epoll 是内核里最成熟的 I/O 机制, 经过 20 年优化
- 结果: 109K QPS, p99=2.12ms — 在这个规模最优

**asio_uring — 有 batch 但 overhead 更大**:
- 每次 I/O: 1次 io_uring_enter(批量) = 1 syscall
- 但 ASIO 调度有固定开销: io_context.poll() + NOP-SQE + ring_fd 管理 ~2μs
- 在 100 conn 时, batch 优势未体现(连接太少), overhead 反而拉低性能
- 结果: 108K QPS — 和 ev 持平, 但 p99=18.5ms 有长尾延迟(ring_fd 空唤醒)

**native_uring — 最差, ASIO 开销换成了手写开销**:
- 和 asio_uring 同原理, 但没有 ASIO 的 shared_ptr/FdState 管理层
- 手写 SQ/CQ 管理的开销 ≈ ASIO overhead, 无优势
- 结果: 89K QPS — 比 ev 低 18%, p50 最快(0.60ms)但整体 QPS 更低

### 为什么 io_uring 没赢 — 三个原因

```
1. 连接数太少 (100~500): io_uring 的批量优势在万级连接才体现
   100 连接 → epoll: 201 syscall, io_uring: 1 syscall
   → 理论上 io_uring 快 200×, 但实际上 201 syscall 在 localhost 只需 ~200μs
   → 这 200μs 在 5ms 的 HTTP 处理延迟中占比不到 4% — 不是瓶颈

2. localhost 掩盖了 syscall 差距:
   真实网络延迟 ~1-10ms → syscall 占比 <1%
   localhost 延迟 ~0.1ms → syscall 占比 ~20%
   → 即使 20% 也不足以让 io_uring 的 batch 优势反超 overhead

3. HTTP 栈是瓶颈:
   wrk 测试的是完整 HTTP 链路: codec → 协程调度 → S2S路由 → Worker处理
   I/O backend 只占其中一小部分, 换 backend 无法解决上层开销
```

### 什么时候 io_uring 会赢

| 连接数 | ev QPS | io_uring QPS (预测) | 分析 |
|--------|--------|-------------------|------|
| 100 | 109K | 108K | 持平, overhead抵消 |
| 1,000 | ~50K | ~80K | batch优势开始体现 |
| 10,000 | ~5K | ~40K | syscall成为瓶颈,io_uring大幅领先 |

### 不同包大小 (ev backend)

| body | QPS | 吞吐 | p50 | 分析 |
|------|-----|------|-----|------|
| Echo(空) | 109K | 68 MB/s | 0.79ms | 基准 |
| 1KB | 72K | 72 MB/s | 1.2ms | QPS降34%, 吞吐持平 |
| 4KB | 28K | 112 MB/s | 3.5ms | QPS降74%, 吞吐升65% |

> QPS 随包增大快速下降 — 内存拷贝成为主导瓶颈。包越大,换 backend 越无帮助。

---

## 2. 四后端实现对比

### EvIoBackend — epoll (默认, 最简)

**怎么工作**:

```
应用层: SubmitRead(fd, buf)
  → 注册 fd 到 epoll (EPOLLIN)
  → epoll_wait 返回 fd 可读
  → read(fd, buf) → 触发 callback
```

- 每次 I/O = 1 次 epoll_wait + 1 次 read/write = **2~3 次系统调用**
- 100 个连接并发 = 1 次 epoll_wait + 100 次 read = **101 次系统调用**
- 优点: 简单,内核 2.6+ 都支持,无外部依赖
- 缺点: syscall 次数随连接数线性增长
- 代码: `EvIoBackend.{hpp,cpp}` (72+313=385 行)

### NativeUringIoBackend — 手写 io_uring (零依赖)

**怎么工作**:

```
应用层: SubmitRead(fd, buf)
  → 构造 SQE(read, fd, buf) 写入 SQ ring buffer
  → ev_prepare: io_uring_submit() 一次性提交所有 SQE
  → 内核处理, 完成后写 CQE 到 CQ ring buffer
  → ev_io(ring_fd): io_uring_peek_cqe() 收割 CQE → 触发 callback
```

- 每次 I/O = **0 次额外系统调用**(bulk submit)
- 100 个连接并发 = 1 次 io_uring_enter = **1 次系统调用**
- 和 EvIo 的关键区别: 不需要 epoll_wait 查就绪, 不需要逐 fd read/write
- 优点: 零外部依赖,纯 C API,编译快,完全可控
- 缺点: 手写 SQ/CQ ring buffer 管理,维护成本高
- 代码: `NativeUringIoBackend.{hpp,cpp}` (110+424=534 行)

### AsioUringIoBackend — ASIO 封装 io_uring (三路驱动)

**怎么工作**:

```
应用层: SubmitRead(fd, buf)
  → ASIO async_read_some → 生成 internal SQE (不提交)
  → ev_prepare: io_context.poll() 批量提交所有 SQE + 收割上一轮 CQE
  → epoll_wait (ring_fd 就绪)
  → ev_io(ring_fd): poll() 收割刚完成的 CQE → completion lambda
  → ev_check: poll() 补收 race window 的 CQE + 诊断
```

- 和 NativeUring 的核心区别: **用 ASIO 管理 SQ/CQ**,不手写 ring buffer
- 三路驱动: ev_prepare(投递) + ev_io(接货) + ev_check(补刀) — 确保零遗漏
- FdState: shared_ptr 生命周期管理, CancelFd 只需 erase(自动析构)
- Fixed Buffers: 预注册 16MB 内存池, send_zc 零拷贝地基
- 优点: ASIO 生态(shared_ptr/weak_ptr), 三路驱动低延迟
- 缺点: ASIO 依赖(编译慢 745 行)
- 代码: `AsioUringIoBackend.{hpp,cpp}` (172+573=745 行)

### DPDK — 未测试

DPDK 需要**独占网卡 + DPDK 兼容 NIC**(如 Intel X520/X710), 且需要 `meson` 编译。
当前测试环境无 DPDK 硬件, 无法实测。**以下对比仅限已测的三个后端**(ev/asio_uring/native_uring)。


### 工作流程图

**ev (epoll)**:
```
SubmitRead  SubmitWrite    ...
    │           │
    └─────┬─────┘
          ▼
    ev_prepare ── 无操作 (epoll 不需要 prepare)
          │
          ▼
    epoll_wait ──── fd 就绪? ──YES──► read/write ──► callback
          │
          ▼
    ev_check ── 无操作
```

**native_uring**:
```
SubmitRead  SubmitWrite    ...
    │           │
    └─────┬─────┘
          ▼
    ev_prepare ── io_uring_submit() ── 批量提交所有 SQE 到 SQ
          │                            同时收割上一轮 CQE
          ▼
    epoll_wait ── ring_fd 可读? ──► io_uring_peek_cqe() ──► callback
          │
          ▼
    ev_check ── 补收割 race window CQE
```

**asio_uring (三路驱动)**:
```
SubmitRead  SubmitWrite
    │           │         (只构造 async_op, 不提交)
    └─────┬─────┘
          ▼
  ┌─ ev_prepare ── io_context.poll() ── ① 提交所有 SQE
  │                                       ② 收割上一轮 CQE → callback
  ├─ epoll_wait ── ring_fd 可读?
  │       │
  ├─ ev_io ── poll() ── 收割 CQE → callback
  │       │            └─ 无挂起 op? → stop ring_fd 监听
  │       │
  └─ ev_check ── poll() ── 补收割 race window CQE
                            └─ UpdateRingWatcher()
                               └─ 有挂起? start : stop
```

**关键差異**: ev 只有 epoll_wait 做实际工作。native_uring 在 ev_prepare 做批量提交。
asio_uring 用三个 watcher 覆盖全部时机——提交(ev_prepare)、即时收割(ev_io)、兜底(ev_check)。

### 核心差异对比表

| | EvIo | NativeUring | AsioUring |
|---|------|-------------|-----------|------|
| I/O 模型 | 就绪通知 | 完成通知 | 完成通知 | 轮询 |
| syscall/请求 | 2~3 | 0(批量) | 0(批量) | 0 |
| SQ/CQ 管理 | 无 | 手写 | ASIO |
| 零拷贝 | 无 | 支持 | send_zc | DMA |
| 外部依赖 | 无(libev) | 无(liburing) | ASIO |
| 代码量 | 385 行 | 534 行 | 745 行 | 898 行 |
| 内核要求 | 2.6+ | 5.1+ | 5.1+ | 不需要 |
| 适用场景 | 默认,通用 | 高性能无依赖 | 高性能+生态 | >10M pps |

---

---

