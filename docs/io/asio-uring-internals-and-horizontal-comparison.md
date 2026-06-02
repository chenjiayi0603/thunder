# AsioUringIoBackend 内部设计与横向对比

> 2026-05-22 | asio_uring 架构剖析 + nginx/envoy 同类项对比

---

## 一、AsioUringIoBackend 内部设计

### 1.1 整体架构：三路驱动模型

AsioUringIoBackend 没有后台线程——ASIO 的 `io_uring_service` 嵌入 libev 主事件循环，通过三个 watcher 协同驱动 SQE 提交和 CQE 收割：

```
                   ┌─────────────────────────────────────────────────┐
                   │              libev 事件循环                      │
                   │                                                 │
    SubmitRead ──→ │  ev_prepare (每次 epoll_wait 之前)               │
    SubmitWrite ─→ │    │ io_context.poll():                          │
   (只构造 SQE,    │    │   ① 攒下的 async_read/write → SQE 入队      │
    不提交)        │    │   ② io_uring_enter() → 批量提交到内核 SQ    │
                   │    │   ③ 收割已到 CQE → 触发 lambda → m_callback │
                   │    ▼                                            │
                   │  epoll_wait (libev 阻塞)                         │
                   │    │ 唤醒源: ring_fd 可读 / 业务 fd / timer      │
                   │    ▼                                            │
                   │  ev_io(ring_fd) → OnRingReady                   │
                   │    │ ring_fd 可读 → io_context.poll()            │
                   │    │ → 批量收割 CQE → lambda → m_callback        │
                   │    │ → UpdateRingWatcher (无挂起 op 则 stop)     │
                   │    ▼                                            │
                   │  ev_check (每次 epoll_wait 之后)                 │
                   │    │ 再次 poll() — 收割 race window 中的 CQE     │
                   │    │ UpdateRingWatcher + 诊断统计                │
                   │    ▼                                            │
                   │  invoke_pending — 统一执行 pending 回调           │
                   └─────────────────────────────────────────────────┘
```

**“三路”是什么**

“三路”指嵌入 libev 事件循环的**三个 libev watcher**，在事件循环的不同阶段各司其职：

```
libev 事件循环的每次迭代:
                    │
            ┌───────▼────────┐
            │ ① ev_prepare   │ ← 第 1 路: "投递" — 把积攒的 I/O 请求批量提交到内核
            └───────┬────────┘
                    │
            ┌───────▼────────┐
            │ ② epoll_wait   │ ← libev 内部: 阻塞等待任意 fd 就绪
            └───────┬────────┘
                    │
            ┌───────▼────────┐
            │ ③ ev_io        │ ← 第 2 路: "接货" — ring_fd 可读 → 收割内核完成的 CQE
            │  (ring_fd)     │   (也可能触发其他 ev_io: 业务 fd accept, timer 等)
            └───────┬────────┘
                    │
            ┌───────▼────────┐
            │ ④ ev_check     │ ← 第 3 路: "补刀" — 收割 ③ 之后新到的 CQE，善后
            └───────┬────────┘
                    │
                  循环
```

**一句话**：三路 = `ev_prepare`（投递 SQE）+ `ev_io(ring_fd)`（接货 CQE）+ `ev_check`（补刀 + 善后）。三个 watcher 分别挂在 libev 事件循环的三个固定回调点，驱动 io_uring 的完整生命周期。

**为什么必须是三路**

io_uring 有两个操作方向——**提交**（向内核发送 I/O 请求）和**收割**（从内核取回 I/O 结果）。它们拆在不同阶段：

| 如果我只有… | 能做什么 | 做不了什么 |
|-----------|---------|-----------|
| 只有 `ev_io(ring_fd)` | 收割 CQE（ring_fd 可读 = 有结果） | **提交 SQE**——还没提交请求，内核怎么可能产生结果？死锁 |
| 只有 `ev_prepare` + `ev_io` | 提交 + 收割 | 收割后到下一次 `epoll_wait` 之间新到的 CQE 会**延迟到下一轮**才处理 |
| **三路都有** ✅ | 提交 + 即时收割 + 补刀收割 | — |

**用快递类比**：
- `SubmitRead`/`SubmitWrite` = 你把快递单写好，放在桌上（写入内存 SQ，不发车）
- `ev_prepare` = 快递员定时来收件，一车拉走（批量 `io_uring_enter`）
- `ev_io(ring_fd)` = 快递到了，门铃响（ring_fd 可读 → 收割 CQE）
- `ev_check` = 开门后发现门口还有一箱刚到的，顺手拿进来（补刀收割 race window 中的 CQE）

### 1.2 SubmitRead/SubmitWrite：只构造 SQE，不提交

```cpp
// SubmitRead 核心流程 (AsioUringIoBackend.cpp:290-339)
bool AsioUringIoBackend::SubmitRead(int fd, shared_ptr<CBuffer> buf, uint32_t seq)
{
    auto& sp = EnsureFdState(fd);           // ① 查/建 FdState
    if (sp->readPending) return true;       // ② 防重入

    sp->sock.async_read_some(               // ③ ASIO 内部生成 SQE，存入内存 SQ
        asio::buffer(dst, cap),
        [this, wp, fd, seq, buf](ec, n) {   // ④ lambda 闭包持有 weak_ptr<FdState>
            auto live = wp.lock();
            if (!live || live->cancelled) return;  // fd 已关闭 → 丢弃
            buf->AdvanceWriteIndex(n);
            m_callback(fd, seq, IoOp::Read, n, m_userData);
        });
    // 不调 poll() — SQE 由下一个 OnPrepare 批量提交
    UpdateRingWatcher();
}
```

关键设计点：
- **`async_read_some`/`async_write_some`** 内部调用 ASIO 的 `io_uring_service`，将操作转化为 SQE 写入内核共享内存的 SQ 区——**这是一次内存写入，零系统调用**
- **不立即 `io_uring_enter`**：SQE 留在 SQ 中，等到 `OnPrepare` 时统一通过一次 `io_uring_enter` 批量提交
- **lambda 用 `weak_ptr<FdState>`**：`CancelFd` 只需 erase map entry，lambda 中 `wp.lock()` 失败则自动丢弃——无需显式等待异步操作完成

### 1.3 OnPrepare：批量提交 + 抢先收割

```cpp
// ev_prepare 回调 — epoll_wait 之前执行
void OnPrepare(ev_loop*, ev_prepare* w, int)
{
    auto* self = (AsioUringIoBackend*)w->data;
    self->m_ioCtx.poll();  // ← 一次调用完成三件事:
    // ① 将 SQ 中积攒的所有 SQE 通过 io_uring_enter() 批量提交到内核
    // ② 收割内核已完成的 CQE（如果有）
    // ③ 对每个已完成的 CQE，调用对应的 completion lambda
}
```

**批量提交的意义**：假设本轮事件循环有 N 个 `SubmitRead`/`SubmitWrite` 调用，ev 模型需要 N 次 `read()`/`write()` 系统调用，native_uring 需要 N 次 `io_uring_submit()`。asio_uring 只需 **1 次** `io_uring_enter`——在 `OnPrepare` 中一次性提交全部 N 个 SQE。

### 1.4 OnRingReady：CQE 到达通知

```cpp
// ev_io(ring_fd) 回调 — 内核通知有新 CQE
void OnRingReady(ev_loop*, ev_io* w, int)
{
    auto* self = (AsioUringIoBackend*)w->data;
    self->m_ioCtx.poll();  // 批量收割 CQE → 触发 lambda → m_callback
    self->UpdateRingWatcher();
}
```

**按需启停 ring_fd 监听**（`UpdateRingWatcher`）：ASIO 的 waitable reactor 通过 NOP-SQE 在 io_uring 中注册 ring_fd 的 epoll 可读性。即使没有用户 I/O 操作，内核也可能因中断、定时器等产生内部 CQE，导致 ring_fd 变为可读 → epoll_wait 被唤醒 → `poll()=0` 空转。

`UpdateRingWatcher` 的解决方案：有挂起 op（`readPending || writePending`）才 `ev_io_start(ring_fd)`，无挂起 op 则 `ev_io_stop(ring_fd)`——消除 idle 场景下的空唤醒。

### 1.5 OnCheck：补刀收割 + 善后

```cpp
// ev_check 回调 — epoll_wait 之后执行
void OnCheck(ev_loop*, ev_check* w, int)
{
    auto* self = (AsioUringIoBackend*)w->data;
    self->m_ioCtx.poll();  // 收割 OnRingReady 与 epoll_wait 之间的 race window CQE
    self->UpdateRingWatcher();
    g_stats.tick();        // 每秒输出诊断统计
}
```

### 1.6 FdState：weak_ptr 生命周期管理

```
FdState 结构:
┌────────────────────────────────┐
│ asio::posix::stream_descriptor │ ← ASIO socket 封装
│ bool readPending               │ ← 防重入
│ bool writePending              │
│ bool cancelled                 │ ← CancelFd 标记
└────────────────────────────────┘
    ↑ shared_ptr 持有 (m_fds map)
    ↑ weak_ptr   持有 (completion lambda 闭包)

CancelFd 流程:
  ① sp->cancelled = true
  ② sp->sock.release() — 释放 ASIO 内部 fd 引用
  ③ m_fds.erase(fd)   — 移除 map entry
  ④ 最后一个 lambda 释放 weak_ptr → FdState 自动析构
```

**不调 `sock.cancel()` 的原因**：`sock.cancel()` 提交 `IORING_OP_ASYNC_CANCEL`，这是一个异步操作。若 `CancelFd` 后立即 `SubmitRead`/`SubmitWrite` 复用同一 fd，cancel op 可能意外取消新注册的操作。改为 `release() + cancelled 标记`——老 lambda 通过 `cancelled` 标志丢弃，新操作不受影响。

### 1.7 Write 双路径：Fixed Buffer vs 普通写

```
SubmitWrite
    │
    ├─ [FixedBuf 路径] m_fixedBufEnabled && data ≤ slotSize && 有空槽
    │    → memcpy 到预注册缓冲
    │    → sock.async_write_some(registered_buffer) 
    │    → 内核 DMA 直接读写注册内存 (减少一次内核拷贝)
    │
    └─ [普通路径] 回退
         → sock.async_write_some(CBuffer 原始指针)
         → 零额外 memcpy（直接用 CBuffer 的原始内存）
```

Fixed Buffer 通过 `THUNDER_ASIO_URING_FIXEDBUF=1` 环境变量启用，池大小可配（默认 256 slots × 64KB）。池满或数据超长时自动回退普通路径。

---

## 二、三项结构优势的深度分析

### 优势 1：零 per-op 堆分配 — ASIO recycling_allocator

```
每个 I/O 操作的内存分配路径:

ev (Thunder):   CBuffer 复用（连接级），ev_io watcher 复用（连接级）
                代码路径无 per-op 堆分配

native_uring:   new PendingOp{fd, seq, buf, isZc...} → 使用 → delete PendingOp
                每次 I/O: ~50-200ns malloc + ~50-200ns free = 100-400ns
                小包 I/O 总耗时 ~700ns → 堆分配占比 15-57%
                大包 I/O 总耗时 ~9ms → 堆分配占比 <0.01%

asio_uring:     ASIO 内部 recycling_allocator<async_read/write_op>
                首次分配 → 放入链表 → 使用 → 归还链表（不 free）
                热路径零堆分配（仅首次冷启动时有）
                小包 I/O 总耗时 ~700ns → 堆分配占比 0%
```

**实际影响**：这是 asio 小包持平 ev、native 小包亏 8% 的**直接根因**。

### 优势 2：批量系统调用 — SQ 攒批 + CQ 批量收割

```
ev 模型:
    for each ready fd:
        read(fd, buf, len)   ← 每次都是 syscall
        write(fd, buf, len)  ← 每次都是 syscall
    N 个操作 = N 次 syscall

native_uring:
    SubmitRead: io_uring_submit()  ← 每次 syscall
    SubmitWrite: io_uring_submit() ← 每次 syscall
    N 个操作 = N 次 syscall (io_uring_enter)

asio_uring:
    SubmitRead: 写入内存 SQ（非 syscall）
    SubmitWrite: 写入内存 SQ（非 syscall）
    ...
    OnPrepare: io_uring_enter(N 个 SQE)  ← 1 次 syscall 提交全部
    OnRingReady: poll() 收割 N 个 CQE  ← 1 次 syscall 收割全部
    N 个操作 = 2 次 syscall
```

**系统调用成本**：每次 `io_uring_enter`/`read`/`write` 涉及用户态↔内核态切换（~50-100ns）+ Spectre/Meltdown 缓解开销（~100-300ns）。N=1000 时，ev 支付 1000×~300ns=300μs，asio 支付 2×~300ns=600ns。

### 优势 3：单 ring_fd 等待 — 无 per-fd 事件注册

```
epoll 模型 (nginx/envoy/Thunder ev):
    epoll_ctl(EPOLL_CTL_ADD, fd1)    ← per-fd 注册
    epoll_ctl(EPOLL_CTL_ADD, fd2)    ← 每连接建立时执行
    ...
    epoll_wait(epfd, ...)            ← 等待 epoll 实例
    → 返回就绪 fd 列表，逐个处理

io_uring 模型 (asio_uring):
    SubmitRead(fd1) → SQE 写入共享内存 SQ  ← 替代 epoll_ctl ADD
    SubmitRead(fd2) → SQE 写入共享内存 SQ
    ...
    poll(ring_fd) ← 等待一个 fd           ← 替代 epoll_wait
    → 收割 CQE 列表，逐个处理
    
    差异:
    - epoll: 1 个 epoll fd + N 个 per-fd 注册/注销操作
    - io_uring: 1 个 ring_fd（SQ/CQ 共用），无 per-fd 注册
```

**为什么这很重要**：
- 连接建立时：epoll 需要 `epoll_ctl(ADD)` 系统调用，io_uring 只需写内存 SQ
- 连接关闭时：epoll 需要 `epoll_ctl(DEL)` 系统调用，io_uring 无此操作
- **idle 友好**的核心含义：10 万 idle 连接在 epoll 下仍然存在于 epoll 实例中（占用内核内存，但不触发事件）；在 io_uring 下 idle 连接不占任何事件注册——只有活跃连接才有 SQE 在 SQ 中。当数据到达时，内核将 CQE 写入共享内存 CQ 并标记 ring_fd 可读，用户态只等一个 ring_fd

---

## 三、横向对比：nginx、envoy、Thunder (ev/asio/native)

### 3.1 对比矩阵

| 维度 | nginx | envoy | Thunder ev | Thunder asio_uring | Thunder native_uring |
|------|-------|-------|-----------|-------------------|---------------------|
| **I/O 多路复用** | epoll | epoll (libevent) | epoll (libev) | io_uring (ASIO + libev 三路) | io_uring (liburing + libev) |
| **事件通知模型** | per-fd epoll | per-fd epoll | per-fd epoll | 单 ring_fd | eventfd + ev_io |
| **per-op 堆分配** | ❌ 连接池预分配 | ❌ Buffer 池 | ❌ CBuffer 复用 | ❌ ASIO 对象池 | ✅ `new PendingOp()` |
| **系统调用模式** | 每操作 read/write | 每操作 read/write | 每操作 read/write | 批量 io_uring_enter | 每操作 io_uring_submit |
| **SQ/CQ 分离** | — | — | — | ✅ 共享内存 SQ/CQ | ✅ 共享内存 SQ/CQ |
| **零拷贝 sendfile** | ✅ sendfile | ❌ (用户态拷贝) | ❌ | 可选 (fixed buf 地基) | ✅ send_zc |
| **连接模型** | per-worker 单线程 | per-worker 多线程 | per-worker 单线程 | per-worker 单线程 | per-worker 单线程 |
| **fd 注册开销** | epoll_ctl per fd | epoll_ctl per fd | epoll_ctl per fd | 无（SQE 即注册） | hash map + SQE |
| **idle 连接管理** | epoll 中存在 | epoll 中存在 | epoll 中存在 | ring_fd + 按需启停 | ring_fd + eventfd |
| **seccomp 复杂度** | 低（4 个 syscall） | 低（~10 个 syscall） | 低（4 个 syscall） | 中（~5 个 + io_uring_*） | 中（~5 个 + io_uring_*） |

### 3.2 nginx 对比分析

**nginx I/O 模型**：
```
worker 进程事件循环:
    for ( ;; ) {
        ngx_process_events(cycle, timer, flags);  // epoll_wait 一次
            → 遍历就绪事件链表
            → 对每个就绪事件调用 read/write handler
            → handler 中直接 read()/write()（同步，非阻塞）
        
        // I/O 层之下有完整的连接池
        ngx_get_connection(s) → 从 free_connections 链表取预分配 ngx_connection_t
    }
```

**关键设计**：
- `ngx_connection_t` 在启动时预分配（`cycle->connections`），从空闲链表取/还——**零 per-connection 堆分配**
- `ngx_buf_t` 链式缓冲，支持 sendfile/sendfilev 零拷贝
- 事件处理是同步的：读就绪 → `recv()` 直到 EAGAIN；写就绪 → `send()` 直到 EAGAIN

**与 asio_uring 的差异**：

| 方面 | nginx | asio_uring |
|------|-------|-----------|
| 事件通知 | epoll_wait → 遍历就绪链表 → 逐个 handler | 单 ring_fd → 收割 CQE 列表 → 逐个 lambda |
| I/O 执行 | read/write 同步 | async_read/write_some 异步（CQE 回调） |
| per-fd 管理 | epoll_ctl ADD/MOD/DEL | SQE 写入内存 SQ（动态注册） |
| 批量操作 | 不支持 | SQE 攒批 → io_uring_enter 一次提交 |
| 零拷贝 | sendfile (file→socket) | fixed buffer (memcpy 减少) + send_zc 地基 |

**nginx 能用 io_uring 吗？** 可以，但需要重大重构——nginx 的同步 I/O 模型与 io_uring 的异步 CQE 模型不兼容。nginx 社区已有讨论但尚未合并主线。

### 3.3 envoy 对比分析

**envoy I/O 模型**：
```
worker 线程事件循环:
    LibeventScheduler::run(dispatcher):
        event_base_loop() → epoll_wait
            → 对每个就绪 fd，调用 libevent callback
            → callback 中:
                IoHandle::read()  → recvmsg/sendmsg（同步）
                或
                FileEventImpl::mergeInBuffer → Buffer::OwnedImpl
                    → 分配新 Buffer (WatermarkFactory)
                    → Reservation 机制减少拷贝
```

**关键设计**：
- Buffer 池（`WatermarkBufferFactory` / `BufferMemoryAccount`）减少 per-request 内存分配
- 同步 I/O：`Api::IoHandle::read()` 直接调 `recvmsg()`
- 无 io_uring 支持——所有 I/O 走标准 socket syscall
- 多线程模型：每个 worker 线程独立的事件循环

| 方面 | envoy | asio_uring |
|------|-------|-----------|
| 事件库 | libevent (epoll 后端) | libev + ASIO io_uring_service |
| I/O 操作 | recvmsg/sendmsg 同步 | io_uring async ops + CQE |
| 缓冲管理 | Buffer pool (WatermarkFactory) | CBuffer + ASIO 对象池 |
| 零拷贝 | ❌ (Buffer 拷贝) | fixed buffer 路径 |
| 线程模型 | per-worker 多线程 | per-worker 单线程（同 ev） |
| 生态定位 | L7 代理，全功能数据面 | L4 I/O 层，可嵌入任意上层 |

**envoy 能用 io_uring 吗？** envoy 的 IoHandle 抽象层支持替换后端。已有社区实验将 `IoHandleImpl` 改为 io_uring，但要完整支持需要改 libevent → io_uring 的事件循环——改动量很大。

### 3.4 核心差异总结

```
                      nginx                envoy                 Thunder asio_uring
                      ─────                ─────                 ──────────────────
事件模型              epoll per-fd         epoll per-fd          io_uring 单 ring_fd
                      
每个 I/O 的系统调用    1 read/write         1 recvmsg/sendmsg     0（已在内存 SQ 中）

批量提交能力          ❌                   ❌                    ✅ io_uring_enter(N SQE)

per-op 堆分配          ❌ (连接池)           ❌ (Buffer 池)        ❌ (ASIO 对象池)

per-fd 注册开销        epoll_ctl (syscall)  epoll_ctl (syscall)   无 (SQE 内存写)

idle 连接代价          epoll 实例中存在      epoll 实例中存在       ring_fd 仅在有 op 时监听

零拷贝                 sendfile             ❌                    fixed buf + send_zc 地基

代码复杂度             中 (~2万行事件模块)  高 (~10万行 I/O 层)   低 (~550行)
```

---

## 四、结论

**asio_uring 不是"更快版的 epoll"，而是一个架构范式转换**：

1. **从 per-fd 事件通知 → 单 ring_fd 等待**：epoll 的 O(N) per-fd 注册/注销被 SQE 的内存写入取代，连接数越大优势越显著

2. **从同步 I/O → 异步 CQE 模型**：`read()`/`write()` 系统调用被提前写入内存 SQ，内核完成时通过 CQE 回调——批量提交将 N 次系统调用压成 1 次

3. **从显式生命周期管理 → weak_ptr 自动回收**：`FdState::cancelled` + `weak_ptr` 使得 `CancelFd` 无需等待异步操作完成，lambda 自然失效

4. **保留 libev 兼容性**：三路驱动模型（prepare/io/check）使得 asio_uring 无缝嵌入现有 libev 事件循环，定时器、信号、Manager 通信等不依赖 io_uring 的组件无需修改

**与 nginx/envoy 相比**：Thunder 作为较新的项目，没有历史包袱，可以直接采用 io_uring 作为一等公民。nginx 和 envoy 的同步 epoll 模型各有优点（确定性高、调试简单、生态成熟），但都面临"如何渐进式迁移到 io_uring"的工程挑战。

---

*代码引用：*
- `code/Net/src/labor/AsioUringIoBackend.hpp` — 三路驱动架构说明（行 1-63）
- `code/Net/src/labor/AsioUringIoBackend.cpp` — `OnPrepare`(行 439-451), `OnRingReady`(行 453-464), `OnCheck`(行 453-464), `SubmitRead`(行 290-339), `SubmitWrite`(行 342-435), `CancelFd`(行 419-435), `UpdateRingWatcher`(行 114-134), `InitFixedBuffers`(行 141-167)
- `code/Net/src/labor/NativeUringIoBackend.cpp` — PendingOp 堆分配路径, send_zc 双 CQE
- `code/Net/src/labor/Worker.cpp` — `AddIoReadEvent`(行 5030), `AddIoWriteEvent`(行 5075)
