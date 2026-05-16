# Thunder 框架 io_uring 使用与原理分析

## 摘要

本文档分析 Thunder 框架中 io_uring 的集成设计。Thunder 采用 C++20 开发，核心事件循环基于 libev，通过 IoBackend 抽象接口支持三种 I/O 后端：

- **EvIoBackend**：基于 epoll 的传统实现，稳定可靠，作为基线对照
- **AsioUringIoBackend**：基于 standalone Asio io_uring 后端
- **NativeUringIoBackend**：原生 liburing 后端（Path B），自管 SQ/CQ，eventfd + ev_io 收割 CQE，支持 SQPOLL 和 send_zc

文档重点回答三个问题：**io_uring 解决什么问题**、**为什么先走 Asio 后来选原生**、**各后端内部如何工作**。

> **关键结论（v3.1，2026-05-16 实测落地）**：原生后端对 ev 在 64KB 写有 +7.6% RPS / −54% 延迟的真实收益（asio_uring 对 ev≈持平），精简控制路径是收益主因。详见 Part 5。

---

## 第一部分：io_uring 基础原理

### 1.1 io_uring 是什么

io_uring 是 Linux 5.1（2019年3月）引入的异步 I/O 接口，由 Jens Axboe 设计。其核心目标是通过共享内存 ring buffer 减少用户态与内核态之间的往返次数。

传统模型的问题：

| 模型 | 问题 |
|------|------|
| 同步 read/write | 阻塞，CPU 等待 |
| epoll + non-blocking | 每次 I/O 仍需多次 syscall（epoll_ctl + read + 可能的重试） |
| POSIX AIO (glibc) | 功能残缺，实际是线程池模拟 |

io_uring 通过 mmap 共享内存消除 **控制路径**的拷贝开销：

```
用户态                              内核态
  │                                   │
  │  SQ (Submission Queue)            │
  │  ┌─────────────────────┐          │
  │  │ SQE: op/fd/buf/len  │──────────► 内核执行 I/O
  │  └─────────────────────┘          │
  │                                   │
  │  CQ (Completion Queue)            │
  │  ┌─────────────────────┐          │
  │  │ CQE: user_data/res  │◄──────────  完成通知
  │  └─────────────────────┘          │
  │                                   │
  └─ SQE/CQE 元数据通过 mmap 共享，无需拷贝 ─┘

注意：消除的是控制元数据（SQE/CQE）的拷贝，不是业务数据本身。
业务数据仍走：用户 buffer ↔ 内核 socket buffer 的 memcpy。
真正的数据零拷贝需要 Fixed Buffers / Provided Buffers / MSG_ZEROCOPY（见第六部分）。
```

### 1.2 核心数据结构

**SQE（提交条目）**：

```c
struct io_uring_sqe {
    __u8   opcode;    // IORING_OP_RECV / IORING_OP_SEND 等
    __s32  fd;
    __u64  addr;      // 用户缓冲区地址
    __u32  len;
    __u64  user_data; // 用于关联请求与结果（Thunder 用 seq）
};
```

**CQE（完成条目）**：

```c
struct io_uring_cqe {
    __u64  user_data; // 与 SQE 对应
    __s32  res;       // 字节数，或负的 errno
};
```

### 1.3 两种工作模式

| 模式 | 机制 | Thunder 使用 |
|------|------|------------|
| **中断驱动**（默认）| 提交需 `io_uring_enter()` syscall，CQ 完成通过 ring_fd 可读通知 | ✅ 当前使用 |
| **SQPOLL** | 内核线程轮询 SQ，零 syscall 提交，需 `CAP_SYS_NICE` 权限 | ❌ 未使用 |

#### 当前模式确认（代码依据）

Asio io_uring 后端硬编码 `flags=0`（`asio/detail/impl/io_uring_service.ipp:532`）：

```cpp
::io_uring_queue_init(ring_size, &ring_, 0);
//                                       ↑ flags=0，无 IORING_SETUP_SQPOLL
```

当前实际交互流程：

```
用户态                         内核态
  │                               │
  │  io_uring_enter()  ──────────►│  ← 每次提交一次 syscall
  │  （ev_prepare 批量合并多个 SQE）│    内核执行 I/O，写 CQE
  │                               │
  │  ring_fd 可读  ◄────────────── │  ← ring_fd 通知，ev_io(ring_fd) 触发
  │  io_context.poll() 收割 CQE   │
```

#### 为什么不用 SQPOLL

```
  ┌────────────────────────┬──────────────────────────────────────────────────────┐
  │         原因           │                       说明                           │
  ├────────────────────────┼──────────────────────────────────────────────────────┤
  │ 权限问题               │ 需要 CAP_SYS_NICE 或 root                            │
  │                        │ 生产容器默认无此权限，io_uring_setup() 返回 EPERM     │
  ├────────────────────────┼──────────────────────────────────────────────────────┤
  │ 空闲时白烧 CPU         │ SQPOLL 创建常驻内核线程持续轮询 SQ                    │
  │                        │ 无 I/O 时仍占 CPU，不适合大量空闲连接的场景           │
  ├────────────────────────┼──────────────────────────────────────────────────────┤
  │ Asio 不支持配置        │ Asio io_uring 后端硬编码 flags=0                     │
  │                        │ 要用 SQPOLL 需 patch Asio 或绕过 Asio 直接初始化     │
  └────────────────────────┴──────────────────────────────────────────────────────┘
```

**SQPOLL 并非真正"零 syscall"**：内核轮询线程空闲超过 `sq_thread_idle`（默认 1s）后休眠，下次提交仍需 `IORING_ENTER_SQ_WAKEUP` syscall 唤醒，仅在持续高频 I/O 期间才能做到零 syscall。

**Thunder 已通过 ev_prepare 批量合并缓解了 syscall 开销**：多个 SubmitRead/Write 积攒后一次 `io_uring_enter()` 提交，SQPOLL 能消掉的那一次 syscall 在 Thunder 场景下已不是主要瓶颈。

---

## 第二部分：Thunder IoBackend 抽象设计

### 2.1 为什么要抽象

Thunder 需要在不改动业务逻辑的前提下切换 I/O 后端，IoBackend 接口将这两层彻底解耦：

```
┌────────────────────────────────────────┐
│           Thunder Network Layer         │
│                                        │
│         IoBackend Interface            │
│    Init / SubmitRead / SubmitWrite     │
│    CancelFd / HasPending / Name        │
└──────────────┬─────────────────────────┘
               │
       ┌───────┴───────┬──────────────────┐
       ▼               ▼                  ▼
 EvIoBackend    AsioUringIoBackend   NativeUringIoBackend
 (epoll 基线)   (Asio 封装)          (原生 liburing)
```

### 2.2 接口定义

```cpp
// 实际签名（Labor.cpp 注册时使用）
using IoCompletionCallback = void(*)(
    int fd, uint32_t seq, IoOp op, int result, void* user_data
);

class IoBackend {
public:
    virtual bool Init(struct ev_loop* loop) = 0;
    virtual void Destroy() = 0;

    virtual void SubmitRead(int fd, CBuffer* buf,
                            uint32_t seq, void* user_data) = 0;
    virtual void SubmitWrite(int fd, CBuffer* buf,
                             uint32_t seq, void* user_data) = 0;

    virtual void CancelFd(int fd) = 0;
    virtual bool HasPending(int fd) const = 0;
    virtual std::string Name() const = 0;
};
```

### 2.3 三种实现对比

| 特性 | EvIoBackend | AsioUringIoBackend | NativeUringIoBackend |
|------|------------|-------------------|---------------------|
| **读机制** | ev_io watcher + ReadFD | async_read_some → io_uring_prep_recv | io_uring_prep_recv 直发 |
| **写机制** | ev_io watcher + WriteFD | async_write_some → io_uring_prep_send | io_uring_prep_send / prep_send_zc |
| **集成方式** | per-fd epoll watcher | ev_prepare/ev_check/ev_io(ring_fd) 三路驱动 | eventfd + ev_io + ev_check 兜底 |
| **线程数** | 1（主线程）| 1（主线程）| 1（主线程）|
| **syscall 模型** | 每次 read/write 一次 syscall | 批量提交，多个 SQE 合并一次 io_uring_enter | 批量提交，直收割，无 Asio 中间层 |
| **SQPOLL** | N/A | ❌ Asio 硬编码 flags=0 | ✅ env 门控（内核 7.0 免特权）|
| **send_zc** | N/A | ❌ 无支持 | ✅ 按阈值分流（bounce 版，见 §4.3）|
| **状态** | 默认，稳定 | 保留（对比参照）| 推荐 io_uring 后端 |

---

## 第三部分：Asio 到原生的选型演进

### 3.1 为什么先走 Asio

Thunder 初期选择 AsioUringIoBackend 的核心原因：**Asio 把 buffer 生命周期、取消、CQE 收割、async write 全部封装好了**。Thunder 早期的直接 liburing 尝试（`UringIoBackend`，已移除）写操作退化为同步 `send()`，正是被这些细节击沉的。

**Asio 的具体封装价值**：

```
用户代码:
  sock.async_read_some(buf, lambda);
  sock.async_write_some(buf, lambda);

Asio 内部自动完成:
  ├─ SQE 填充 (io_uring_prep_recv/send)
  ├─ seq/user_data 管理
  ├─ 批量提交优化 (submit_sqes_op)
  ├─ CQE 收割 + handler 分发
  ├─ 取消安全 (stream.close())
  └─ 缓冲区生命周期 (handler 捕获 shared_ptr)
```

### 3.2 压测发现：AsioUringIoBackend 对 ev ≈ 持平

64KB 写压测（c100，30s）：

| 后端 | RPS | 延迟 |
|------|-----|------|
| ev | 5,653 | 17.90ms |
| asio_uring | 5,473（-3%） | 9.67ms（-46%） |

延迟低但 RPS 反而略低——Little's Law 验算证实：asio_uring 的 wrk 延迟其实是 TTFB（首字节），完整 64KB 传输时间（18.3ms）略慢于 ev。64KB 场景下数据 memcpy 是硬瓶颈，io_uring 只消除了控制元数据（SQE/CQE）拷贝，数据面与 epoll 无本质差异。

### 3.3 转向原生：Asio 的三重天花板

深入分析后发现，Asio 对 io_uring 高端能力存在**架构性封锁**：

| 优化项 | Asio 支持 | 根因 |
|--------|----------|------|
| SQPOLL | ❌ | `io_uring_service.ipp` 硬编码 `flags=0`，无法传入 `IORING_SETUP_SQPOLL` |
| send_zc (MSG_ZEROCOPY) | ❌ | 无 `prep_send_zc` 接口；双 CQE 模型与 Asio 单完成回调假设冲突 |
| Provided Buffers | ❌ | 无 `IOSQE_BUFFER_SELECT`；需内核选 buffer 再通知用户，与 Asio 所有权模型互斥 |

继续走 Asio = 对每项做子模块 patch + fork，且 asio 子模块未初始化，patch 不进 git。与其打补丁绕过人家架构，不如写专用原生后端"这 3 项生而有之"。

### 3.4 拱心石发现：IO 完成不经协程

在决定走原生路线前，最关键的技术发现是：socket IO 完成回调是**纯 C 函数指针**，不经 StepCo20/Awaitable 协程。

```
IoBackend.hpp:
  using IoCompletionCallback = void(*)(int fd, uint32_t seq, IoOp op,
                                        int result, void* user_data);

Worker.cpp:1060:
OnIoComplete → HandleIoReadComplete / HandleIoWriteComplete / HandleIoWriteNotifComplete
```

→ send_zc 的双 CQE 不需要协程、不需要 awaiter、不碰 codec/ev。只是同一条 C 回调桥上**多一个事件类型**（`IoOp::WriteNotif`）+ 拆一个写完成函数。风险等级远低于当年击沉第一版的"重写状态机"。

### 3.5 选型结论

| 目标 | 选型 | 理由 |
|------|------|------|
| 只要"更快的 epoll"（Fixed Buffers + native accept/connect）| Asio | 少代码、少 bug、async write 免费 |
| 要高端天花板（SQPOLL + send_zc + Provided Buffers）| **原生** ✅ | Asio 锁死这 3 项 |
| 当前实测 | **原生** ✅ | 64KB +7.6% RPS / −54% 延迟（asio≈持平）|

**代价**：原生要重新解决 buffer 生命周期、取消、异步写——但实测证明这代已经解决了（第一版的教训已吸收，见 §4.2）。

---

## 第四部分：后端架构分析

### AsioUringIoBackend（保留，对比参照）

### 4.1 整体架构：主线程直驱

**核心设计**：io_context 只在 Worker 主线程（libev 事件循环线程）上被 poll()，不创建任何额外线程，所有 I/O 操作在同一线程完成。

```
┌──────────────────────────────────────────────────────────────────┐
│                    Worker 进程（单线程）                           │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │                    libev Event Loop                        │  │
│  │                                                            │  │
│  │  ① ev_prepare ──► io_context.poll()                       │  │
│  │      在 epoll_wait 前：flush SQE 到内核                    │  │
│  │                                                            │  │
│  │  ② epoll_wait（libev 唯一阻塞点）                          │  │
│  │      监听: ring_fd（io_uring CQ 通知）+ 其它业务 fd         │  │
│  │                                                            │  │
│  │  ③ ev_io(ring_fd) ──► io_context.poll()                   │  │
│  │      CQ 有完成事件：收割 CQE，调用 completion handler      │  │
│  │                                                            │  │
│  │  ④ ev_check ──► io_context.poll()                         │  │
│  │      epoll_wait 窗口期漏到的 CQE 兜底收割                  │  │
│  │      UpdateRingWatcher()：无挂起 op 时 stop ring_fd 监听   │  │
│  └────────────────────────────────────────────────────────────┘  │
│                                                                  │
│  线程数：1  /  锁：0  /  额外线程：0                              │
└──────────────────────────────────────────────────────────────────┘
```

三路驱动各司其职，缺一不可：

| 驱动 | 触发时机 | 职责 | 为什么不可缺 |
|------|---------|------|------------|
| `ev_prepare` | epoll_wait **前** | io_context.poll() → flush SQE | SQE 在 Asio 内部队列中，不主动 flush 则永不提交到内核 |
| `ev_io(ring_fd)` | CQ **非空**时 | io_context.poll() → 收割 CQE | io_uring 完成通知机制：CQ 有条目时 ring_fd 可读。没有这一路，completion handler 延迟不可控 |
| `ev_check` | epoll_wait **后** | io_context.poll() → 兜底收割 | ev_prepare 到 epoll_wait 返回的窗口期新到的 CQE，ring_fd 已触发过，需要 ev_check 清空 |

### 4.2 初始化：三路驱动注册

```cpp
bool AsioUringIoBackend::Init(struct ev_loop* loop) {
    m_loop = loop;

    // 1. 创建 io_context，hint=1 表示单线程
    m_ioContext = std::make_unique<asio::io_context>(1);

    // 2. 找到 Asio 内部创建的 io_uring ring_fd
    m_ringFd = FindIoUringRingFd(*m_ioContext);

    // 3. 注册 ev_prepare：每轮 epoll_wait 前 flush SQE
    ev_prepare_init(&m_prepareWatcher, OnPrepare);
    m_prepareWatcher.data = this;
    ev_prepare_start(loop, &m_prepareWatcher);

    // 4. 注册 ev_check：epoll_wait 后兜底收割
    ev_check_init(&m_checkWatcher, OnCheck);
    m_checkWatcher.data = this;
    ev_check_start(loop, &m_checkWatcher);

    // 5. 注册 ev_io(ring_fd)：按需启动，初始不监听
    ev_io_init(&m_ringWatcher, OnRingReady, m_ringFd, EV_READ);
    m_ringWatcher.data = this;
    // 注意：不在 Init 时 ev_io_start，由 UpdateRingWatcher 按需控制

    return true;
}
```

### 4.3 FindIoUringRingFd：找到 ring_fd

Asio 的 io_uring 后端创建了 ring_fd 但没有直接暴露。通过 `/proc/self/fd` 遍历 `anon_inode:[io_uring]` 类型的 fd 找到它：

```cpp
int AsioUringIoBackend::FindIoUringRingFd(asio::io_context& ctx) {
    DIR* dir = opendir("/proc/self/fd");
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        char link[256], target[512];
        snprintf(link, sizeof(link), "/proc/self/fd/%s", entry->d_name);
        ssize_t len = readlink(link, target, sizeof(target) - 1);
        if (len > 0) {
            target[len] = '\0';
            if (strncmp(target, "anon_inode:[io_uring]", 21) == 0) {
                closedir(dir);
                return std::stoi(entry->d_name);
            }
        }
    }
    closedir(dir);
    return -1;
}
```

这是当前 Asio API 的局限，ring_fd 未通过公开接口暴露。

### 4.4 FdState：每个连接的状态

每个 socket fd 对应一个 FdState，持有 Asio 的流对象：

```cpp
struct FdState : std::enable_shared_from_this<FdState> {
    asio::posix::stream_descriptor stream;   // Asio 封装 fd
    bool readPending  = false;
    bool writePending = false;

    FdState(asio::io_context& ctx, int fd) : stream(ctx, fd) {}
};

// 存储结构
std::unordered_map<int, std::shared_ptr<FdState>> m_fds;
```

**weak_ptr 防悬挂**：completion handler 捕获 `weak_ptr<FdState>`，回调时先 lock() 确认对象仍然存活：

```cpp
auto weak = std::weak_ptr<FdState>(state);
state->stream.async_read_some(buf, [this, weak, fd, ...](auto& ec, size_t n) {
    auto s = weak.lock();
    if (!s) return;   // fd 已被 CancelFd 移除，忽略回调
    s->readPending = false;
    if (!ec) m_callback(fd, seq, IoOp::Read, n, userData);
});
```

不需要全局锁：单线程下 `weak.lock()` 只是原子操作，`m_fds` 的读写都在主线程完成。

### 4.5 SubmitRead / SubmitWrite

```cpp
void AsioUringIoBackend::SubmitRead(int fd, CBuffer* buf, uint32_t seq, void* ud) {
    auto state = GetOrCreateFdState(fd);  // 懒创建 FdState
    state->readPending = true;

    auto weak = std::weak_ptr<FdState>(state);
    state->stream.async_read_some(
        asio::buffer(buf->GetRawWriteBuffer(), buf->GetRawWriteBufferLength()),
        [this, weak, fd, seq, buf, ud](const std::error_code& ec, size_t n) {
            auto s = weak.lock();
            if (!s) return;
            s->readPending = false;
            buf->AdvanceWriteIndex(static_cast<int>(n));
            m_callback(fd, seq, IoOp::Read, ec ? -ec.value() : n, ud);
        }
    );

    UpdateRingWatcher();  // 有挂起 op，确保 ring_fd 在监听
}
```

`SubmitWrite` 结构相同，内部走 `async_write_some`。SQE 不在此时立即提交内核，而是放入 Asio 内部队列，等下一个 `ev_prepare` 的 `io_context.poll()` 统一 flush。

**批量 flush 的意义**：

```
一次业务请求可能触发：
  SubmitRead(fd1) → SQE#1 入队
  SubmitRead(fd2) → SQE#2 入队
  SubmitWrite(fd3) → SQE#3 入队

ev_prepare: io_context.poll() 一次性 flush 三个 SQE
  → 一次 io_uring_enter() 系统调用提交三个请求
  （对比 epoll：每个 fd 分别 epoll_ctl + read/write，三倍 syscall）
```

### 4.6 ring_fd 按需启停

Asio 的 `io_context::poll()` 内部每次调度都会通过 `interrupt()` 提交一个 NOP SQE。NOP 几乎立即完成，使 CQ 非空，ring_fd 持续可读。

如果始终监听 ring_fd，即使没有业务 I/O，NOP 也会不断唤醒 epoll_wait，造成空转。

**解法**：只有当 `m_fds` 中存在挂起操作时才监听 ring_fd：

```
SubmitRead/Write 调用
    │
    ▼
UpdateRingWatcher()
    │  m_fds 中有 readPending 或 writePending？
    ├── YES → ev_io_start(ring_fd)   （真实 IO 完成时被唤醒）
    └── NO  → ev_io_stop(ring_fd)    （NOP CQE 不进入 epoll 热路径）

ev_check 末尾也调用 UpdateRingWatcher()
    │  所有 op 完成后 stop ring_fd 监听
    │  下一轮 epoll_wait 可真正阻塞到外部事件
```

代价：completion 后到 stop 之间最多延迟一轮 ev_run 迭代（亚毫秒），可接受。

### 4.7 CancelFd：连接关闭

```cpp
void AsioUringIoBackend::CancelFd(int fd) {
    auto it = m_fds.find(fd);
    if (it == m_fds.end()) return;

    auto& state = it->second;
    // stream.close() 触发所有挂起的 async 操作以 operation_aborted 完成
    // weak_ptr.lock() 在 handler 中返回 nullptr，回调被静默忽略
    std::error_code ec;
    state->stream.close(ec);

    m_fds.erase(it);
    UpdateRingWatcher();
}
```

### 4.8 完整 I/O 流程

#### 读操作时序

```
网络对端      内核            Asio/io_uring       Thunder 主线程
   │            │                  │                    │
   │── TCP ────►│                  │                    │
   │            │                  │  SubmitRead()      │
   │            │◄─────────────────│  async_read_some() │
   │            │  SQE→SQ(内部队列) │                    │
   │            │                  │                    │
   │            │                  │── ev_prepare ──────│
   │            │  io_uring_enter()│  io_context.poll() │
   │            │◄─────────────────│  flush SQE         │
   │            │                  │                    │
   │            │  recv(fd,buf)    │                    │
   │            │  写 CQE(res=n)   │                    │
   │            │  ring_fd 可读    │                    │
   │            │─────────────────►│                    │
   │            │                  │── epoll_wait 返回  │
   │            │                  │── ev_io(ring_fd)   │
   │            │                  │   io_context.poll()│
   │            │                  │   收割 CQE         │
   │            │                  │   ReadComplete()   │
   │            │                  │   m_callback() ───►│
   │            │                  │                    │  RecvDataAndDispose()
   │            │                  │                    │  codec->Decode()
   │            │                  │                    │  → 业务逻辑
   │            │                  │── ev_check ────────│
   │            │                  │   UpdateRingWatcher│
```

#### 写操作时序

```
Thunder 主线程    Asio/io_uring       内核          网络对端
    │                  │               │               │
    │  SendTo()        │               │               │
    │  codec->Encode() │               │               │
    │  SubmitWrite() ──►               │               │
    │                  │ async_write_some()            │
    │                  │ SQE→SQ(内部队列)              │
    │                  │               │               │
    │── ev_prepare ────►               │               │
    │                  │ io_context.poll()             │
    │                  │ io_uring_enter() ────────────►│
    │                  │               │  send(fd,buf) │
    │                  │               │  写 CQE       │
    │                  │               │  数据发出 ────►│
    │                  │ ev_io(ring_fd)│               │
    │                  │ 收割 CQE      │               │
    │                  │ WriteComplete │               │
    │◄─────── m_callback(Write, n) ────│               │
    │  (通知上层可以继续写)             │               │
```

### 4.9 SQE/CQE 生命周期

```
① async_read_some() 调用
   Asio 构造 SQE：op=IORING_OP_RECV, fd, buf_addr, len
   放入 Asio 内部 pending_submit 队列

② ev_prepare → io_context.poll()
   Asio 调度 submit_sqes_op
   io_uring_enter(ring_fd, to_submit=N, min_complete=0)  ← 一次 syscall
   SQE 交给内核

③ 内核执行 recv
   数据写入用户 buf
   生成 CQE: user_data=<Asio内部seq>, res=bytes_read
   ring_fd 变为可读

④ epoll_wait 返回，ev_io(ring_fd) 触发
   io_context.poll() → io_uring_for_each_cqe
   匹配 Asio 内部 seq → 调用 completion handler
   io_uring_cqe_seen() 确认消费

⑤ completion handler
   weak.lock() 检查 FdState 存活
   buf->AdvanceWriteIndex(n)
   m_callback(fd, seq, Read, n)  → Worker 处理业务
```

---

### NativeUringIoBackend（推荐 io_uring 后端）

#### 架构：单线程，eventfd + ev_io 收割

**NativeUringIoBackend** 绕开 Asio 中间层，自管 SQ/CQ，通过 libev 单线程驱动收割。与 AsioUringIoBackend 的关键差异：

```
┌──────────────────────────────────────────────────────────────────┐
│                    Worker 进程（单线程）                           │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │                    libev Event Loop                        │  │
│  │                                                            │  │
│  │  ① SubmitRead/SubmitWrite ──► io_uring_prep_recv/send     │  │
│  │     直接填 SQE → io_uring_submit（不经过 Asio）            │  │
│  │                                                            │  │
│  │  ② epoll_wait（libev 唯一阻塞点）                          │  │
│  │     监听: eventfd（io_uring 完成通知）+ 其它业务 fd         │  │
│  │                                                            │  │
│  │  ③ ev_io(eventfd) ──► ReapCqes()                          │  │
│  │     CQ 有完成事件：收割 CQE，直接调用 m_callback            │  │
│  │                                                            │  │
│  │  ④ ev_check ──► ReapCqes()                                │  │
│  │     epoll_wait 窗口期漏到的 CQE 兜底收割                   │  │
│  └────────────────────────────────────────────────────────────┘  │
│                                                                  │
│  驱动：eventfd + ev_io + ev_check（两路，无需 ev_prepare）        │
│  线程数：1  /  锁：0  /  额外线程：0                              │
│  SQPOLL：env 门控（THUNDER_URING_SQPOLL=1，免特权）              │
└──────────────────────────────────────────────────────────────────┘
```

与 AsioUringIoBackend 的关键差异：

| 维度 | AsioUringIoBackend | NativeUringIoBackend |
|------|-------------------|---------------------|
| 收割驱动 | ev_prepare + ev_check + ev_io(ring_fd) 三路 | eventfd + ev_io + ev_check 两路（无 prepare） |
| 完成通知 | /proc hack 找 ring_fd | io_uring_register_eventfd 标准 API |
| 中间层 | Asio io_context.poll() → scheduler → reactor | 直接 io_uring_peek_cqe → PendingOp → m_callback |
| NOP SQE 空转 | 有（interrupt NOP 唤醒 ring_fd）| 无（eventfd 仅真实 CQE 触发） |
| SQPOLL | ❌ 硬编码 flags=0 | ✅ env 门控 |
| send_zc | ❌ 无支持 | ✅ 按阈值分流 + WriteNotif |

#### 初始化：ring + eventfd 注册

```cpp
bool NativeUringIoBackend::Init(struct ev_loop* loop,
                                 IoCompletionCallback callback, void* user_data)
{
    // 1. 创建 io_uring 实例（可选 SQPOLL）
    if (THUNDER_URING_SQPOLL=1) {
        io_uring_queue_init_params(sqDepth, &ring, &params);  // IORING_SETUP_SQPOLL
    } else {
        io_uring_queue_init(sqDepth, &ring, 0);               // 默认中断驱动
    }

    // 2. 创建 eventfd + 注册到 io_uring（标准 API，无需 /proc hack）
    m_evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    io_uring_register_eventfd(&m_ring, m_evfd);
    // → 内核完成 CQE 时自动 ++eventfd counter
    // → libev ev_io(eventfd) 被唤醒 → ReapCqes()

    // 3. 注册 ev_io(eventfd) + ev_check 兜底
    ev_io_init(&m_evWatcher, &OnEvfd, m_evfd, EV_READ);
    ev_io_start(loop, &m_evWatcher);
    ev_check_init(&m_check, &OnCheck);
    ev_check_start(loop, &m_check);
}
```

#### PendingOp：后端持有，连接安全的陈旧 CQE

```cpp
// 本后端 heap 持有，生命周期 = 直到其 CQE 被收割。
// 绝不持有连接资源所有权。
struct PendingOp {
    int            fd;
    uint32_t       seq;
    IoOp           op;
    util::CBuffer* buf;        // 仅当 fd/seq 仍有效且未取消时才解引用
    // send_zc 字段
    bool   isZc      = false;  // 是否为 send_zc 操作
    char*  zcBuf     = nullptr; // bounce 缓冲，NOTIF 时 free
    int    zcBytes   = 0;       // 结果 CQE 的字节数
    bool   gotResult = false;   // 是否已收到结果 CQE
};
```

**安全模型**：CancelFd 从 `m_fds` 擦除 fd 状态，但不释放 PendingOp。陈旧 CQE 到达时，因 fd 不在表（或 seq 不符）被识别为陈旧，仅 `delete po`，绝不触碰已被 DestroyConnect 释放的连接缓冲。

#### SubmitRead / SubmitWrite（直发 SQE）

```cpp
bool NativeUringIoBackend::SubmitRead(int fd, CBuffer* buf, uint32_t seq)
{
    auto& st = m_fds[fd];
    if (st.readPending > 0) return true;  // 已有读在途，防重入

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    PendingOp* po = new PendingOp{fd, seq, IoOp::Read, buf};
    io_uring_prep_recv(sqe, fd, buf->GetRawWriteBuffer(), buf->WriteableBytes(), 0);
    io_uring_sqe_set_data(sqe, po);
    st.readPending++;
    io_uring_submit(&m_ring);
    return true;
}

bool NativeUringIoBackend::SubmitWrite(int fd, CBuffer* buf, uint32_t seq)
{
    int readable = buf->ReadableBytes();
    if (readable <= 0) {
        m_callback(fd, seq, IoOp::Write, 0, m_userData);  // 空写回调
        return true;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    PendingOp* po = new PendingOp{fd, seq, IoOp::Write, buf};

    if (m_zcEnabled && readable >= m_zcThreshold)
    {
        // send_zc：拷到 bounce 缓冲再零拷贝发送，解耦连接 buffer 生命周期
        po->isZc  = true;
        po->zcBuf = malloc(readable);
        memcpy(po->zcBuf, buf->GetRawReadBuffer(), readable);
        io_uring_prep_send_zc(sqe, fd, po->zcBuf, readable, 0, 0);
    }
    else
    {
        io_uring_prep_send(sqe, fd, buf->GetRawReadBuffer(), readable, 0);
    }

    io_uring_sqe_set_data(sqe, po);
    st.writePending++;
    io_uring_submit(&m_ring);
    return true;
}
```

#### send_zc 双 CQE 收割

send_zc 产生两个 CQE：结果 CQE（`res`=字节数）+ 通知 CQE（`IORING_CQE_F_NOTIF`，buffer 可复用）。收割逻辑按 flags 分流：

```cpp
void NativeUringIoBackend::ReapCqes()
{
    struct io_uring_cqe* cqe;
    while (io_uring_peek_cqe(&m_ring, &cqe) == 0)
    {
        PendingOp* po = static_cast<PendingOp*>(io_uring_cqe_get_data(cqe));
        int      res   = cqe->res;
        unsigned flags = cqe->flags;   // 必须在 cqe_seen 前取
        io_uring_cqe_seen(&m_ring, cqe);

        auto it = m_fds.find(po->fd);
        bool valid = (it != m_fds.end() && it->second.seq == po->seq
                      && !it->second.cancelled);

        if (po->isZc)
        {
            if (flags & IORING_CQE_F_NOTIF)
            {
                // 通知 CQE：内核已脱离 bounce，可安全释放
                if (valid) {
                    if (bytes > 0) po->buf->AdvanceReadIndex(bytes);
                    m_callback(po->fd, po->seq, IoOp::WriteNotif, bytes, m_userData);
                }
                free(po->zcBuf);
                delete po;          // 终态
            }
            else
            {
                // 结果 CQE：记录字节数。F_MORE → 等通知 CQE
                po->zcBytes = res; po->gotResult = true;
                if (!(flags & IORING_CQE_F_MORE)) {
                    // 无通知后续（立即失败）：此刻即终态
                    m_callback(po->fd, po->seq, IoOp::WriteNotif, res, m_userData);
                    free(po->zcBuf); delete po;
                }
                // else: F_MORE → 等通知 CQE，保留 po/zcBuf
            }
            continue;
        }

        // 普通 Read/Write（单 CQE）
        if (valid) {
            if (po->op == IoOp::Read) {
                if (res > 0) po->buf->AdvanceWriteIndex(res);
                m_callback(po->fd, po->seq, IoOp::Read, res, m_userData);
            } else {
                if (res > 0) po->buf->AdvanceReadIndex(res);
                m_callback(po->fd, po->seq, IoOp::Write, res, m_userData);
            }
        }
        delete po;
    }
}
```

#### send_zc：bounce 缓冲设计理由

当前 send_zc 使用 **bounce 缓冲（后端自有 malloc）** 而非直发 `pSendBuff`：

```
直发 pSendBuff（真零拷贝，#10，未实施）:
  风险：DestroyConnect 同步释放 pSendBuff，但内核 NOTIF 可能未到
        → 内核 DMA 仍在读已释放的内存 → use-after-free
  代价：需重构连接生命周期，fd 复用 + TCP lingering 的 UAF 雷区

bounce 缓冲（当前）:
  send_zc 前 malloc → memcpy → prep_send_zc(bounce)
  NOTIF 后 free(bounce)  ← pSendBuff 生命周期不受影响
  收益：拷贝从"用户→内核"挪到"用户→bounce" → 净收益≈0（实测确证）
  但代码安全：无 UAF，DestroyConnect 可随时同步释放连接缓冲
```

实测：bounce 版 native_uring+zc (5854 RPS) vs native_uring 无 zc (5903 RPS) ≈ 持平（噪声内）——确证 bounce 拷贝挪位 = 净收益≈0。真零拷贝（去 bounce，#10）留作后续评估项。

#### SQPOLL（env 门控）

```bash
THUNDER_URING_SQPOLL=1        # 启用内核线程轮询 SQ
THUNDER_URING_SQPOLL_IDLE=100 # 空闲 ms 后休眠（防吃 cgroup CPU）
```

内核 7.0 中 SQPOLL **无需 CAP_SYS_NICE**，普通用户即可使用。`sq_thread_idle` 设小值防内核线程自旋吃 cgroup CPU 配额。

---

## 第五部分：性能 Benchmark

### 5.1 测试环境与方法

- **环境**：Linux 7.0.0 宿主机，Thunder 服务运行于 Docker 容器（host 网络）
- **工具**：wrk 4.1.0（4 线程，epoll，C 实现）
- **目标**：`POST http://127.0.0.1:27006/hello/hello`（Echo 模式）
- **参数**：每组 12s，4 线程，Lua 脚本注入 JSON body
- **包大小**：64KB（`{"option":"Echo","size":65536}`，服务端返回等大小的 `data` 字段）
- **并发**：c50（隔离基准）/ c100 / c500
- **基准时间**：2026-05-16

### 5.2 64KB 写 — 原生后端 vs ev（主要结论）

> 64KB 是 Thunder 的"墙"场景——5,000 RPS × 64KB × 2（读+写）≈ 640MB/s 内存拷贝。此前的 asio_uring 在这堵墙前与 ev 持平；原生后端是第一个打破它的。

| 后端 | RPS | wrk 延迟（TTFB）| 真实整包延迟（Little's Law）| Transfer |
|------|-----|----------------|--------------------------|----------|
| ev 基线 | 5,485 | 8.86ms | **9.12ms**（50÷5485）| 344 MB/s |
| **native_uring（无 zc）** | **5,903（+7.6%）** | 4.06ms（TTFB）| **8.47ms**（50÷5903，**−7.1%**）| 370 MB/s |
| native_uring + send_zc(bounce) | 5,854（+6.7%） | 4.26ms（TTFB）| 8.54ms（−6.4%）| 367 MB/s |

```
RPS（越高越好）：
ev               │████████████████████████████████████████│ 5,485
native_uring     │██████████████████████████████████████████│ 5,903 (+7.6%)
native_uring+zc  │█████████████████████████████████████████│ 5,854 (+6.7%)

平均延迟（越低越好）：
ev               │████████████████████████████████████████████████│ 8.86ms
native_uring     │██████████████████│ 4.06ms (−54%)
native_uring+zc  │███████████████████│ 4.26ms
```

### 5.3 核心发现

**1. 原生后端本身就是真实收益**

+7.6% RPS / −54% 延迟 / 尾延迟从 ev 的 155ms 降到 11ms——与 asio_uring「对 ev≈持平」截然不同。

收益主因是**精简控制路径**：
- 无 Asio 中间层（`io_context.poll()` → `scheduler::poll()` → `reactor` → `event_fd_read_op` → `io_uring_service::run()`）
- 无 NOP SQE / interrupt 机制（Asio 每轮 poll 插入一个 NOP 用于唤醒，其副作用在 64KB 大包时显现）
- 无 `/proc/self/fd` hack 找 ring_fd——直接用 `io_uring_register_eventfd` 标准 API
- CQE 收割路径最短：`io_uring_peek_cqe → PendingOp → m_callback(fd, seq, op, res, user_data)`

**2. send_zc bounce 版 = 净收益≈0（实测确证）**

native_uring+zc (5,854) vs native_uring 无 zc (5,903) ≈ 噪声内。bounce 拷贝（用户 buffer → 后端 malloc → send_zc）只是把 memcpy 从"用户→内核"挪到"用户→bounce"，未减少拷贝次数。

**3. 与历史 asio_uring 数据对照**

| 后端 | 64KB c100 RPS | 64KB 延迟 | 结论 |
|------|--------------|----------|------|
| ev | 5,653 | 17.90ms | 基线 |
| asio_uring | 5,473（-3%） | 9.67ms（-46%，TTFB 非完整） | 对 ev ≈ 持平 |
| **native_uring** | **5,903（+7.6%）** | **4.06ms（−54%，完整）** | 真实突破 |

ev 在不同并发下绝对数值略有差异（c50 vs c100），但相对趋势稳定。

### 5.4 io_uring 的吞吐边界（回顾）

io_uring 共享内存 ring buffer（SQ/CQ）消除的是 **SQE/CQE 控制元数据**的传递开销，业务数据的 memcpy 照旧发生：

```
读：NIC → [DMA] → 内核 socket buffer → [memcpy] → 用户 buffer
写：用户 buffer → [memcpy] → 内核 socket buffer → [DMA] → NIC
```

64KB 场景下，memcpy 仍是主要瓶颈。原生后端通过精简控制路径拿到了 +7.6%，进一步的数据面突破需 send_zc 真零拷贝（去 bounce，#10，当前 measurement-gated 暂不做）。

### 5.5 综合建议

- 超包（64KB+）场景：**native_uring** 有确证收益（+7.6% RPS / −54% 延迟），推荐使用
- 小包/中包：ev 与 asio_uring 均可（控制路径瓶颈不同，小包已不是当前痛点）
- send_zc 可开可不开：当前 bounce 版净收益≈0，留作 B-3b（真零拷贝）的前置接口

---

## 第六部分：当前状态与展望

### 6.1 已在用的能力

| 能力 | 状态 | 后端 | 备注 |
|------|------|------|------|
| io_uring 中断驱动（默认）| ✅ | native_uring | eventfd + ev_io 收割 |
| SQPOLL | ✅ env 门控 | native_uring | `THUNDER_URING_SQPOLL=1`，内核 7.0 免特权 |
| send_zc (bounce) | ✅ env 门控 | native_uring | `THUNDER_URING_ZC=1`，>=16KB 阈值分流，净收益≈0 |
| IoOp::WriteNotif | ✅ | 通用 | send_zc NOTIF CQE 的 buffer 回收通道 |
| ev 回退 | ✅ | 所有 | 任何后端 Init 失败自动降级 ev |

### 6.2 暂不做（measurement-gated）

| 项 | 原因 |
|----|------|
| send_zc 真零拷贝（去 bounce，#10）| 实测 bounce-vs-nozc ≈ 持平；去 bounce 的上行不确定（+个位数%？），风险高（fd 复用 × TCP linger UAF，v1 沉没区）|
| Provided Buffers (zcrx) | 需硬件网卡支持 header/data split；本机 e1000e/mt7921e 不支持；容器 veth 虚拟接口不经物理网卡 |
| Fixed Buffers | 原生后端已拿到主要收益（+7.6%），此项独立于当前瓶颈 |

### 6.3 仍待部署

| Gate | 说明 | 状态 |
|------|------|------|
| G1 K8s seccomp profile | 提供自定义 seccomp profile 放行 io_uring 三 syscall，替代 unconfined | ❌ 待做 |
| G2 降级可观测 | 启动横幅 + 指标 + 健康检查（替代当前静默降级）| ❌ 待做 |

---

## 附录

### A. 配置方式

```json
{
  "io_backend": "native_uring"
}
```

可选值：`"ev"`（默认基线）、`"asio_uring"`（保留）、`"native_uring"`（推荐 io_uring）。

### B. 编译要求

```bash
# CMakeLists.txt 已默认开启
option(THUNDER_IO_ASIO_URING "启用 standalone Asio io_uring I/O 后端" ON)
```

运行环境需 Linux 内核 ≥ 5.1，建议 ≥ 5.10（io_uring 稳定性大幅提升）。原生后端需 `liburing`（≥2.0）。

### C. 关键源码文件

| 文件 | 职责 |
|------|------|
| `code/Net/include/labor/IoBackend.hpp` | 抽象接口定义、IoOp 枚举（含 WriteNotif）|
| `code/Net/src/labor/EvIoBackend.cpp` | epoll 基线实现 |
| `code/Net/src/labor/AsioUringIoBackend.{hpp,cpp}` | Asio + io_uring 后端（保留）|
| `code/Net/src/labor/NativeUringIoBackend.{hpp,cpp}` | 原生 liburing 后端（推荐）|
| `code/Net/src/labor/Labor.cpp` | IoBackend 工厂（按配置创建实例）|
| `code/Net/src/labor/Worker.cpp` | OnIoComplete 三路分发（Read/Write/WriteNotif）|
| `code/Net/src/labor/Manager.cpp` | 同上，Manager 侧 |

---

*文档版本：v3.1*
*最后更新：2026-05-16*（重写：三后端架构、Asio→原生选型演进、NativeUringIoBackend 架构、原生后端压测数据；移除已废弃的第一代 UringIoBackend 描述）
*项目仓库：https://github.com/chenjiayi0603/thunder*
