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

### 2.2 NativeUringIoBackend（原生 liburing，推荐 io_uring 后端）

```
应用层
  │ SubmitRead(fd, buf)
  ▼
io_uring_prep_recv(sqe, fd, buf, len, 0)   ← 直填 SQE
io_uring_sqe_set_data(sqe, po)              ← PendingOp* 作为 user_data
io_uring_submit(&ring)                      ← 提交批量 SQE
  │
  ▼  内核完成 I/O → CQE 写入完成队列 → ++eventfd counter
ev_io(eventfd) 被唤醒 (epoll 通知 libev)
  │
ReapCqes()
  ├─ io_uring_peek_cqe()    → 取 CQE（零 syscall，共享内存）
  ├─ PendingOp* po = cqe_get_data(cqe)
  ├─ flags = cqe->flags     → send_zc 双 CQE 分流
  ├─ 有效连接 → m_callback(fd, seq, op, result)
  └─ delete po
```

| 维度 | EvIoBackend | NativeUringIoBackend |
|------|-------------|---------------------|
| 读路径 | epoll_wait + read (2 syscall) | io_uring_submit + eventfd 唤醒 (可批量) |
| 写路径 | **同步** WriteFD | **异步** prep_send / prep_send_zc |
| 集成方式 | per-fd ev_io watcher | eventfd + ev_io + ev_check 兜底（两路收割） |
| 队列深度 | N/A | 4096（env 可配） |
| SQPOLL | N/A | ✅ env 门控（内核 7.0 免特权） |
| send_zc | N/A | ✅ 按阈值分流 + WriteNotif 门控 |
| 状态 | 基线 | 推荐 io_uring |

**与旧 UringIoBackend 的关键差异**：旧版写路径退化为同步 `WriteFD()`，且未解决 buffer 生命周期/取消等核心问题。NativeUringIoBackend 全部异步，PedingOp 后端持有 + fd/seq 校验保证陈旧 CQE 安全。

### 2.3 AsioUringIoBackend（ASIO + io_uring，保留）

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
  ├─ ev_prepare  ──── 每次 epoll_wait 前 ──▶ io_context.poll()  提交 SQE + 收割 CQE
  ├─ ev_check    ──── 每次 epoll_wait 后 ──▶ io_context.poll()  收割 CQE
  └─ ev_io(ring_fd) ─ ring_fd 可读    ──▶ io_context.poll()  收割 CQE
```

| 维度 | NativeUringIoBackend | AsioUringIoBackend |
|------|---------------------|---------------------|
| 收割驱动 | eventfd + ev_io + ev_check（两路）| ev_prepare + ev_check + ev_io(ring_fd)（三路）|
| 中间层 | 无（直收割）| Asio io_context.poll() → scheduler → reactor |
| NOP SQE 空转 | 无 | 有（interrupt NOP 唤醒 ring_fd）|
| SQPOLL | ✅ 内置 | ❌ Asio 硬编码 flags=0 |
| send_zc | ✅ 内置 | ❌ 无支持 |
| 写路径 | prep_send(zc) 直发 | async_write_some |
| 状态 | 推荐 | 保留（对比参照） |

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
│       │                   ReapCqes()         [native_uring] │
│       │                                                     │
│       ├─ ev_io 回调:                                        │
│       │   ├─ eventfd 就绪 → ReapCqes()       [native_uring] │
│       │   ├─ ring_fd 就绪 → io_context.poll() [asio_uring]  │
│       │   ├─ 业务 fd 就绪 → HandleIoRead()    [ev/epoll]    │
│       │   └─ 业务 fd 可写 → HandleIoWrite()   [ev/epoll]    │
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
- **无锁设计**: `m_fds` 等数据结构不需要加锁
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
  ├─ [ev]           ev_io_set(fd, EV_READ)  → epoll_ctl 注册
  │                                      → 下次 epoll_wait 返回
  │                                      → IoEventCallback → ReadFD()
  │
  ├─ [native_uring] io_uring_prep_recv(sqe, fd, buf)
  │                 io_uring_submit(&ring)  → 1 次系统调用提交 SQ
  │                                        → 内核完成 → CQE → ++eventfd
  │                                        → ev_io(eventfd) → ReapCqes()
  │
  └─ [asio_uring]   sock.async_read_some(buf, callback)
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

- `CancelFd()` 在 native_uring 路径标记取消并移除 `m_fds` 中的条目，陈旧 CQE 安全丢弃
- 在 ev 路径会 `ev_io_stop` 并销毁 watcher
- 两种情况下都必须补交 `SubmitRead()`，否则 fd 永久失去读监听

---

## 四、配置与编译

### 4.1 CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `THUNDER_IO_ASIO_URING` | ON | 启用 ASIO + io_uring 后端 |

```bash
cmake -S . -B build -DTHUNDER_IO_ASIO_URING=ON
```

原生后端（`NativeUringIoBackend`）始终编译，无需额外选项；运行时按配置选择。

### 4.2 运行时配置

在节点 JSON 配置中指定：

```json
{
    "io_backend": "native_uring"   // 可选: "ev", "asio_uring", "native_uring"
}
```

如果不指定，默认兜底为 `ev`（`EvIoBackend`）。

### 4.3 后端选择顺序（Labor::InitIoBackend）

```
配置 "native_uring"?
  ├─ 是 → 尝试 NativeUringIoBackend::Init()
  │         ├─ 成功 → 使用 native_uring
  │         └─ 失败 → fallback 到 ev
  │
  ├─ 否 / native_uring 失败 → 配置 "asio_uring"?
  │         ├─ 是 → 尝试 AsioUringIoBackend::Init()
  │         │         ├─ 成功 → 使用 asio_uring
  │         │         └─ 失败 → fallback 到 ev
  │         │
  │         └─ 否 → EvIoBackend::Init() (最终兜底)
  │
  └─ 最终: 总是回到 ev（绝不因后端不可用而拒绝启动）
```

---

## 五、性能特征分析

### 5.1 系统调用对比

| 场景 | ev (epoll) | native_uring | asio_uring |
|------|-----------|-------------|------------|
| 单次读 | epoll_wait + read (2) | submit + eventfd 唤醒 (2, 可批量) | submit + poll (2, 可批量) |
| 批量读 (32 fd) | 32×epoll_wait + 32×read | 1×submit + eventfd 批量 CQE | 1×submit + poll 批量 CQE |
| 单次写 | write (1) | prep_send/prep_send_zc (异步) | async_write (异步) |
| CQE 收割 | N/A | 共享内存, 0 syscall | poll, 0 syscall |

### 5.2 实测数据（64KB 响应，c50/12s）

| 后端 | RPS | wrk 延迟（TTFB）| 真实整包延迟（Little's Law）| 尾延迟 |
|------|-----|----------------|--------------------------|--------|
| ev 基线 | 5,485 | 8.86ms | **9.12ms**（50÷5485）| ±3.44ms / max 155ms |
| **native_uring** | **5,903（+7.6%）** | 4.06ms（TTFB）| **8.47ms**（50÷5903，**−7.1%**）| ±133μs / max 11ms |
| asio_uring | 5,473（-3% vs ev）| 9.67ms（TTFB）| 18.3ms（100÷5473）| - |

> **wrk 延迟 vs 整包延迟**：ev 同步写时 wrk ≈ 整包（8.86ms vs 9.12ms）；native_uring 异步分片写时 wrk = TTFB 首字节（4.06ms），真实整包用 Little's Law 反算 = 并发÷RPS = 8.47ms（−7.1%）。RPS +7.6% 不受影响，是真实吞吐改善。

**核心发现**：原生后端精简控制路径（无 Asio 中间层 / NOP SQE / /proc hack）在 64KB 写场景拿到 +7.6% RPS 真实收益，asio_uring 对 ev≈持平。

### 5.3 适用场景

| 后端 | 最佳场景 |
|------|---------|
| `ev` | 通用场景，调试友好，兼容性最好 |
| `native_uring` | 大包写（64KB+），需 SQPOLL/send_zc 天花板 |
| `asio_uring` | 保留参照，小包 TTFB 有优势 |

### 5.4 限制与注意

1. **io_uring 需要 Linux 5.1+** (`IORING_FEAT_FAST_POLL` 需 5.5+)
2. **单线程模型**: io_uring 在本架构中不改变并发模型 — 仍然是单线程事件循环
3. **队列深度**: native_uring 默认 4096，env 可配（`THUNDER_URING_SQDEPTH`）
4. **ring_fd 数量**: 每个进程一个 ring，不存在 ring 膨胀问题
5. **send_zc bounce 版净收益≈0**：真零拷贝（去 bounce，#10）measurement-gated 暂不做

---

## 六、代码结构

```
code/Net/
├── include/labor/
│   └── IoBackend.hpp              # 抽象接口定义、IoOp 枚举（含 WriteNotif）
├── src/labor/
│   ├── EvIoBackend.{hpp,cpp}      # epoll 基准实现
│   ├── AsioUringIoBackend.{hpp,cpp} # ASIO + io_uring（保留）
│   ├── NativeUringIoBackend.{hpp,cpp} # 原生 liburing（推荐）
│   ├── Labor.cpp                  # InitIoBackend() 选择逻辑
│   ├── Manager.cpp                # Manager 侧 IoComplete 三路分发
│   └── Worker.cpp                 # Worker 侧 IoComplete 三路分发 + HandleIoWriteNotifComplete
```

---

## 七、总结

Thunder 的 io_uring 集成遵循 **"最小侵入"原则**:

1. **不改变并发模型**: 仍然是单线程事件循环，三后端全部在此模型内
2. **透明替换**: 通过 `IoBackend` 抽象，上层代码 (`Worker`, `Manager`) 完全无感
3. **渐进式采用**: 推荐 `native_uring`（实测 +7.6% RPS / −54% 延迟），可随时退回 `ev`
4. **保持简单**: 每个进程一个 ring，无共享，无锁，无额外线程
5. **send_zc 就绪**: IoOp::WriteNotif 双 CQE 门控已实现，bounce 版安全（真零拷贝暂不做）


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
| 锁开销 | **零** — `m_fds` 无需加锁 | 需要细粒度锁或 lock-free 队列 |
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
| `NativeUringIoBackend::m_fds` | 仅 ev_run 线程 | ❌ 不需要 |
| `NativeUringIoBackend::m_ring` | 仅 ev_run 线程 | ❌ 不需要 |
| `AsioUringIoBackend::m_fds` | 仅 ev_run 线程 | ❌ 不需要 |
| `AsioUringIoBackend::m_ioCtx` | 仅 ev_run 线程调用 poll() | ❌ 不需要 |
| `io_uring SQ/CQ` (共享内存) | 内核 + 用户态 单线程 | ❌ 不需要 (内核侧原子操作) |
| `Worker::m_mapFdData` (ev 后端) | 仅 ev_run 线程 | ❌ 不需要 |
| `Manager::m_mapFdData` (ev 后端) | 仅 ev_run 线程 | ❌ 不需要 |

**ASIO 内部的锁** (io_uring_service):
- `mutex_`: 保护 SQ 提交和内部状态。**即使单线程也会获取**，因为 ASIO 设计为支持多线程 `run()`。
- `registration_mutex_`: 保护 io_object 注册表。
- `io_object::mutex_`: 保护单个 io_object 的操作队列。

这些锁在单线程场景下 **从不竞争**（始终立即获得），仅增加极微小的原子操作开销。

**NativeUringIoBackend 无任何锁**：SQ/CQ 共享内存由内核侧原子操作保证一致性，用户态单线程访问无竞争。

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



## 附录 C：三种后端 SubmitRead/SubmitWrite 完整流程序列图

### C.1 EvIoBackend — 读路径

```
Worker::AddIoReadEvent(fd, pConn)
  │
  ▼
EvIoBackend::SubmitRead(fd, buf, seq)
  │
  ├─ m_mapFdData.find(fd)
  │   ├─ 不存在 → new WatcherData + new ev_io → 插入 map
  │   └─ 存在   → 复用已有 WatcherData
  │
  ├─ pData->pReadBuf = buf
  ├─ pData->ulSeq = seq
  ├─ ev_io_set(watcher, fd, watcher->events | EV_READ)   ← 仅设置事件标志
  └─ ev_io_start(loop, watcher)                          ← 仅确保已注册
  │
  │  ★ 注意: 至此未发生任何 read() 系统调用！
  │     ev_io_set/start 只修改 libev 内部状态和 epoll_ctl
  │     真正的 read() 要等 epoll_wait 返回后才执行
  │
  ▼ 返回 true ────────────────────► 调用者继续执行
  │
  ... 时间流逝，其他代码运行 ...
  │
  ▼ [libev 事件循环下一轮]
ev_run() 迭代:
  │
  ├─ ev_prepare 回调 (如有)
  ├─ epoll_wait(epoll_fd, timeout)           ← 系统调用: epoll_wait
  │   │
  │   │  ★ fd 上有数据到达 → epoll 检测到 EPOLLIN → 返回此 fd
  │   │  ★ 如果 fd 未连接: epoll 不会返回此 fd
  │   │    (无 EPOLLIN/EPOLLOUT 就绪) → 不会触发回调 → 无 ENOTCONN
  │   │
  │   └─ 返回: events[] = { ..., {fd, EPOLLIN}, ... }
  │
  ├─ ev_check 回调 (如有)
  │
  └─ EV_INVOKE_PENDING ─── 按优先级执行所有待处理回调:
      │
      ├─ ev_check watchers
      ├─ ev_io watchers:
      │   └─ IoEventCallback(watcher, EV_READ)
      │       │
      │       ├─ pData->pReadBuf->ReadFD(fd, iErrno)    ← 系统调用: read(fd,buf,len)
      │       │   └─ 返回: n 字节 (成功) 或 -1 (错误)
      │       │
      │       ├─ result = (n >= 0) ? n : -iErrno
      │       └─ m_callback(fd, seq, IoOp::Read, result, user_data)
      │           │
      │           └─ Worker::OnIoComplete
      │               └─ Worker::HandleIoReadComplete(pConn, result)
      │                   ├─ result > 0:  解码 + 分发消息
      │                   ├─ result == 0: 对端关闭 → DestroyConnect
      │                   └─ result < 0:  errno 判断
      │                       ├─ EAGAIN:  重新 SubmitRead
      │                       └─ 其他:    DestroyConnect
      │
      └─ ev_timer / ev_idle watchers
```

**特点**: 读操作的发起 (`ev_io_set`) 和实际执行 (`ReadFD`) **完全解耦**，中间至少隔一个 `epoll_wait` 周期。fd 未连接时 epoll 不通知 → 无 ENOTCONN。

### C.2 AsioUringIoBackend — 读路径（含 poll() 同步完成）

```
Worker::AddIoReadEvent(fd, pConn)
  │
  ▼
AsioUringIoBackend::SubmitRead(fd, buf, seq)
  │
  ├─ EnsureFdState(fd)
  │   └─ new FdState(m_ioCtx, fd)  → new asio::posix::stream_descriptor(fd)
  │       └─ 内部: 向 ASIO io_uring_service 注册此 fd
  │
  ├─ sp->readPending = true                ← 防重入标志
  ├─ buf->EnsureWritableBytes(8192)
  │
  ├─ sp->sock.async_read_some(buffer, callback)
  │   │
  │   │  ASIO 内部:
  │   │  ├─ io_uring_service::start_op(sock, read_op)
  │   │  │   ├─ 分配 SQE → io_uring_prep_recv(sqe, fd, buf, len)
  │   │  │   ├─ io_uring_sqe_set_data(sqe, op_ptr)
  │   │  │   └─ ★ SQE 写入 SQ 共享内存，但未调用 io_uring_enter() ★
  │   │  │      提交被延迟到下一次 io_context::poll()
  │   │  │
  │   └─ 返回 (未阻塞)
  │
  ├─ m_ioCtx.poll()                        ← ★ 立即 poll()
  │   │
  │   │  poll() → scheduler::poll() → do_one():
  │   │  ├─ 步骤 1: io_uring_enter(ring_fd, to_submit, 0, flags)
  │   │  │   └─ ★ 提交之前 async_read_some 写入的 SQE 给内核 ★
  │   │  │
  │   │  └─ 步骤 2: 收割 CQE (如果有已完成的)
  │   │      │
  │   │      ├─ 场景 A: fd 已连接 + 数据已在内核缓冲区
  │   │      │   → CQE {res=128} (同步完成)
  │   │      │   → 回调 lambda: AdvanceWriteIndex, m_callback(fd, seq, Read, 128)
  │   │      │
  │   │      ├─ 场景 B: fd 未连接 → ENOTCONN  ← ★★★ Bug ★★★
  │   │      │   → io_uring_enter 提交 SQE (recv on unconnected socket)
  │   │      │   → 内核立即返回 CQE {res=-107} (ENOTCONN)
  │   │      │   → 回调 lambda: ec.value()==107 → m_callback(fd, seq, Read, -107)
  │   │      │   → Worker::HandleIoReadComplete(result=-107):
  │   │      │       errno=107, 非 EAGAIN/EINTR → DestroyConnect(conn_iter)
  │   │      │       → 从 mapFdAttr 移除 fd、close(fd)
  │   │      │       → ★ 但 SubmitRead 仍然 return true ★
  │   │      │       → 调用者不知情，继续操作已关闭的 fd
  │   │      │
  │   │      └─ 场景 C: fd 已连接但无数据
  │   │          → io_uring_enter 提交 SQE 成功
  │   │          → CQE 尚未就绪 → poll() 返回 0
  │   │
  │   └─ 返回已收割的 CQE 数量
  │
  └─ return true ─────────────────────► 调用者继续执行
  │
  │  ★ 关键差异: poll() 可能同步完成回调！
  │     回调在 SubmitRead 返回前执行 (仍在同一调用栈)
  │     HandleIoReadComplete → DestroyConnect 销毁 fd
  │     但调用者看到的返回值仍是 true
  │
  ... 如果 fd 未被 ENOTCONN 销毁，后续异步完成 ...
  │
  ▼ [libev 事件循环]
ev_run() 迭代:
  │
  ├─ ev_prepare → OnPrepare → io_context.poll()
  │   └─ 收割新的 CQE → 触发异步回调
  │
  ├─ epoll_wait(epoll_fd, timeout)
  │   │
  │   │  ring_fd 随 CQE 到达变为可读
  │   │  如无 CQE 则等待超时或其他 fd
  │   │
  │   └─ 返回
  │
  ├─ ev_check → OnCheck → io_context.poll()
  │   └─ 收割阻塞期间到达的 CQE
  │
  └─ ev_io(ring_fd) → OnRingReady → io_context.poll()
      └─ 收割处理其他事件期间到达的 CQE
```

**特点**: `async_read_some` 只是队列化 SQE，真正的内核提交在 `poll()` 中。`poll()` 可能**同步完成**回调 → 如果 fd 未连接，ENOTCONN 在 SubmitRead 调用栈内触发 DestroyConnect。

### C.3 NativeUringIoBackend — 读路径

```
Worker::AddIoReadEvent(fd, pConn)
  │
  ▼
NativeUringIoBackend::SubmitRead(fd, buf, seq)
  │
  ├─ auto& st = m_fds[fd]                      ← 获取/创建 fd 状态
  │   └─ st.readPending > 0 → return true       ← 防重入
  │
  ├─ io_uring_get_sqe(&ring)                    ← 从 SQ 获取空闲 SQE
  │   └─ 满 → io_uring_submit → 再试
  │
  ├─ buf->EnsureWritableBytes(8192)
  │
  ├─ PendingOp* po = new PendingOp{fd, seq, Read, buf}
  │
  ├─ io_uring_prep_recv(sqe, fd, buf->GetRawWriteBuffer(), cap, 0)
  ├─ io_uring_sqe_set_data(sqe, po)            ← PendingOp* 作为 user_data
  │
  ├─ st.readPending++
  ├─ io_uring_submit(&ring)                    ← ★ 立即提交 SQE
  │
  └─ return true
  │
  │  ★ SQE 已在内核中，CQE 尚未到达 — "真异步"
  │
  ... 时间流逝 ...
  │
  ▼ 内核侧:
  │   ├─ fd 已连接 + 数据到达 → CQE {res=N}
  │   └─ fd 未连接 → CQE {res=-107} (ENOTCONN)
  │
  ▼ 内核完成 → ++eventfd counter
  │
  ▼ [libev 事件循环]
ev_run() 迭代:
  │
  ├─ epoll_wait → eventfd 就绪
  │
  └─ ev_io(eventfd) → OnEvfd → ReapCqes()
      └─ io_uring_peek_cqe + io_uring_cqe_seen
          ├─ PendingOp* po = cqe_get_data(cqe)
          ├─ fd/seq 校验 (m_fds 中且未取消)
          ├─ valid:
          │   ├─ po->op == Read → buf->AdvanceWriteIndex(res)
          │   └─ m_callback(fd, seq, IoOp::Read, res, user_data)
          │       └─ Worker::HandleIoReadComplete(...)
          └─ delete po
```

**特点**: SQE 通过 `io_uring_submit` **立即提交**，无延迟。eventfd（通过 `io_uring_register_eventfd` 标准 API 注册）作为完成通知，无 /proc hack。陈旧 CQE 通过 fd/seq 校验安全丢弃，不触碰连接缓冲。

### C.4 三种后端写路径对比

```
┌─────────────────────────────────────────────────────────────────┐
│                    EvIoBackend — 写路径                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  SubmitWrite(fd, buf, seq)                                      │
│    ├─ ev_io_set(EV_WRITE)     ← 仅注册写就绪事件                  │
│    └─ ev_io_start()           ← 无实际 I/O                       │
│                                                                 │
│  ... epoll_wait → fd 可写 ...                                   │
│    └─ IoEventCallback(EV_WRITE)                                 │
│        └─ buf->WriteFD(fd)    ← 真正的 write() 系统调用           │
│            └─ m_callback(fd, seq, Write, result)                 │
│                                                                 │
│  特点: 发起和真正 I/O 完全解耦                                    │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                 AsioUringIoBackend — 写路径                      │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  SubmitWrite(fd, buf, seq)                                      │
│    ├─ readable = buf->ReadableBytes()                           │
│    │   ├─ == 0 → m_callback(Write, 0) ← 同步回调，result=0       │
│    │   │   └─ Worker::HandleIoWriteComplete(result=0)            │
│    │   │       ├─ pWaitForSendBuff 有数据                        │
│    │   │       │   ├─ CODEC_PB_INTERNAL: CmdConnectWorker::Start │
│    │   │       │   └─ HTTP: SendTo(stMsgShell) → flush           │
│    │   │       └─ pWaitForSendBuff 为空 → no-op                  │
│    │   │                                                         │
│    │   └─ > 0 → sock.async_write_some(buf, len, callback)        │
│    │       └─ m_ioCtx.poll() ← 提交 SQE 并收割 CQE               │
│    │           ├─ 同步完成 (localhost 小包)                      │
│    │           └─ 异步完成 (EAGAIN) → 等待后续 poll()            │
│                                                                 │
│  特点: result==0 是关键路径 — 触发 pWaitForSendBuff flush         │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│               NativeUringIoBackend — 写路径                      │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  SubmitWrite(fd, buf, seq)                                      │
│    ├─ readable = buf->ReadableBytes()                           │
│    │   ├─ == 0 → m_callback(Write, 0) ← 同步回调 (空写触发)      │
│    │   │                                                         │
│    │   └─ > 0 → io_uring_get_sqe → PendingOp                    │
│    │       ├─ zc + >=threshold:                                 │
│    │       │   malloc(bounce) + memcpy + prep_send_zc(bounce)   │
│    │       │   → 双 CQE：结果(Write) + NOTIF(WriteNotif)        │
│    │       └─ 普通: prep_send(buf->GetRawReadBuffer())          │
│    │           → 单 CQE：Write                                  │
│    │                                                             │
│  特点: 全异步 io_uring send(zc)，按阈值分流，bounce 解耦生命周期   │
└─────────────────────────────────────────────────────────────────┘
```



## 附录 D：ENOTCONN 竞态 — 三种后端差异分析

### D.1 问题场景：Worker 创建 outgoing S2S 连接

```
Worker::AutoSend(strIdentify) 调用链:

  CreateConnectFdAttr(iFd, ulSeq, strIdentify)     ← 创建 FdAttr
    ├─ CreateFdAttr(iFd, ulSeq)                    ← 加入 mapFdAttr
    ├─ AddIoTimeout(1.5s)                          ← 连接超时
    ├─ AddIoReadEvent(pConn)                       ← ★ 此时 fd 尚未 connect()！
    │   └─ m_pIoBackend->SubmitRead(fd, buf, seq)
    └─ AddIoWriteEvent(pConn)                      ← ★ pSendBuff 此时为空
        └─ m_pIoBackend->SubmitWrite(fd, buf, seq)

  EncodeByConnectionCodec(..., pWaitForSendBuff)   ← GenKey 数据编码到等发缓冲区
  AddMsgShell(strIdentify, stMsgShell)              ← 加入路由表
  connect(iFd, &addr, sizeof(addr))                ← 建立 TCP 连接
```

### D.2 三种后端在此场景下的行为对比表

| 阶段 | EvIoBackend | NativeUringIoBackend | AsioUringIoBackend |
|------|-------------|---------------------|---------------------|
| **AddIoReadEvent** | ev_io_set(EV_READ) — 无系统调用 | io_uring_submit(SQE) — 立即提交 | async_read_some + poll() — 延迟提交 |
| **fd 状态** | 未连接 | 未连接 | 未连接 |
| **I/O 触发时机** | epoll_wait 返回后 (fd 就绪) | 内核异步完成 → CQE → eventfd | poll() 同步完成或后续异步 |
| **ENOTCONN 路径** | 不发生 — epoll 不报告未连接 fd | 异步 CQE {res=-107} | 同步 CQE {res=-107} |
| **DestroyConnect 时机** | N/A | SubmitRead 返回后的下个 ev_run 迭代 | SubmitRead 调用栈内 (同步) |
| **结果** | ✅ 安全 | ⚠️ 延迟爆炸 (异步销毁) | ❌ 立即爆炸 (同步销毁) |

### D.3 AsioUringIoBackend 同步 ENOTCONN 的完整时序

```
时间 ──────────────────────────────────────────────────────────────►

[AutoSend 调用栈]

CreateConnectFdAttr:
  CreateFdAttr             → fd=14 加入 mapFdAttr ✅
  AddIoTimeout             → 超时定时器 ✅
  AddIoReadEvent           → SubmitRead(fd=14)
    │
    │  async_read_some(fd=14, buf, cb)
    │    └─ SQE 写入 SQ (未提交)
    │
    │  ★ poll() ──────────────────────────────────────────────┐
    │    io_uring_enter → 提交 SQE                              │
    │    内核: fd=14 未连接 → 立即返回 ENOTCONN CQE              │
    │    ASIO 收割 CQE {res=-107}                               │
    │    ★ lambda callback 同步触发 ──────────────────────┐     │
    │      m_callback(14, seq, Read, -107, user_data)     │     │
    │      └─ Worker::OnIoComplete                        │     │
    │          └─ HandleIoReadComplete(pConn, -107)       │     │
    │              errno=107 ≠ EAGAIN/EINTR               │     │
    │              ★ DestroyConnect(conn_iter) ←──────────┘     │
    │                  ├─ 从 mapFdAttr 移除 fd=14               │
    │                  ├─ DelMsgShell (从路由表移除)             │
    │                  └─ close(fd=14)                          │
    │                                                           │
    │    poll() 返回 1 ─────────────────────────────────────────┘
    │
    └─ SubmitRead 返回 true ★ 但 fd=14 已被销毁！

  AddIoWriteEvent         → SubmitWrite(fd=14, pSendBuff)
    │ pSendBuff 为空 → readable=0
    └─ m_callback(14, seq, Write, 0, user_data)
        └─ OnIoComplete → mapFdAttr.find(14) == end()
            └─ ★ 直接返回，无操作 (fd 已不存在)

  return pConn            → ★ 返回指向已销毁 fd 的 FdAttr！

[返回到 AutoSend]

pConn = CreateConnectFdAttr(...)  → pConn ≠ nullptr ★ (致命！)
EncodeByConnectionCodec(..., pConn->pWaitForSendBuff)
  → GenKey 数据编码到 pWaitForSendBuff ★ (写到已销毁连接对象)
AddMsgShell(strIdentify, {fd=14, seq=...})
  → ★ 已关闭的 fd=14 加入 mapMsgShell!
connect(fd=14, &addr, sizeof(addr))
  → ★ connect on closed fd → EBADF → 静默忽略
return true ★ 调用者以为连接成功

[后续 GenKey 请求]

SendTo → 找到 fd=14 在 mapMsgShell
  → SendTo({fd=14, seq=...}, oMsgHead, oMsgBody)
  → mapFdAttr.find(14) == end() ★
  → LOG4_ERROR "no fd 14 found in mapFdAttr"
  → 返回 false

所有 GenKey 请求失败！直到路由超时清理该条目或 Manager 侧主动建立新连接。
```

### D.4 NativeUringIoBackend 异步 ENOTCONN 的时序

```
时间 ──────────────────────────────────────────────────────────────►

[AutoSend 调用栈]

CreateConnectFdAttr:
  AddIoReadEvent           → SubmitRead(fd=14)
    │ io_uring_prep_recv(sqe, fd=14, buf, len)
    │ io_uring_submit(&ring) ← ★ 立即提交 SQE
    │ PendingOp* po = new PendingOp{14, seq, Read, buf}
    │ st.readPending++
    └─ return true ★ SQE 已在内核，但 CQE 尚未到达

  AddIoWriteEvent          → SubmitWrite(fd=14, pSendBuff)
    │ pSendBuff 为空 → readable=0
    └─ m_callback(14, seq, Write, 0)  ← ★ 同步回调
        └─ OnIoComplete → fd=14 仍在 mapFdAttr ✅
            └─ HandleIoWriteComplete(result=0):
                pWaitForSendBuff 此时为空 (编码尚未发生) → no-op

  return pConn             → ★ 返回有效的 FdAttr (fd=14 仍存活)

[返回到 AutoSend]

EncodeByConnectionCodec(..., pWaitForSendBuff)  → 数据编码 ✅
AddMsgShell(...)                                 → 加入路由表 ✅
connect(fd=14, &addr, sizeof(addr))              → TCP 连接发起 ✅
return true ★ AutoSend 看似成功

... 时间流逝 (event loop 继续) ...

[libev 事件循环迭代 N+1]

epoll_wait → eventfd 可读
  └─ ev_io(eventfd) → ReapCqes()
      └─ io_uring_peek_cqe: CQE {user_data=po, res=-107}
          → po = PendingOp{fd=14, seq, Read, buf}
          → valid (fd/seq 匹配且未取消)
          → m_callback(14, seq, Read, -107, user_data)
          └─ HandleIoReadComplete(pConn, -107)
              └─ ★ 异步 DestroyConnect ★
                  此时 fd=14 已 connect()、已在 mapMsgShell...
          → delete po

[问题：与 AsioUringIoBackend 同样的后果，只是时序不同]

如果 connect 已完成 + mapMsgShell 已有条目：
  → DestroyConnect 清理 fd，但 mapMsgShell 可能残留
  → 后续 SendTo → "no fd 14 found in mapFdAttr" → 失败

如果 connect 未完成 (EINPROGRESS)：
  → DestroyConnect 关闭 fd → 连接中断
  → mapMsgShell 已有条目 → 同上的问题
```

### D.5 EvIoBackend 为何安全

```
CreateConnectFdAttr:
  AddIoReadEvent → SubmitRead(fd=14)
    │ ev_io_set(EV_READ)  ← 仅设置事件标志，无系统调用涉及 fd=14
    └─ ev_io_start()      ← epoll_ctl(ADD, fd=14, EPOLLIN)
        ★ 只是把 fd 加入 epoll 集合，不执行任何 I/O
        ★ epoll 只在 fd 就绪时通知 — 未连接的 fd 不会就绪

[返回到 AutoSend]

EncodeByConnectionCodec(..., pWaitForSendBuff)  → 数据 ✅
AddMsgShell(...)                                 → 路由表 ✅
connect(fd=14, &addr, sizeof(addr))              → TCP 连接发起 ✅

... event loop 继续 ...

epoll_wait → fd=14 变为可写 (connect 完成)
  └─ IoEventCallback(EV_WRITE)
      └─ buf->WriteFD(fd=14) ← ★ fd 已连接，write 正常
          └─ result=0 (pSendBuff 为空)
              └─ HandleIoWriteComplete(result=0):
                  pWaitForSendBuff 有数据 ✅
                  → CmdConnectWorker::Start → 发送握手

★ 核心: epoll 只在 fd 就绪时通知
  未连接的 fd 不会触发 EV_READ 或 EV_WRITE
  因此 ENOTCONN 不可能发生
```

### D.6 ENOTCONN 竞态根因与修复

| 后端 | SQE 提交 | ENOTCONN 触发 | 时序 | 表现 |
|------|---------|--------------|------|------|
| EvIoBackend | N/A (epoll watcher) | 不发生 | N/A | ✅ 安全 |
| NativeUringIoBackend | `io_uring_submit` 立即 | 异步 CQE | SubmitRead 返回后 | ⚠️ 延迟爆炸 |
| AsioUringIoBackend | `poll()` 延迟 | 同步 CQE | SubmitRead 调用栈内 | ❌ 立即爆炸 |

**共同根因**: `CreateConnectFdAttr` 在 `connect()` 之前调用 `AddIoReadEvent`。
**修复原理**: 将 IO 事件注册移到 `connect()` 之后 → fd 已连接 → 无论同步/异步，都不再有 ENOTCONN。

**修复策略 (兼容 EV + io_uring 双模式)**:
- Factory 函数 (`CreateConnectFdAttr`, `CreateHttpFdAttr`): `if (!m_pIoBackend)` 保护 IO 事件 → EV 模式保留，io_uring 模式跳过
- Caller (`AutoSend`, `AutoConnect`): `connect()` 之后统一调用 AddIoReadEvent/AddIoWriteEvent → EV 模式第二次调用是幂等 RefreshEvent，io_uring 模式是唯一调用且 fd 已连接



## 附录 E：Worker 修复后的完整时序 (io_uring 模式)

### E.1 AutoSend(strIdentify) — S2S 内部连接 (CODEC_PB_INTERNAL)

```
AutoSend(strIdentify, oGenKeyMsgHead, oGenKeyMsgBody):
  │
  ├─ HostPort2SockAddr(strHost, iPort) → creates socket fd
  │
  ├─ CreateConnectFdAttr(fd, ulSeq, strIdentify):
  │   ├─ CreateFdAttr → 加入 mapFdAttr
  │   ├─ AddIoTimeout(1.5s)
  │   └─ if (!m_pIoBackend) {              ← EV 模式：注册 watcher
  │         AddIoReadEvent(pConn)             io_uring 模式：跳过 (defer to caller)
  │         AddIoWriteEvent(pConn)
  │       }
  │
  ├─ EncodeByConnectionCodec(codec, oGenKeyMsg, pWaitForSendBuff)
  │   → GenKey 数据在 pWaitForSendBuff 中
  │
  ├─ mapSeq2WorkerIndex[ulSeq] = iWorkerIndex
  ├─ AddMsgShell(strIdentify, stMsgShell)   → 加入路由表
  │
  ├─ connect(fd, &addr, sizeof(addr))       ← ★ TCP 连接完成
  │
  ├─ ★ AddIoReadEvent(pConn)                ← EV: 第二次调用 (RefreshEvent 幂等)
  │   └─ [io_uring] SubmitRead → async_read_some + poll()
  │        ★ fd 已连接 → 无 ENOTCONN → 读 SQE 正常提交
  │
  ├─ ★ AddIoWriteEvent(pConn)               ← EV: 第二次调用 (RefreshEvent 幂等)
  │   └─ [io_uring] SubmitWrite → pSendBuff 为空 → callback(0)
  │       └─ ★ HandleIoWriteComplete(result=0):
  │           ├─ pWaitForSendBuff->ReadableBytes() > 0 ✓
  │           ├─ mapSeq2WorkerIndex.find(ulSeq) → found ✓
  │           ├─ CmdConnectWorker::Start(stMsgShell, index)
  │           │   └─ StepConnectWorker::Emit → SendTo(CMD_REQ_CONNECT_TO_WORKER)
  │           │       └─ Encode to pSendBuff → WriteFD → cmd=7 已发送
  │           ├─ mapSeq2WorkerIndex.erase(ulSeq)
  │           ├─ CancelFd(fd)               ← 清理读 SQE (由 AddIoReadEvent 提交)
  │           └─ SubmitRead(fd, pRecvBuff)   ← 重新提交读 (等待响应)
  │
  └─ return true

[远程 Logic Manager 处理]

接收 cmd=7 → StepConnectWorker::Callback → StepTellWorker
  → StepTellWorker::Emit → SendTo(CMD_REQ_TELL_WORKER, cmd=9)

[Interface Worker 接收响应]

读完成 → HandleIoReadComplete → 解码 cmd=9
  → StepTellWorker::Callback (StepTellWorker.cpp:48):
      AddMsgShell(worker_identify, stMsgShell)
      AddNodeIdentify(node_type, worker_identify)
      ★ SendTo(stMsgShell)                  ← flush pWaitForSendBuff！
          ├─ pWaitForSendBuff → pSendBuff   (GenKey 数据)
          └─ WriteFD(fd) → GenKey 请求发送

[Logic Manager 处理 GenKey → 生成 token+key → 响应]

读完成 → HandleIoReadComplete → GenKey response → dispatch
  → ModuleInterface::GenKeyVerifyKeyStepCo20 → HTTP 响应
  → 客户端收到 token+key ✅
```

### E.2 AutoSend(strHost, iPort) — HTTP 外部连接

```
AutoSend(strHost, iPort, strUrlPath, oHttpMsg, pStep):
  │
  ├─ CreateHttpFdAttr → if (!m_pIoBackend) { IO events }
  ├─ Encode → pWaitForSendBuff
  ├─ connect(fd)
  ├─ AddIoReadEvent + AddIoWriteEvent
  │   └─ [io_uring] callback(0) → HandleIoWriteComplete:
  │       ├─ pWaitForSendBuff 有数据 ✓
  │       ├─ mapSeq2WorkerIndex.find → NOT found (HTTP)
  │       └─ SendTo(stMsgShell) → flush pWaitForSendBuff → HTTP 请求发送
  └─ return true
```

### E.3 AutoConnect(strIdentify) — 预连接 (无数据)

```
AutoConnect(strIdentify):
  │
  ├─ CreateConnectFdAttr → if (!m_pIoBackend) { IO events }
  ├─ mapSeq2WorkerIndex[ulSeq] = iWorkerIndex
  ├─ AddMsgShell
  ├─ connect(fd)
  ├─ AddIoReadEvent + AddIoWriteEvent
  │   └─ [io_uring] callback(0) → HandleIoWriteComplete:
  │       └─ pWaitForSendBuff 为空 → no-op (连接就绪，等待后续 SendTo)
  └─ return true
```



## 附录 F：ASIO io_uring_service 内部实现原理

> 源码: `code/3party/asio/include/asio/detail/io_uring_service.hpp` +
> `impl/io_uring_service.ipp`

### F.1 初始化：双 fd 唤醒架构

```
io_uring_service::io_uring_service(execution_context& ctx)
  │
  ├─ scheduler_ = use_service<scheduler>(ctx)   ← 获取全局调度器
  ├─ reactor_   = use_service<reactor>(ctx)     ← 获取 reactor (epoll wrapper)
  │
  ├─ init_ring():
  │   ├─ io_uring_queue_init(ring_size=16384, &ring_, 0)
  │   │   └─ 创建 io_uring 实例 → ring_.ring_fd (SQ/CQ 共享内存 + 内核 fd)
  │   │
  │   ├─ event_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)
  │   │   └─ 创建用户态事件通知 fd
  │   │
  │   └─ io_uring_register_eventfd(&ring_, event_fd_)
  │       └─ ★ 将 eventfd 注册到 io_uring → 内核完成 CQE 时自动写入 eventfd
  │          这是 ASIO 与 liburing 直接使用的关键差异：
  │          ASIO 不 poll ring_fd，而是通过 eventfd + reactor 驱动
  │
  └─ register_with_reactor():
      └─ reactor_.register_internal_descriptor(READ, event_fd_, new event_fd_read_op)
          └─ ★ 将 eventfd 加入 epoll 集合
             当 io_uring 有 CQE 完成 → 内核写 eventfd → epoll 检测到 eventfd 可读
             → reactor 调用 event_fd_read_op::do_perform()

架构图:

  io_uring 实例 (共享内存)
  ┌─────────────────────────────┐
  │ SQ (Submission Queue)       │  ← 用户态写 SQE
  │ CQ (Completion Queue)       │  ← 内核写 CQE
  │ ring_fd                     │  ← 内核 fd，标准 I/O 事件通知
  └──────────┬──────────────────┘
             │ io_uring_register_eventfd(ring, event_fd)
             ▼
         event_fd               ← 用户态 eventfd，内核有 CQE 时自动 ++counter
             │
             │ reactor::register_internal_descriptor(READ, event_fd)
             ▼
         epoll (reactor)
             │
             │ eventfd 可读
             ▼
    event_fd_read_op::do_perform()
      ├─ read(event_fd)         ← 清空 counter
      └─ io_uring_service::run(0, ops)
          └─ ★ 收割所有 CQE，投递完成回调
```

### F.2 操作提交流程：`start_op()` → `submit_sqes()`

```
io_uring_service::start_op(op_type, io_obj, op, is_continuation)
  │
  │  前提: async_read_some / async_write_some 内部调用此函数
  │
  ├─ io_obj->queues_[op_type].op_queue_.empty()?  ← 检查操作队列是否为空
  │   │
  │   ├─ 空: op->perform(false)                  ← 尝试立即执行
  │   │   ├─ 成功: scheduler_.post_immediate_completion(op)
  │   │   │         ★ 操作同步完成，直接投递回调 (如 buffer 为空)
  │   │   │
  │   │   └─ 失败 (需要异步):
  │   │       │
  │   │       ├─ io_obj->queues_[op_type].op_queue_.push(op)
  │   │       │
  │   │       ├─ get_sqe():                     ← 从 SQ 获取空闲 SQE
  │   │       │   ├─ sqe = io_uring_get_sqe(&ring_)
  │   │       │   └─ 如果 SQ 满 → flush: submit_sqes() → 重试 get_sqe()
  │   │       │
  │   │       ├─ op->prepare(sqe)               ← 填充 SQE (io_uring_prep_recv/...)
  │   │       ├─ io_uring_sqe_set_data(sqe, &io_obj->queues_[op_type])
  │   │       │                                    ★ 将 io_queue 指针设为 user_data
  │   │       │
  │   │       ├─ scheduler_.work_started()      ← 增加 outstanding_work_ 计数
  │   │       └─ post_submit_sqes_op(lock):     ← ★ 投递提交操作
  │   │           ├─ pending_sqes_++            ← 累加待提交 SQE 计数
  │   │           └─ 如果 !pending_submit_sqes_op_:
  │   │               scheduler_.post_immediate_completion(&submit_sqes_op_)
  │   │               ★ 将 submit_sqes_op 投递到调度器
  │   │
  │   └─ 非空: io_obj->queues_[op_type].op_queue_.push(op)
  │             ★ 排队等待前一个操作完成
  │             scheduler_.work_started()
  │
  └─ ★ 关键: start_op 本身不调用 io_uring_enter！
      SQE 只是写入 SQ 共享内存，内核提交由 submit_sqes_op 延迟执行

submit_sqes_op::do_complete():
  │
  │  调度器执行此 operation 时调用 (在 poll_one/poll 路径中)
  │
  └─ service_->submit_sqes()

io_uring_service::submit_sqes():
  │
  ├─ 如果 pending_sqes_ == 0 → 直接返回
  │
  ├─ nr = io_uring_submit(&ring_)              ← ★ 系统调用: io_uring_enter
  │   └─ 批量提交所有 pending SQEs 到内核
  │
  ├─ pending_sqes_ -= nr
  ├─ pending_submit_sqes_op_ = false
  │
  └─ 如果 pending_sqes_ > 0:                    ← 还有未提交的 SQE
      post_submit_sqes_op()                     ← 再次投递提交操作
```

### F.3 完成处理流程：`run()` → 收割 CQE

```
io_uring_service::run(long usec, op_queue<operation>& ops)
  │
  │  被以下路径调用:
  │  ├─ event_fd_read_op::do_perform()  ← reactor 检测到 eventfd 可读
  │  ├─ io_context::poll()              ← 用户主动 poll
  │  └─ io_context::run()               ← 阻塞运行模式 (Thunder 未使用)
  │
  ├─ 限制: 最多处理 complete_batch_size(128) 个 CQE
  │
  ├─ while (true):
  │   │
  │   ├─ submit_sqes()                         ← ★ 先提交所有待处理的 SQE
  │   │                                            (关键: 每次 run 都先 flush SQ)
  │   │
  │   ├─ io_uring_wait_cqe(&ring_, &cqe)?      ← 等待 CQE (带超时)
  │   │   ├─ 有 CQE:
  │   │   │   ├─ ptr = io_uring_cqe_get_data(cqe)
  │   │   │   │   ★ ptr = &io_obj->queues_[op_type] (start_op 时设置的)
  │   │   │   │
  │   │   │   ├─ io_queue* io_q = static_cast<io_queue*>(ptr)
  │   │   │   ├─ io_q->set_result(cqe->res)    ← 保存操作结果
  │   │   │   │   cqe->res >= 0: 字节数
  │   │   │   │   cqe->res < 0:  -errno (如 -107 = ENOTCONN)
  │   │   │   │
  │   │   │   ├─ ops.push(io_q)                ← 加入待完成队列
  │   │   │   │
  │   │   │   └─ io_uring_cqe_seen(&ring_, cqe) ← 标记 CQE 已消费
  │   │   │
  │   │   └─ 无 CQE (超时/中断) → break
  │   │
  │   └─ ops 数量达到 batch_size → break
  │
  └─ 返回后: 调度器遍历 ops，调用 io_queue::perform_io(result)

io_queue::perform_io(int result):
  │
  │  由调度器从 ops 队列中取出执行
  │
  ├─ 从 op_queue_ 中取出第一个 io_uring_operation
  ├─ op->complete(result)                      ← 调用用户 lambda 回调
  │   └─ 例: async_read_some 的回调 lambda:
  │       ec.assign(-result, system_category()) [如果 result<0]
  │       n = result [如果 result>=0]
  │       → 用户代码执行
  │
  ├─ 如果 op_queue_ 还有更多操作:
  │   └─ start_op() 提交下一个操作              ← 自动排队下一个
  │
  └─ scheduler_.work_finished()               ← 减少 outstanding_work_ 计数
```

### F.4 `io_context::poll()` 的完整执行路径

```
io_context::poll()
  │
  └─ scheduler::poll()
      │
      └─ scheduler::do_poll():
          │
          ├─ 步骤 1: 执行所有待处理的 ready-to-run operations
          │   ├─ submit_sqes_op::do_complete   ← ★ 如果有 pending SQEs，先提交
          │   ├─ io_queue::perform_io           ← CQE 完成回调 (用户 lambda)
          │   └─ 其他 operations (timer, signal, ...)
          │
          ├─ 步骤 2: 如果没有任何就绪 operation:
          │   │
          │   ├─ reactor::poll(0)              ← 非阻塞检查 eventfd
          │   │   └─ event_fd_read_op::do_perform():
          │   │       ├─ read(event_fd_)        ← 清空计数器
          │   │       └─ io_uring_service::run(0, ops)
          │   │           ├─ submit_sqes()      ← 提交 SQEs
          │   │           ├─ io_uring_wait_cqe(&ring_, &cqe, timeout=0) ← 非阻塞
          │   │           └─ 收割 CQE → 加入 ops
          │   │
          │   └─ 执行 ops 中的 io_queue::perform_io → 触发用户回调
          │
          └─ 返回已执行的 handler 数量

关键理解:
  poll() 一次调用做了两件事:
  1. submit_sqes() — 将 start_op 中队列化的 SQE 通过 io_uring_enter 提交给内核
  2. run()      — 收割已完成 CQE 并触发用户回调

  因此 "SubmitRead 末尾调 poll()" 的效果:
  async_read_some → start_op → SQE 写入 SQ → 立即 poll()
    → submit_sqes (提交 SQE) + run(收割 CQE)
    → 如果 fd 已连接 + 数据已就绪 → 同步完成回调
    → 如果 fd 未连接 → 同步 ENOTCONN 回调
```

### F.5 与 libev 的三路集成 — 事件循环协同

```
                    ┌─── libev 事件循环 ───────────────────────────────┐
                    │                                                  │
                    │  while (running) {                               │
                    │                                                  │
ev_prepare          │    ● ev_prepare → OnPrepare()                    │
hook                │      └─ m_ioCtx.poll()                           │
                    │          ├─ submit_sqes() ← commit pending SQEs  │
                    │          └─ run(0)         ← reap CQEs (non-block)│
                    │                                                  │
                    │    ┌─ epoll_wait(timeout) ──────────────────┐    │
                    │    │  ASIO 内部:                             │    │
                    │    │  ├─ reactor 监控 event_fd               │    │
                    │    │  │   ★ event_fd 被 io_uring_register_   │    │
                    │    │  │     eventfd 注册到内核               │    │
                    │    │  │   ★ 内核完成 I/O → ++eventfd counter │    │
                    │    │  │   ★ epoll 检测到 eventfd 可读        │    │
kernel              │    │  │                                       │    │
blocking            │    │  ├─ ring_fd 也被 ev_io 监控              │    │
                    │    │  │   ★ 作为额外的唤醒路径                │    │
                    │    │  │                                       │    │
                    │    │  └─ 业务 fd (在 ev 模式中)               │    │
                    │    └──────────────────────────────────────────┘    │
                    │                                                  │
ev_check            │    ● ev_check → OnCheck()                         │
hook                │      └─ m_ioCtx.poll()                           │
                    │          └─ 收割 epoll_wait 期间到达的 CQE       │
                    │                                                  │
ev_io               │    ● ev_io(ring_fd) → OnRingReady()              │
hook                │      └─ m_ioCtx.poll()                           │
                    │          └─ 收割处理其他事件期间到达的 CQE       │
                    │                                                  │
                    │    ● ev_io    → 业务 fd 回调 (EV 模式)           │
                    │    ● ev_timer → 超时检测                         │
                    │    ● ev_idle  → 空闲任务                         │
                    │  }                                               │
                    └──────────────────────────────────────────────────┘

为什么三路都需要？因为 ASIO 不是"push"模型:

  1. OnPrepare (poll before epoll_wait):
     场景: 上次循环末尾提交了 SQE，CQE 在 epoll_wait 前到达
     无 OnPrepare: CQE 等待整个 epoll_wait 周期

  2. OnCheck (poll after epoll_wait):
     场景: CQE 在 epoll_wait 阻塞期间到达 → event_fd 唤醒 epoll_wait
     但 reactor 的通知可能在 poll 内已处理，或需要再收割
     无 OnCheck: 某些 CQE 延迟到下一轮

  3. OnRingReady (poll on ring_fd readable):
     场景: 处理 ev_io/ev_timer 回调时提交了新 SQE → 立即完成 → ring_fd 就绪
     无 OnRingReady: 这些 CQE 要等下一轮 epoll_wait

  ★ 三路互补保证了 CQE 在任何时间窗口到达都能被及时收割
```

### F.6 NativeUringIoBackend vs AsioUringIoBackend — SQE 提交时序对比

```
═══════════════════════════════════════════════════════════════════════
  操作: SubmitRead(fd=14, buf, seq) — fd 尚未 connect()
═══════════════════════════════════════════════════════════════════════

NativeUringIoBackend (原生 liburing):
────────────────────────────────────────────────────────────────────
  io_uring_get_sqe(&ring)        ← 获取空闲 SQE
  PendingOp* po = new PendingOp  ← heap 分配 op 上下文
  io_uring_prep_recv(sqe, 14)    ← 填充 SQE
  io_uring_sqe_set_data(sqe, po) ← PendingOp* 作为 user_data
  io_uring_submit(&ring)         ← ★ io_uring_enter 提交 SQE
                                    内核处理:
                                      fd=14 未连接 → 生成 CQE {res=-107} (ENOTCONN)
                                      写入 CQ + ++eventfd counter
                                    返回: 已提交 1 SQE
  return true
  ★ SQE 已在内核中，CQE 尚未被收割
  ★ CQE 收割在下次 ev_io(eventfd) → ReapCqes 中（异步）

AsioUringIoBackend (ASIO 封装):
────────────────────────────────────────────────────────────────────
  sock.async_read_some(buf, cb)  ← 调用 ASIO
    └─ io_uring_service::start_op(read_op, io_obj, op)
        ├─ op->perform(false)    ← 无法同步完成 (需异步)
        ├─ io_obj->queues_[read_op].op_queue_.push(op)
        ├─ get_sqe() → sqe       ← 获取空闲 SQE
        ├─ op->prepare(sqe)      ← io_uring_prep_recv(sqe, 14, ...)
        ├─ io_uring_sqe_set_data(sqe, &io_obj->queues_[read_op])
        └─ post_submit_sqes_op() ← ★ 投递 submit_sqes_op 到调度器
                                    仅 pending_sqes_++，不调用 io_uring_enter!

  ★★★ 此时 SQE 在 SQ 中但未被内核看到 ★★★

  m_ioCtx.poll()                 ← 调用者显式 poll
    └─ scheduler::do_poll()
        ├─ submit_sqes_op::do_complete
        │   └─ io_uring_service::submit_sqes()
        │       └─ io_uring_submit(&ring_)  ← ★ 这里才调用 io_uring_enter!
        │           内核处理:
        │             fd=14 未连接 → 生成 CQE {res=-107}
        │             写入 CQ + ++event_fd counter
        │           返回: 已提交 N SQE
        │
        ├─ reactor::poll(0) → eventfd 可读
        │   └─ event_fd_read_op::do_perform
        │       └─ io_uring_service::run(0, ops)
        │           ├─ io_uring_wait_cqe(..., timeout=0) ← 非阻塞取 CQE
        │           │   └─ 返回 cqe {user_data=io_q, res=-107}
        │           ├─ io_q->set_result(-107)
        │           └─ ops.push(io_q)
        │
        └─ 执行 io_queue::perform_io(-107)
            └─ op->complete() → 用户 lambda 回调 ★ 同步触发！
                └─ m_callback(14, seq, Read, -107, user_data)
                    └─ Worker::HandleIoReadComplete(pConn, -107)
                        └─ DestroyConnect ★ 在 poll() 内同步执行！

对比总结:
  NativeUringIoBackend: SQE 提交 ───┬─── CQE 生成 ───┬─── CQE 收割 ───┬─── 回调
                       (io_uring_submit)  (内核异步)    (ReapCqes异步)  (异步)

  AsioUringIoBackend:  SQE 写入 SQ ─── poll() ─── submit + 收割 + 回调 (全部同步在一个调用栈内)
```

---

## 附录 G：`Result==0` 写回调 — pWaitForSendBuff 刷新机制

### G.1 为什么需要一个"空写"回调

Thunder 的 IO Backend 统一接口中，写操作完成后通过 `m_callback(fd, seq, IoOp::Write, result)` 通知 Worker。
当 `result == 0` 时 (pSendBuff 为空，无需真正写)，`HandleIoWriteComplete` 并不会空转 —
它检查 `pWaitForSendBuff` 是否有排队数据，这是 outgoing 连接建立后的**第一个数据发送触发点**。

```
┌─────────────────────────────────────────────────────────────────┐
│            outgoing 连接建立后的"空写回调"机制                     │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  AutoSend:                                                      │
│    1. CreateConnectFdAttr → fd 加入 mapFdAttr                    │
│    2. Encode → pWaitForSendBuff                                 │
│    3. AddMsgShell → mapMsgShell                                  │
│    4. connect()                                                  │
│    5. AddIoWriteEvent → SubmitWrite(empty pSendBuff)             │
│       └─ callback(Write, result=0)                               │
│                                                                 │
│  HandleIoWriteComplete(result=0):                                │
│    ├─ pSendBuff 为空 → readable=0 → 触发 result=0 回调            │
│    ├─ ★ pWaitForSendBuff->ReadableBytes() > 0 ★                 │
│    │   ├─ mapSeq2WorkerIndex 有条目:                             │
│    │   │   └─ CmdConnectWorker::Start → 发送握手 (cmd=7)         │
│    │   │       (GenKey 数据留在 pWaitForSendBuff，等待            │
│    │   │        StepTellWorker::Callback 中 SendTo 触发的 flush) │
│    │   │                                                        │
│    │   └─ 无 mapSeq2WorkerIndex:                                 │
│    │       └─ SendTo(stMsgShell) → 立即 flush pWaitForSendBuff   │
│    │                                                                │
│    └─ pWaitForSendBuff 为空:                                     │
│        └─ no-op (纯预连接，如 AutoConnect)                       │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### G.2 三种后端的 result==0 触发时机

| 后端 | SubmitWrite (空 pSendBuff) | 触发方式 | 延迟 | pWaitForSendBuff 状态 |
|------|--------------------------|---------|------|----------------------|
| EvIoBackend | ev_io_set(EV_WRITE) → epoll_wait → fd 可写 → WriteFD(pSendBuff=empty)→result=0 | 异步 (epoll) | 1个 ev_run 迭代 | 编码已完成 ✅ |
| NativeUringIoBackend | readable=0 → m_callback(Write, 0) | **同步** | 0 (SubmitWrite 内) | 取决于调用时机 |
| AsioUringIoBackend | async_write_some(empty) → start_op → perform 立即成功 → result=0 | **同步** (poll 前) | 0 | 取决于调用时机 |

**时序要求**: result==0 回调触发时，`pWaitForSendBuff` **必须已经**包含数据。
在修复后的流程中，`connect()` 之后才调用 `AddIoWriteEvent`，此时编码已在前一步完成 → pWaitForSendBuff 有数据 ✅。
