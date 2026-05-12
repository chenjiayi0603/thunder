# Thunder io_uring 并发模型分析

> 日期: 2026-05-12 | 分支: dev | 分析范围: Net/src/labor 全部 I/O 后端实现

---

## 一、架构全景

Thunder 采用 **多进程 + 单线程事件循环** 的并发模型。每个 Worker/Manager 进程内部运行一个 libev 事件循环，I/O 后端通过统一抽象接口 `IoBackend` 接入。

```
┌─────────────────────────────────────────────────────────────────┐
│                         Thunder 进程架构                         │
├─────────────────────────────────────────────────────────────────┤
│  Manager 进程                    Worker 进程 (× N)               │
│  ┌───────────────────┐          ┌───────────────────┐           │
│  │   libev loop       │          │   libev loop       │           │
│  │   ┌─────────────┐  │          │   ┌─────────────┐  │           │
│  │   │  IoBackend   │  │          │   │  IoBackend   │  │           │
│  │   │  (独立实例)   │  │          │   │  (独立实例)   │  │           │
│  │   └─────────────┘  │          │   └─────────────┘  │           │
│  │   ev_io / ev_timer  │          │   ev_io / ev_timer  │           │
│  └───────────────────┘          └───────────────────┘           │
│         ↑ fork()                       ↑ fork()                 │
└─────────────────────────────────────────────────────────────────┘
```

**关键特性**:
- 每个进程拥有**独立的 io_uring 实例**（ring），不共享 SQ/CQ
- 所有 I/O 操作在**单一线程**内完成 — 无锁，无上下文切换
- io_uring 通过 ring_fd 集成到 libev 事件循环，完全复用现有 epoll 唤醒机制

---

## 二、三种 I/O 后端对比

### 2.1 EvIoBackend（epoll 基准）

```
应用层
  │ SubmitRead(fd, buf)
  ▼
ev_io_init(watcher, callback, fd, EV_READ)
ev_io_start(loop, watcher)
  │
  ▼  fd 就绪 (epoll_wait 返回)
IoEventCallback()
  ├─ EV_READ:  buf->ReadFD(fd)   →  syscall: read()   (1 次系统调用)
  └─ EV_WRITE: buf->WriteFD(fd)  →  syscall: write()  (1 次系统调用)
```

- 每个 fd 一个 `ev_io` watcher
- 每次读写 = `epoll_wait` 醒来 + 1 次 read/write 系统调用
- **优点**: 简单，调试方便
- **缺点**: 高并发时 epoll + read/write 各一次 syscall，开销翻倍

### 2.2 UringIoBackend（原始 io_uring）

```
应用层
  │ SubmitRead(fd, buf)
  ▼
io_uring_prep_recv(sqe, fd, buf, len, 0)   ← 填 SQE
io_uring_sqe_set_data(sqe, user_data)
io_uring_submit(&ring)                      ← 提交批量 SQE（1 次系统调用）
  │
  ▼  内核完成 I/O → CQE 写入完成队列
ring_fd 可读 (epoll 通知 libev)
  │
RingEventCallback()
  │
ReapCqes()
  ├─ io_uring_peek_cqe()    → 取 CQE（无系统调用，共享内存）
  ├─ buf->AdvanceWriteIndex(result)
  ├─ m_callback(fd, seq, IoOp::Read, result)
  └─ io_uring_cqe_seen()    → 标记 CQE 已消费
```

| 维度 | EvIoBackend | UringIoBackend |
|------|-------------|----------------|
| 读路径系统调用 | epoll_wait + read (2次) | io_uring_submit + ring_fd 唤醒 (可批量) |
| 写路径 | **同步** `WriteFD()` | **同步** `WriteFD()`（未用 io_uring 写） |
| CQE 收割 | N/A | 共享内存零拷贝，批量处理 (最多 32/次) |
| 队列深度 | N/A | 256 |
| 适用场景 | 通用 | 读密集型，减少 syscall |

**写路径说明**: `SubmitWrite()` 故意保持同步。源码注释指出异步写会与 TLS/Codec 状态机产生不必要的往返开销，小数据量的 buffered send 在内核中本质上是同步完成的。

### 2.3 AsioUringIoBackend（ASIO + io_uring）

```
应用层
  │ SubmitRead(fd, buf)
  ▼
sock.async_read_some(buffer, callback)     ← ASIO 封装，内部用 io_uring
  │
  ▼  ASIO io_context 内部处理
  │  ├─ 填 SQE → io_uring_submit
  │  └─ CQE → 触发 lambda 回调
  ▼
callback(ec, n)
  ├─ buf->AdvanceWriteIndex(n)
  └─ m_callback(fd, seq, Read, n)
```

**与 libev 的三路集成**（全部在同一线程）:

```
libev 事件循环
  │
  ├─ ev_prepare  ──── 每次 epoll_wait 前 ──▶ io_context.poll()  收割 CQE
  ├─ ev_check    ──── 每次 epoll_wait 后 ──▶ io_context.poll()  收割 CQE
  └─ ev_io(ring_fd) ─ ring_fd 可读    ──▶ io_context.poll()  收割 CQE
```

| 维度 | UringIoBackend | AsioUringIoBackend |
|------|----------------|---------------------|
| 读路径 | `io_uring_prep_recv` 直接操作 | `async_read_some` ASIO 封装 |
| 写路径 | **同步** `WriteFD()` | **异步** `async_write_some` |
| 集成方式 | 监听 ring_fd → ReapCqes | 三路 poll (prepare/check/ring_fd) |
| 队列深度 | 256 | ASIO 内部管理 |
| 复杂度 | 轻量，head-only liburing | 依赖 ASIO standalone 库 |
| 当前状态 | `THUNDER_IO_URING=OFF` (默认关闭) | `THUNDER_IO_ASIO_URING=ON` (默认开启) |

---

## 三、并发模型深入

### 3.1 单线程事件循环

```
┌─────────────── Worker 进程（单线程） ───────────────────────┐
│                                                             │
│   while (running) {                                         │
│     ev_run(m_loop, 0);  // libev 主循环                     │
│       │                                                     │
│       ├─ ev_prepare    → io_context.poll()  [asio_uring]    │
│       ├─ epoll_wait    → 等待 fd / timer / ring_fd 就绪     │
│       ├─ ev_check      → io_context.poll()  [asio_uring]    │
│       │                                                     │
│       ├─ ev_io 回调:                                        │
│       │   ├─ ring_fd 就绪  → ReapCqes()      [uring]       │
│       │   ├─ 业务 fd 就绪  → HandleIoRead()   [ev/epoll]   │
│       │   └─ 业务 fd 可写  → HandleIoWrite()  [ev/epoll]   │
│       │                                                     │
│       ├─ ev_timer 回调:                                     │
│       │   ├─ IoTimeout (心跳/超时检测)                      │
│       │   ├─ StepTimeout (协程超时)                         │
│       │   └─ SessionTimeout (会话超时)                      │
│       │                                                     │
│       └─ ev_idle 回调: 空闲时执行延迟任务                    │
│   }                                                         │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

**关键结论**:
- **无并发竞争**: 所有 I/O 回调、定时器、协程恢复都在同一线程执行
- **无锁设计**: `m_mapPending`、`m_fds` 等数据结构不需要加锁
- **非抢占**: 回调之间不会互相打断，类似 Node.js 的事件循环模型

### 3.2 io_uring 上下文隔离

```
Worker-0 (pid 1001)          Worker-1 (pid 1002)          Manager (pid 1000)
┌──────────────────┐        ┌──────────────────┐        ┌──────────────────┐
│ io_uring ring_0  │        │ io_uring ring_1  │        │ io_uring ring_m  │
│ SQ ──▶ CQ        │        │ SQ ──▶ CQ        │        │ SQ ──▶ CQ        │
│ fd=10, fd=12..   │        │ fd=20, fd=22..   │        │ fd=5 (listen)    │
└──────────────────┘        └──────────────────┘        └──────────────────┘
```

- fork 后每个子进程的 io_uring 实例完全独立
- `ev_loop_fork(m_loop)` 确保 fork 后 libev 正确重新初始化 epoll
- Manager 的 ring_fd **不会**被子进程继承（源码在 fork 前做了保护）

### 3.3 I/O 提交流程

```
HandleIoReadComplete()
  │  解析完一个完整消息
  │  需要继续读下一个
  ▼
SubmitRead(fd, pRecvBuff, seq)
  │
  ├─ [ev]     ev_io_set(fd, EV_READ)  → epoll_ctl 注册
  │                                     → 下次 epoll_wait 返回
  │                                     → IoEventCallback → ReadFD()
  │
  ├─ [uring]  io_uring_prep_recv(sqe, fd, buf)
  │           io_uring_submit(&ring)   → 1 次系统调用提交 SQ
  │                                     → 内核完成 → CQE
  │                                     → ring_fd 可读
  │                                     → RingEventCallback → ReapCqes()
  │
  └─ [asio]   sock.async_read_some(buf, callback)
              → ASIO 内部 io_uring submit
              → io_context.poll() 收割 CQE
              → callback 被调用
```

### 3.4 RemoveIoWriteEvent 中的读事件补交

这是一个关键的并发安全点：

```cpp
// Worker.cpp RemoveIoWriteEvent
m_pIoBackend->CancelFd(pConn->iFd);  // 移除 fd 所有事件
// 补交读 — 因为 CancelFd 销毁了所有 I/O 事件（含 EV_READ）
pConn->pRecvBuff->Compact(8192);
pConn->pRecvBuff->EnsureWritableBytes(8192);
m_pIoBackend->SubmitRead(pConn->iFd, pConn->pRecvBuff.get(), pConn->ulSeq);
```

- `CancelFd()` 在 uring 路径只移除 `m_mapPending` 中的条目
- 在 ev 路径会 `ev_io_stop` 并销毁 watcher
- 两种情况下都必须补交 `SubmitRead()`，否则 fd 永久失去读监听

---

## 四、配置与编译

### 4.1 CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `THUNDER_IO_URING` | OFF | 启用原始 io_uring 后端 |
| `THUNDER_IO_ASIO_URING` | ON | 启用 ASIO + io_uring 后端 |

```bash
cmake -S . -B build -DTHUNDER_IO_URING=ON -DTHUNDER_IO_ASIO_URING=ON
```

### 4.2 运行时配置

在节点 JSON 配置中指定：

```json
{
    "io_backend": "asio_uring"   // 可选: "ev", "uring", "asio_uring"
}
```

如果不指定，默认兜底为 `ev`（`EvIoBackend`）。

### 4.3 后端选择顺序（Labor::InitIoBackend）

```
配置 "asio_uring"?
  ├─ 是 → 尝试 AsioUringIoBackend::Init()
  │         ├─ 成功 → 使用 asio_uring
  │         └─ 失败 → fallback 到 uring
  │
  ├─ 否 / asio_uring 失败 → 配置 "uring"?
  │         ├─ 是 → 尝试 UringIoBackend::Init()
  │         │         ├─ 成功 → 使用 uring
  │         │         └─ 失败 → fallback 到 ev
  │         │
  │         └─ 否 → EvIoBackend::Init() (最终兜底)
  │
  └─ 最终: 总是回到 ev（绝不因后端不可用而拒绝启动）
```

---

## 五、性能特征分析

### 5.1 系统调用对比

| 场景 | ev (epoll) | uring | asio_uring |
|------|-----------|-------|------------|
| 单次读 | epoll_wait + read (2) | submit + ring_fd 唤醒 (2, 但可批量) | submit + poll (2, 可批量) |
| 批量读 (32 fd) | 32×epoll_wait + 32×read | 1×submit + ring_fd 批量 CQE | 1×submit + poll 批量 CQE |
| 单次写 | write (1) | WriteFD (1, 同步) | async_write (submit+回调) |
| CQE 收割 | N/A | 共享内存, 0 syscall | poll, 0 syscall |

### 5.2 适用场景

| 后端 | 最佳场景 |
|------|---------|
| `ev` | 通用场景，调试友好，兼容性最好 |
| `uring` | 读密集型（大量并发连接读），减少 syscall |
| `asio_uring` | 读写均衡场景，希望全部异步化 |

### 5.3 限制与注意

1. **io_uring 需要 Linux 5.1+** (`IORING_FEAT_FAST_POLL` 需 5.5+)
2. **单线程模型**: io_uring 在本架构中不改变并发模型 — 仍然是单线程事件循环
3. **uring 写路径是同步的**: `UringIoBackend::SubmitWrite()` 直接调 `WriteFD()`，不是真正的异步写
4. **队列深度 256**: 高并发时需确保 SQE 不被耗尽（`io_uring_get_sqe` 返回 NULL）
5. **ring_fd 数量**: 每个进程一个 ring，不存在 ring 膨胀问题

---

## 六、代码结构

```
code/Net/
├── include/labor/
│   └── IoBackend.hpp              # 抽象接口定义
├── src/labor/
│   ├── EvIoBackend.{hpp,cpp}      # epoll 基准实现
│   ├── UringIoBackend.{hpp,cpp}   # 原始 io_uring (liburing)
│   ├── AsioUringIoBackend.{hpp,cpp} # ASIO + io_uring
│   ├── Labor.cpp                  # InitIoBackend() 选择逻辑
│   ├── Manager.cpp                # Manager 侧 RemoveIoWriteEvent
│   └── Worker.cpp                 # Worker 侧 RemoveIoWriteEvent
```

---

## 七、总结

Thunder 的 io_uring 集成遵循 **"最小侵入"原则**:

1. **不改变并发模型**: 仍然是单线程事件循环，io_uring 只是换了一种 syscall 方式
2. **透明替换**: 通过 `IoBackend` 抽象，上层代码 (`Worker`, `Manager`) 完全无感
3. **渐进式采用**: 默认 `asio_uring=ON`，但可随时退回到 `ev`
4. **保持简单**: 每个进程一个 ring，无共享，无锁，无额外线程


## 附录 A：三路 CQE 收割机制深度分析

### A.1 libev ev_run 主循环时序

```c
// libev ev.c:3535 — ev_run() 核心循环
do {
    // ══════════ 阶段 0: PREPARE ══════════
    queue_events(prepares, preparecnt, EV_PREPARE); // 收集所有 ev_prepare watcher
    EV_INVOKE_PENDING;                               // 立即执行它们的回调
    // → 此时 AsioUringIoBackend::OnPrepare() 被调用
    // → 内部执行 io_context.poll()，收割一轮 CQE

    // ══════════ 阶段 1: 内核状态同步 ══════════
    fd_reify();  // 将 libev 内部的 fd 变更同步到 epoll (EPOLL_CTL_ADD/DEL/MOD)

    // ══════════ 阶段 2: 阻塞等待 ══════════
    waittime = 计算最近的定时器到期时间;
    backend_poll(waittime);  // → epoll_wait(epoll_fd, events, max, timeout)
    // 线程在此阻塞，直到:
    //   a) 定时器到期
    //   b) 任何注册的 fd 就绪 (包括 ring_fd)
    //   c) 被信号中断

    // ══════════ 阶段 3: 处理定时器 ══════════
    timers_reify();     // 收集到期定时器
    periodics_reify();  // 收集到期周期任务
    idle_reify();       // 无其他事件时收集 idle watcher

    // ══════════ 阶段 4: CHECK ══════════
    queue_events(checks, checkcnt, EV_CHECK); // 收集所有 ev_check watcher
    // → 但还未执行 (下面 EV_INVOKE_PENDING 统一执行)

    // ══════════ 阶段 5: 统一执行所有待处理回调 ══════════
    EV_INVOKE_PENDING;
    // 执行顺序: check watcher → io watcher → timer watcher → idle
    // → 此时执行 OnCheck() 和 OnRingReady()
    // → OnCheck() 内部执行 io_context.poll()，收割 CQE
    // → OnRingReady() 内部执行 io_context.poll()，收割 CQE

} while (有活跃的 watcher && 未请求停止);
```

### A.2 三路收割的时序图

```
时间 →

libev 线程:
  │
  ├─ OnPrepare()           ← ev_prepare 回调
  │   └─ io_context.poll()
  │       收割: CQE_A, CQE_B (在线程阻塞前到达的)
  │
  ├─ epoll_wait(timeout)   ← 线程在此阻塞
  │   │
  │   │  内核侧:
  │   │    io_uring 完成 I/O → 写入 CQE_C 到共享内存
  │   │    → ring_fd 变为可读
  │   │    → epoll 检测到 ring_fd 就绪
  │   │
  │   └─ epoll_wait 返回 (因为 ring_fd 可读)
  │
  ├─ 处理其他就绪 fd (业务逻辑)
  │
  ├─ OnCheck()             ← ev_check 回调
  │   └─ io_context.poll()
  │       收割: CQE_C (阻塞期间完成的)
  │
  ├─ OnRingReady()          ← ev_io(ring_fd) 回调
  │   └─ io_context.poll()
  │       收割: CQE_D (处理其他事件期间新到达的)
  │
  └─ 下一轮循环...
```

### A.3 为什么三路都必须在主线程？

**根本原因：Thunder 没有 I/O 工作线程。**

```
┌──────────────────────────────────────────────────────────────┐
│            Thunder 单线程模型 vs 多线程模型                     │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  Thunder (实际):                                              │
│  ┌─────────────────────────────────────────────────────┐     │
│  │  主线程                                              │     │
│  │  ev_run() {                                         │     │
│  │    prepare → poll() → epoll_wait → poll() → poll()  │     │
│  │  }                                                  │     │
│  │  ▲ 所有 CQE 在此收割，所有回调在此执行                  │     │
│  └─────────────────────────────────────────────────────┘     │
│                                                              │
│  多线程模型 (未采用):                                          │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐   │
│  │  主线程       │    │  I/O 线程 1   │    │  I/O 线程 N   │   │
│  │  ev_run()    │◄───│  io_uring    │    │  io_uring    │   │
│  │  业务逻辑    │ 队列 │  submit/get  │    │  submit/get  │   │
│  └──────────────┘    └──────────────┘    └──────────────┘   │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

**为什么选择单线程？**

| 维度 | 单线程 (Thunder) | 多线程 |
|------|-----------------|--------|
| 锁开销 | **零** — `m_fds`、`m_mapPending` 无需加锁 | 需要细粒度锁或 lock-free 队列 |
| 上下文切换 | **零** — 回调之间无抢占 | 线程间切换 ≈ 1-10µs/次 |
| 缓存局部性 | **极好** — 所有数据在单核 cache | 跨核 cache line bouncing |
| 调试 | 简单 — 单调用栈 | 困难 — 多线程交织 |
| CPU 利用率 | 限定单核（但有 N 个 Worker 进程） | 可利用多核（但带来同步开销） |

Thunder 通过 **fork N 个 Worker 进程** 来利用多核，每个进程内部保持单线程简单性。
这本质上是一种 **shared-nothing** 架构 — 比多线程更易推理，比纯单进程有更好的水平扩展性。

### A.4 三路设计的必要性

如果没有三路收割，会出现以下问题：

**只保留 ring_fd ev_io（去掉 prepare/check）**:
```
问题 1: 提交 SQE 后、epoll_wait 前，CQE 已经到达
  → ring_fd 在 epoll_wait 内部已经可读，但 libev 不知道
  → 必须等下一次 epoll_wait 返回才能处理
  → 延迟增加一个完整的 epoll_wait 周期

问题 2: epoll_wait 返回后有多个 fd 就绪（ring_fd + 业务 fd）
  → libev 先处理业务 fd 的回调
  → 业务回调中可能提交新的 SQE
  → 这些 SQE 立即完成，CQE 到达
  → 但 ring_fd 的回调排在后面，ring_fd 仍然是可读状态
  → CQE 堆积，在下一次事件循环迭代才被收割
```

**三路协同的效果**:

```
提交 SQE 后的时间线:
  submit ─┬─ prepare 收割 (立即)    ← 消除"提交后、epoll_wait 前"的盲区
          ├─ epoll_wait 阻塞
          │   └─ 内核完成 → ring_fd 就绪 → epoll_wait 返回
          ├─ check 收割 (立即)       ← 消除"epoll_wait 返回后、回调前"的盲区
          └─ ring_fd 回调收割        ← 处理回调期间新到达的
```

### A.5 完整数据流追踪

以下追踪一个完整的 `SubmitRead` → 回调 全路径：

```
1. Worker 调用 SubmitRead(fd=10, buf, seq=5)
   │
   ├─ [AsioUringIoBackend::SubmitRead]
   │   sock.async_read_some(buffer, lambda_callback)
   │   ASIO 内部: io_uring_prep_recv(sqe, fd=10, ...)
   │              io_uring_submit(&ring)
   │              返回 (未阻塞)
   │
2. 内核处理:
   │   fd=10 数据到达
   │   io_uring 内核模块: 拷贝数据到 buf
   │   写入 CQE {user_data=X, res=128}
   │   更新 ring_fd → 可读
   │
3. libev 感知:
   │
   ├─ 如果此时在 ev_prepare 阶段:
   │   OnPrepare() → io_context.poll()
   │   → ASIO 内部收割 CQE
   │   → lambda_callback(ec, 128) 被调用 ← 在主线程!
   │   → buf->AdvanceWriteIndex(128)
   │   → m_callback(10, 5, IoOp::Read, 128, m_userData)
   │
   ├─ 如果在 epoll_wait 阻塞期间:
   │   ring_fd 变为可读 → epoll_wait 返回
   │   → 定时器处理后 → EV_INVOKE_PENDING
   │   → OnRingReady() → io_context.poll()
   │   → lambda_callback(...) ← 仍在主线程!
   │
   └─ 如果在处理其他事件期间:
       → OnCheck() → io_context.poll()
       → lambda_callback(...) ← 仍在主线程!

关键保证: lambda_callback 永远在调用 ev_run() 的线程上执行。
ASIO 的 io_context::poll() 是同步语义，不会内部起线程。
```


## 附录 B：为什么 CQE 全在主线程 — libev + ASIO 单线程保证的完整证明

### B.1 libev 侧的保证

libev 的事件循环 `ev_run()` 是严格的**单线程执行**：

```c
// libev ev.c:3535
int ev_run(EV_P_ int flags) {
    do {
        queue_events(prepares, preparecnt, EV_PREPARE);
        EV_INVOKE_PENDING;          // ← ① 执行所有 ev_prepare 回调 (OnPrepare)

        fd_reify();                 // ← 同步 epoll fd 集合到内核
        backend_poll(waittime);     // ← epoll_wait() 阻塞
        timers_reify();             // ← 收集到期定时器

        queue_events(checks, checkcnt, EV_CHECK);
        EV_INVOKE_PENDING;          // ← ② 执行 ev_check (OnCheck) + ev_io (OnRingReady) + ...
    } while (活跃);
}
```

所有 `EV_INVOKE_PENDING` 调用都在 **调用 `ev_run()` 的同一线程** 上执行回调。libev 内部没有线程池，不创建线程。

### B.2 Thunder 侧的调用链

```
Worker 进程 (单线程)
  │
  ev_run(m_loop, 0)  ← 主线程在此循环
  │
  ├─ EV_INVOKE_PENDING ①
  │   └─ OnPrepare()                            [AsioUringIoBackend.cpp:214]
  │       └─ m_ioCtx.poll()                     ← 同步调用，当前线程
  │           └─ scheduler::poll()               ← ASIO 调度器，当前线程
  │               └─ select_reactor 检查 eventfd
  │                   └─ eventfd 可读 → event_fd_read_op::do_perform()
  │                       └─ run(0, ops)
  │                           └─ io_uring_peek_cqe()  ← 零 syscall，共享内存读 CQE
  │                               └─ op->complete()  ← 用户 lambda 回调
  │
  ├─ epoll_wait(m_epoll_fd, timeout)
  │   │  ← 线程在此阻塞
  │   │  ring_fd 就绪 (io_uring 有 CQE)
  │   └─ epoll_wait 返回
  │
  ├─ EV_INVOKE_PENDING ②
  │   ├─ OnCheck()                               [AsioUringIoBackend.cpp:219]
  │   │   └─ m_ioCtx.poll()  ← 同上，同步调用，当前线程
  │   │
  │   └─ OnRingReady()                           [AsioUringIoBackend.cpp:224]
  │       └─ m_ioCtx.poll()  ← 同上，同步调用，当前线程
  │
  └─ 回到循环开头
```

**关键断言**: `m_ioCtx.poll()` 永远在调用 `ev_run()` 的线程上执行。

### B.3 ASIO 侧的保证

**`io_context::poll()` 的线程语义**:

```cpp
// asio/impl/io_context.ipp:76
io_context::count_type io_context::poll() {
    // impl_ = scheduler, 直接在当前线程运行
    count_type s = impl_.poll(ec);
    // scheduler::poll() 在调用线程执行所有就绪的 handler
    return s;
}
```

`io_context::poll()` **不创建线程**。它同步地在调用线程上处理所有就绪的异步操作完成回调。

**`io_context::run()` vs `io_context::poll()`**:

```cpp
run()       // 阻塞直到所有工作完成，可被多线程并发调用
poll()      // 非阻塞：运行已就绪的 handler 然后返回，始终在调用线程
```

Thunder 只使用 `poll()`，保证了单线程语义。

### B.4 双 fd 唤醒机制

io_uring 完成事件通过 **两个文件描述符** 通知用户态：

```
io_uring 实例
  ├─ ring_fd     (io_uring 原生 fd，由 io_uring_queue_init 创建)
  │   └─ 被 libev ev_io 监控 → OnRingReady → io_context.poll()
  │
  └─ event_fd    (ASIO 内部创建，由 io_uring_register_eventfd 注册)
      └─ 被 ASIO select_reactor 监控 → event_fd_read_op → run() → CQE 收割
```

**ring_fd 唤醒路径** (外部，libev 层):
```
用户代码 submit SQE
  → io_uring_submit()
  → 内核完成 → CQE 写入 → ring_fd 变为可读
  → epoll_wait 检测到 ring_fd 就绪
  → libev 触发 ev_io 回调 → OnRingReady()
  → io_context.poll()
  → 内部收割 CQE
```

**eventfd 唤醒路径** (内部，ASIO 层):
```
io_context.poll() 被调用 (无论来自哪个路径)
  → scheduler::poll()
  → select_reactor 检查所有注册的 fd
  → 发现 eventfd 可读 (io_uring 已写入)
  → event_fd_read_op::do_perform()
      ├─ read(eventfd) 清空计数器
      └─ run(0, ops)
          └─ io_uring_peek_cqe(&ring, &cqe)  // 零 syscall，共享内存
              └─ 结果 → op->complete() → 用户 lambda 回调
```

### B.5 为什么需要三路？

```
时间线:  ←──────── 一次 ev_run 迭代 ────────→

          prepare        epoll_wait          check + ring_fd
          ──────── ─────────────────────── ──────────────────
          
场景 A: CQE 在 prepare 阶段到达
  → OnPrepare 收割 ✅  (否则延迟到下一次 epoll_wait 后)

场景 B: CQE 在 epoll_wait 阻塞期间到达  
  → ring_fd 唤醒 epoll_wait
  → OnCheck/OnRingReady 收割 ✅  (否则延迟到下一次迭代)

场景 C: CQE 在处理其他事件时到达 (timer / 业务逻辑回调中)
  → OnCheck 收割 ✅  (处理完当前批次再收割，避免重入)

场景 D: CQE 在 ev_prepare 和 epoll_wait 之间到达 (竞争窗口)
  → 此时 epoll_wait 已调用但尚未阻塞
  → ring_fd 已在 epoll fd 集合中，epoll_wait 立即返回
  → OnCheck/OnRingReady 收割 ✅
```

**仅保留 ring_fd (去掉 prepare/check) 的问题**:
```
迭代 N:
  epoll_wait → ring_fd → OnRingReady → io_context.poll() → 收割 3 个 CQE
  处理业务回调 → 回调中间接提交新 SQE
  → 内核立即完成 → CQE 到达
  → 但迭代 N 已经过了 ring_fd 处理阶段
  → 依赖下一次迭代 N+1 的 epoll_wait 才能察觉 ring_fd 可读
  → 额外延迟 = 一次完整的 epoll_wait timeout (可能数百微秒到毫秒)
```

### B.6 线程安全总结表

| 数据结构 | 访问者 | 需要锁? |
|---------|--------|---------|
| `AsioUringIoBackend::m_fds` | 仅 ev_run 线程 | ❌ 不需要 |
| `AsioUringIoBackend::m_ioCtx` | 仅 ev_run 线程调用 poll() | ❌ 不需要 |
| `UringIoBackend::m_mapPending` | 仅 ev_run 线程 | ❌ 不需要 |
| `io_uring SQ/CQ` (共享内存) | 内核 + 用户态 单线程 | ❌ 不需要 (内核侧原子操作) |
| `Worker::m_mapFdData` (ev 后端) | 仅 ev_run 线程 | ❌ 不需要 |
| `Manager::m_mapFdData` (ev 后端) | 仅 ev_run 线程 | ❌ 不需要 |

**ASIO 内部的锁** (io_uring_service):
- `mutex_`: 保护 SQ 提交和内部状态。**即使单线程也会获取**，因为 ASIO 设计为支持多线程 `run()`。
- `registration_mutex_`: 保护 io_object 注册表。
- `io_object::mutex_`: 保护单个 io_object 的操作队列。

这些锁在单线程场景下 **从不竞争**（始终立即获得），仅增加极微小的原子操作开销。

### B.7 单线程模型的边界与代价

**优点**:
1. 零上下文切换 — 回调之间无抢占
2. 零数据竞争 — 所有数据结构天然线程安全
3. 缓存友好 — 数据保持在 L1/L2 cache 中
4. 可预测 — 回调执行顺序确定

**代价**:
1. **一个慢回调阻塞一切**: 如果某个业务回调执行时间过长，所有 I/O 停止处理
2. **单核限制**: 每个 Worker 进程只能用一个 CPU 核（通过 fork N 个 Worker 来横向扩展）
3. **epoll_wait 阻塞期间的 CQE 需要等待**: 这就是为什么需要 ring_fd 来唤醒 epoll_wait

**Thunder 的应对策略**:
1. 协程 (`StepCo20`) 将长任务拆分为多个步骤，通过 `co_await` 让出执行权
2. 多进程架构 (`process_num: N`) 利用多核，每个进程内保持单线程简单性
3. Worker 的 `io_timeout` 定时器和心跳检测确保连接不会被长期阻塞
