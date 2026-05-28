# AsioUringIoBackend 三路驱动架构详解

> 2026-05-17 | 适用于 asio_uring 后端核心理解

---

## 一、背景：三路驱动的设计动机

### 1.1 问题：Asio 如何与 libev 集成？

AsioUringIoBackend 的核心挑战是：**如何让 Boost.Asio 的 io_context 嵌入 libev 的事件循环？**

```
┌─────────────────────────────────────────────┐
│           libev 主事件循环                    │
│  while (!done) {                            │
│      ev_prepare 回调                        │
│      epoll_wait()  ← 阻塞等待               │
│      处理返回的事件（业务 fd）                │
│      ev_check 回调                          │
│  }                                          │
└─────────────────────────────────────────────┘
         ↑ libev 管理
         ↓
┌─────────────────────────────────────────────┐
│        Asio io_context                      │
│  - 接收业务提交的 async_read/write          │
│  - 攒 SQE → io_uring_enter 提交           │
│  - poll() 收割 CQE → completion lambda     │
└─────────────────────────────────────────────┘
         ↑ Asio 管理
```

### 1.2 解决方案：libev 的 prepare/check 钩子

libev 提供了三个特殊的 watcher 类型，专门用于在事件循环的关键时机插入回调：

| Watcher | 触发时机 | 典型用途 |
|---------|----------|----------|
| **ev_prepare** | `epoll_wait()` **之前** | 准备数据、提交工作 |
| **ev_io** | 文件描述符可读/可写 | 处理 I/O 事件 |
| **ev_check** | `epoll_wait()` **返回之后** | 善后处理、补充操作 |

---

## 二、三路驱动的精确时序

### 2.1 libev 事件循环的完整流程

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                     libev 事件循环（单次迭代）                                  │
├──────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌──────────────────────────────┐                                           │
│  │ [第 1 路] ev_prepare 触发     │ ← libev 在每次迭代开始时自动调用            │
│  │                              │                                           │
│  │ 调用 OnPrepare():            │                                           │
│  │   → io_context.poll()        │ ← ASIO 提交 SQE + 收割 CQE                 │
│  │   → 批量 io_uring_enter()   │ ← N 个操作 → 1 次 syscall                  │
│  │   → 触发 completion lambda  │ ← 减少延迟，提前处理已完成的 I/O            │
│  └──────────────────────────────┘                                           │
│                ↓                                                            │
│  ┌──────────────────────────────┐                                           │
│  │ [主循环] epoll_wait()        │ ← 阻塞等待 I/O 事件（业务 fd、timer）      │
│  │                              │                                           │
│  │ 返回条件：                    │                                           │
│  │   - 业务 fd 可读/可写         │ ← 新连接、客户端数据到达                    │
│  │   - timer 超时              │ ← Step 心跳、超时                           │
│  │   - ring_fd 可读            │ ← 内核通知有 CQE 完成                       │
│  └──────────────────────────────┘                                           │
│                ↓                                                            │
│  ┌──────────────────────────────┐                                           │
│  │ 处理返回的事件                 │ ← 遍历触发 ev_io 回调                    │
│  │   → EvIoBackend (业务 fd)    │ ← 接收连接、处理请求                        │
│  │   → timer watcher           │ ← 触发 Step 超时                           │
│  │   → OnRingReady (ring_fd)   │ ← 内核通知有新 CQE                         │
│  │                              │                                           │
│  │ [第 2 路] OnRingReady 触发   │ ← ring_fd 可读时调用（按需启停）           │
│  │   → io_context.poll()       │ ← 收割内核通知的 CQE                       │
│  │   → 触发 completion lambda  │ ← 分发到业务层                             │
│  └──────────────────────────────┘                                           │
│                ↓                                                            │
│  ┌──────────────────────────────┐                                           │
│  │ [第 3 路] ev_check 触发      │ ← libev 在每次迭代结束时自动调用           │
│  │                              │                                           │
│  │ 调用 OnCheck():              │                                           │
│  │   → io_context.poll()       │ ← 补刀：收割 race window 中新到的 CQE      │
│  │   → UpdateRingWatcher()     │ ← 无挂起 op 则停 ring_fd 监听（防空转）    │
│  │   → 诊断统计输出             │ ← 每秒输出一次诊断                         │
│  └──────────────────────────────┘                                           │
│                ↓                                                            │
│  回到第 1 步，进入下一次迭代                                                   │
└──────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 为什么需要"补刀"收割？

**Race Window 问题：**

```
时间线：
  T1: 内核写入 CQE 到 CQ Ring
  T2: epoll_wait() 返回（此时 T1 已发生，但 epoll 不感知 CQE）
  T3: 遍历处理业务事件（业务 fd 先于 ring_fd 被处理）
  T4: OnRingReady() 收割 CQE
  
  ⚠️ CQE 在 T1 已到达，但被延迟到 T4 才收割
  ⚠️ 如果没有 ev_check，T4 和下一次 OnRingReady 之间的 CQE 会延迟到下一轮
```

**ev_check 的作用：**
- 在 `epoll_wait()` 返回**之后**立即收割，不等 `OnRingReady`
- 确保 CQE 被尽快处理，减少延迟
- 防止 CQE 在 `OnRingReady` 和下一次 `epoll_wait` 之间"迟到"

---

## 三、三路驱动的代码映射

### 3.1 数据结构声明

```cpp
// code/Net/src/labor/AsioUringIoBackend.hpp (第162-163行)
struct AsioUringIoBackend {
    // ...
    ev_prepare m_prepare{};      // [三路-第 1 路] epoll_wait 前: 投递 SQE + 抢先收割 CQE
    ev_check   m_check{};        // [三路-第 3 路] epoll_wait 后: 补刀收割 + 善后诊断
    ev_io      m_ringWatcher{};  // [三路-第 2 路] ring_fd 可读: 收割内核通知的 CQE
    // ...
};
```

### 3.2 初始化代码

```cpp
// code/Net/src/labor/AsioUringIoBackend.cpp (第251-257行)

// 第 1 路和第 3 路 always-on — 即使无 op 也要运行（准备投递 + 兜底收割）
ev_prepare_init(&m_prepare, &OnPrepare);
m_prepare.data = this;
ev_prepare_start(loop, &m_prepare);

ev_check_init(&m_check, &OnCheck);
m_check.data = this;
ev_check_start(loop, &m_check);

// 第 2 路按需启停 — 有挂起 op(readPending||writePending) 才 start
// 无则 stop，阻止 ASIO NOP-SQE 产生的伪 CQE 在 idle 时唤醒 epoll 造成忙循环
```

### 3.3 三个回调的实现

#### [第 1 路] OnPrepare

```cpp
// code/Net/src/labor/AsioUringIoBackend.cpp (第463-470行)
void AsioUringIoBackend::OnPrepare(struct ev_loop*, ev_prepare* w, int)
{
    auto* be = static_cast<AsioUringIoBackend*>(w->data);
    
    // 关键：poll() 内部完成两件事：
    //   1. 提交 SQE — ASIO 攒下的 async ops → io_uring_enter 批量提交
    //   2. 收割 CQE — poll() 同时检查并收割已到的 CQE → 触发 completion lambda
    auto n = be->m_ioCtx.poll();
    
    if (n > 0) {
        diag_log("[IODIAG AsioUring OnPrepare poll=%zu\n", n);
        ++g_stats.prepare_real;
    }
}
```

#### [第 2 路] OnRingReady

```cpp
// code/Net/src/labor/AsioUringIoBackend.cpp (第496-505行)
void AsioUringIoBackend::OnRingReady(struct ev_loop*, ev_io* w, int)
{
    auto* be = static_cast<AsioUringIoBackend*>(w->data);
    auto n = be->m_ioCtx.poll();
    
    ++g_stats.ring_ready;
    if (n == 0) ++g_stats.ring_empty;  // 空唤醒: poll() 只处理了 NOP/中断 CQE
    else        ++g_stats.ring_real;   // 真实完成: 有用户 op 的 CQE 被收割
    
    diag_log("[IODIAG AsioUring OnRingReady ring_fd=%d poll=%zu\n", be->m_ringFd, n);
}
```

#### [第 3 路] OnCheck

```cpp
// code/Net/src/labor/AsioUringIoBackend.cpp (第479-488行)
void AsioUringIoBackend::OnCheck(struct ev_loop*, ev_check* w, int)
{
    auto* be = static_cast<AsioUringIoBackend*>(w->data);
    auto n = be->m_ioCtx.poll();
    
    if (n > 0) {
        diag_log("[IODIAG AsioUring OnCheck poll=%zu\n", n);
        ++g_stats.check_real;
    }
    
    be->UpdateRingWatcher();  // 收割完后，无挂起 op 则停 ring_fd 监听
    g_stats.tick();           // 每秒一次诊断输出
}
```

---

## 四、关键设计点

### 4.1 为什么第 2 路要按需启停？

**问题：ASIO NOP-SQE 产生的伪 CQE**

当 io_context 没有挂起的 I/O 操作时，ASIO 可能会提交 NOP SQE 来保持 ring_fd 的通知机制。这会产生"伪 CQE"，导致：

```
无业务 I/O 时：
  1. ASIO 提交 NOP SQE → 内核返回 NOP CQE
  2. ring_fd 持续可读
  3. epoll_wait 每次都返回 ring_fd 事件
  4. OnRingReady 被频繁调用 → CPU 空转
```

**解决方案：UpdateRingWatcher()**

```cpp
void AsioUringIoBackend::UpdateRingWatcher() {
    bool hasOp = readPending || writePending;
    
    if (hasOp && !m_ringWatcherActive) {
        ev_io_start(m_loop, &m_ringWatcher);
        m_ringWatcherActive = true;
    } else if (!hasOp && m_ringWatcherActive) {
        ev_io_stop(m_loop, &m_ringWatcher);
        m_ringWatcherActive = false;
    }
}
```

**效果：**
- 有挂起 I/O → 启动 ring_fd 监听 → 收割真实 CQE
- 无挂起 I/O → 停止 ring_fd 监听 → 不响应伪 CQE → 不空转

### 4.2 为什么第 1 路和第 3 路 always-on？

**第 1 路（ev_prepare）：**
- 需要在 `epoll_wait()` 之前提交 SQE，否则 I/O 操作会延迟到下一轮
- 即使当前没有挂起操作，也需要运行（检查是否有新提交的 async op）

**第 3 路（ev_check）：**
- 作为"补刀"机制，确保所有 CQE 都能被及时收割
- 执行诊断统计和 watcher 状态管理

### 4.3 为什么需要"抢先收割"？

**减少延迟的关键：**

```
传统方式（无 OnPrepare 抢先）：
  epoll_wait 返回 → 处理业务逻辑 → poll() 收割 CQE
  ↑ CQE 早在 epoll_wait 期间就到达，但被延迟收割

三路方式（有 OnPrepare 抢先）：
  OnPrepare poll() → 抢先收割 epoll_wait 期间的 CQE
  epoll_wait 返回 → 处理业务逻辑 → CQE 已被处理
  ↑ 减少延迟：提前处理已完成的 I/O
```

---

## 五、架构优势总结

### 5.1 与 ev 对比

| 维度 | ev | AsioUringIoBackend（三路驱动） |
|------|:--:|:--:|
| 提交方式 | 每个 read/write 一次 syscall | 批量提交 N 个 → 1 次 syscall |
| 收割方式 | 每个事件一次回调 | poll() 批量收割 + completion lambda |
| CQE 收割时机 | epoll_wait 返回后 | OnPrepare（抢先）+ OnCheck（补刀） |
| CPU 空转 | 无（epoll 阻塞） | 无（通过 UpdateRingWatcher 防止） |

### 5.2 核心价值

1. **零后台线程** — ASIO io_context 完全嵌入 libev 主循环
2. **批量提交** — N 个 I/O 操作 → 1 次 io_uring_enter syscall
3. **抢先收割** — 在 epoll_wait 之前就处理已完成的 CQE
4. **防止空转** — 按需启停 ring_fd 监听，避免伪 CQE 造成空循环

---

## 六、相关代码文件

| 文件 | 行号 | 内容 |
|------|------|------|
| `code/Net/src/labor/AsioUringIoBackend.hpp` | 162-163 | 三路 watcher 声明 |
| `code/Net/src/labor/AsioUringIoBackend.cpp` | 251-257 | 三路 watcher 初始化 |
| `code/Net/src/labor/AsioUringIoBackend.cpp` | 463-470 | OnPrepare 实现（第 1 路） |
| `code/Net/src/labor/AsioUringIoBackend.cpp` | 479-488 | OnCheck 实现（第 3 路） |
| `code/Net/src/labor/AsioUringIoBackend.cpp` | 496-505 | OnRingReady 实现（第 2 路） |

---

*文档生成时间：2026-05-17*
