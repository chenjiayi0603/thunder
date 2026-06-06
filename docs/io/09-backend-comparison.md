# IoBackend 四后端对比

> 代码: `code/Net/src/labor/{Ev,NativeUring,AsioUring,Dpdk}IoBackend.{cpp,hpp}`
> 接口: `code/Net/include/labor/IoBackend.hpp`

---


## 1. 性能实测 (wrk HTTP 全链路, 2026-06-06)

**测试**: wrk -t4 -c100 -d5s, POST Echo, Hello 服务

**指标说明**:
- **QPS**: 每秒完成的请求数 (越高越好)
- **p50 时延**: 50% 请求的响应时间 ≤ 此值 (中位数, 衡量典型延迟)
- **p99 时延**: 99% 请求的响应时间 ≤ 此值 (长尾, 衡量最慢的 1%)
- **吞吐量**: 每秒传输的数据量

| backend | 100 conn QPS | 500 conn QPS | p50 时延 | p99 时延 | 吞吐量 |
|---------|------------|------------|---------|---------|--------|
| **ev (epoll)** | **109,574** | **102,357** | 0.79 ms | 2.12 ms | 68.1 MB/s |
| asio_uring | 108,000 | 82,707 | 0.88 ms | 18.5 ms | 67.5 MB/s |
| native_uring | 89,791 | 80,001 | 0.60 ms | 5.7 ms | 55.8 MB/s |

> p50 时延 = 50% 请求的响应时间 ≤ 此值 (中位数)
> p99 时延 = 99% 请求的响应时间 ≤ 此值 (长尾)

### 为什么是这个排名

### 不同包大小对比

| body | ev QPS | ev p50 | asio_uring QPS | asio_uring p50 | native_uring QPS | native_uring p50 |
|------|--------|--------|---------------|---------------|-----------------|-----------------|
| 空(18B) | **109,574** | 0.79 ms | 108,000 | 0.88 ms | 89,791 | 0.60 ms |
| 1KB | 58,598 | 2.81 ms | **71,465** | 1.28 ms | 67,678 | 13.91 ms |
| 4KB | 23,083 | 4.48 ms | **39,326** | 0.93 ms | 23,763 | 1.20 ms |

**结论变了**:

- 空body: ev 第一 (109K), 但 asio_uring 几乎持平 (108K)
- **1KB body: asio_uring 反超** (71K vs ev 58K, 快 22%)
- **4KB body: asio_uring 大幅领先** (39K vs ev 23K, 快 70%)

**大包时 io_uring 赢**。原因: 包越大, epoll 的 read/write syscall 开销越大(数据拷贝), io_uring 的批量提交 + 零拷贝优势越明显。

### 排名分析（按开销从小到大）:

1. **ev**: 每次 I/O 就是 epoll_wait + read/write。没有额外的调度层,没有 ring buffer 管理,**内核里跑了几十年,极致优化**。

2. **asio_uring**: 每次 poll 要多走一层 ASIO 调度(遍历 FdState → 构造 SQE → 写 SQ ring → io_uring_enter)。100 连接时,这层调度**比那几十个 syscall 还贵**——io_uring 省了 syscall, 但加的调度代码更贵。p99 延迟 18.5ms 长尾来自 ring_fd 空唤醒。

3. **native_uring**: 手写的 ring buffer 管理,无 ASIO 的对象池和编译器优化。每次手动操作 SQ tail/CQ head 指针,比 ASIO 的优化版本更慢。

**io_uring 的真正场景**: 不是 100 连接, 是 **10000 连接**。那时 epoll 要 10000 次 read, io_uring 1 次 enter——批量优势才体现。当前规模**不该用 io_uring**。


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

