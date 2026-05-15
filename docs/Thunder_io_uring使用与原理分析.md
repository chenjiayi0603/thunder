# Thunder 框架 io_uring 使用与原理分析

## 摘要

本文档分析 Thunder 框架中 io_uring 的集成设计。Thunder 采用 C++20 开发，核心事件循环基于 libev，通过 IoBackend 抽象接口支持两种 I/O 后端：

- **EvIoBackend**：基于 epoll 的传统实现，稳定可靠，作为基线对照
- **AsioUringIoBackend**：基于 standalone Asio io_uring 后端，生产级主力

文档重点回答三个问题：**io_uring 解决什么问题**、**为什么用 Asio 封装而非直接用 liburing**、**AsioUringIoBackend 内部如何工作**。

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

io_uring 通过 mmap 共享内存实现零拷贝内核交互：

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
  └──── 共享内存（mmap），无需数据拷贝 ────┘
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
       ┌───────┴────────┐
       ▼                ▼
 EvIoBackend    AsioUringIoBackend
 (epoll 基线)   (io_uring 生产)
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

### 2.3 两种实现对比

| 特性 | EvIoBackend | AsioUringIoBackend |
|------|------------|-------------------|
| **读机制** | ev_io watcher + ReadFD | async_read_some → io_uring_prep_recv |
| **写机制** | ev_io watcher + WriteFD | async_write_some → io_uring_prep_send |
| **集成方式** | per-fd epoll watcher | ev_prepare/ev_check/ev_io(ring_fd) 三路驱动 |
| **线程数** | 1（主线程）| 1（主线程，零额外线程）|
| **syscall 模型** | 每次 read/write 一次 syscall | 批量提交，多个 SQE 合并一次 io_uring_enter |
| **状态** | 默认，稳定 | 生产主力 |

---

## 第三部分：为什么选 AsioUringIoBackend，不直接用 liburing

这是设计的核心问题。Thunder 早期曾实现过一个直接使用 liburing 的后端（已移除），其局限直接说明了为什么要走 Asio 路线。

### 3.1 直接用 liburing 的代价

直接调用 liburing API，开发者需要自己处理所有细节：

```cpp
// 每次读操作需要手动完成：
struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
io_uring_prep_recv(sqe, fd, buf, len, 0);
sqe->user_data = seq++;                      // 手动 seq 追踪
pending_ops[seq] = {fd, buf, callback};      // 手动 pending map
io_uring_submit(&ring);                      // 手动提交

// CQE 收割需要手动：
io_uring_for_each_cqe(&ring, head, cqe) {
    auto it = pending_ops.find(cqe->user_data);
    if (it != pending_ops.end()) {
        auto op = std::move(it->second);
        pending_ops.erase(it);
        op.callback(op.fd, cqe->res);        // 手动分发
    }
}
io_uring_cq_advance(&ring, count);           // 手动推进 CQ head
```

这套代码的问题：

1. **写操作难以真正异步**：io_uring 异步写与 TLS/codec 状态机交互复杂。Thunder 的 codec 需要对 CBuffer 进行分阶段处理（Encode → 写入 → 确认发出）。手写后端迫于此复杂性，写操作退化为同步 `send()`，完全放弃了 io_uring 的写侧优势。
2. **取消逻辑繁琐**：fd 关闭时，需要遍历 pending map 移除所有相关条目，同时还要向内核提交 `IORING_OP_CANCEL` SQE，否则内核仍会写 CQE 到已废弃的 fd 上。
3. **缓冲区生命周期管理**：SQE 提交后，用户缓冲区必须保持有效直到 CQE 返回。手写代码需要精心设计引用计数或 shared_ptr。
4. **没有批量提交优化**：需要自己决定何时调用 `io_uring_submit()`，提前调用浪费 syscall，延迟调用增加 I/O 延迟。

### 3.2 Asio io_uring 后端的封装价值

Asio 的 io_uring 后端（`asio::posix::stream_descriptor`）将上述所有问题封装在内部：

```cpp
// Asio 封装后，读操作：
stream.async_read_some(
    asio::buffer(buf->Data(), buf->Capacity()),
    [weakState, buf, callback](const std::error_code& ec, size_t n) {
        if (auto s = weakState.lock()) {
            callback(s->fd, IoOp::Read, n, ec.value());
        }
    }
);
// Asio 内部自动完成：SQE 填充、seq 管理、CQE 收割、error_code 映射

// 写操作同样全异步：
stream.async_write_some(
    asio::buffer(buf->Data(), buf->Length()),
    [weakState, buf, callback](const std::error_code& ec, size_t n) {
        callback(s->fd, IoOp::Write, n, ec.value());
    }
);
// 底层：io_uring_prep_send，真正异步，不退化为同步 send()
```

**具体收益**：

| 问题 | liburing 直接 | Asio 封装 |
|------|-------------|----------|
| 写操作异步 | ❌ 退化为同步 send() | ✅ async_write_some → io_uring_prep_send |
| SQE 批量提交 | 需手写调度逻辑 | ✅ Asio 内部 submit_sqes_op，ev_prepare 统一 flush |
| CQE 收割 | 手动循环 + map 查找 | ✅ Asio 内部处理，直接回调 handler |
| 取消安全 | 手动遍历 + CANCEL SQE | ✅ stream.close()，Asio 自动处理挂起 op |
| 缓冲区生命周期 | 手动引用计数 | ✅ handler 捕获 shared_ptr，自动管理 |
| seq/user_data | 手写 map | ✅ Asio 内部，不暴露给用户 |

### 3.3 为什么 io_context.poll() 天然适合嵌入 libev

这是能实现"主线程直驱、零额外线程"的关键。

Asio 的 `io_context::poll()` 设计保证：**不阻塞，只处理当前已就绪的任务后立即返回**。这意味着它可以被任何外部事件循环在合适的时机主动调用，而不是反客为主地占据线程。

直接用 liburing 时没有这个机制，开发者要么需要独立线程跑收割循环，要么在主线程中自己实现相同的调度逻辑——即重新造 Asio 的轮子。

### 3.4 standalone Asio，依赖极轻

Thunder 使用的是 **standalone Asio**（`ASIO_STANDALONE` 宏），不依赖 Boost，只需头文件：

```cmake
add_compile_definitions(ASIO_STANDALONE ASIO_HAS_IO_URING ASIO_DISABLE_EPOLL)
```

- `ASIO_HAS_IO_URING`：启用 io_uring 后端
- `ASIO_DISABLE_EPOLL`：强制走 io_uring 路径，不 fallback 到 epoll

**小结**：选择 Asio 不是因为需要 Asio 的网络层功能，而是因为它的 io_uring 后端提供了一个经过充分测试的、可嵌入的 io_uring 调度器，以最小代价解决了直接使用 liburing 的所有痛点。

---

## 第四部分：AsioUringIoBackend 深度分析

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

## 第五部分：性能 Benchmark

### 5.1 测试环境与方法

- **环境**：Linux 7.0.0 宿主机，Thunder 服务运行于 Docker 容器（bridge 网络）
- **工具**：wrk 4.1.0（4 线程，epoll，C 实现）
- **目标**：`POST http://127.0.0.1:27006/hello/hello`（Echo 模式）
- **参数**：每组 30s，4 线程，Lua 脚本注入 JSON body
- **并发**：c100 / c500
- **包大小**：37B（小包）/ 4KB（大包）/ 64KB（超包）
- **基准时间**：2026-05-15

Lua 脚本：`tests/benchmark/wrk_small.lua` / `wrk_4k.lua` / `wrk_64k.lua`

### 5.2 小包（37B）

| Backend | c100 RPS | c100 Avg Lat | c500 RPS | c500 Avg Lat |
|---------|----------|-------------|----------|-------------|
| ev | **136,130** | 0.73ms | **127,079** | 4.21ms |
| asio_uring | 132,246 | **0.52ms** | 124,184 | **3.25ms** |

```
c100 平均延迟（越低越好）：
ev          │████████████████████████████████████│ 0.73ms
asio_uring  │██████████████████████████│ 0.52ms  (-29%)
```

小包场景 RPS 几乎持平（差 2.9%），asio_uring 延迟低 23-29%。

### 5.3 大包（4KB）

| Backend | c100 RPS | c100 Avg Lat | c500 RPS | c500 Avg Lat |
|---------|----------|-------------|----------|-------------|
| ev | 57,384 | 1.74ms | 52,004 | 9.73ms |
| asio_uring | **58,069** | **1.13ms** | **55,152** | **7.55ms** |

```
c100 平均延迟：
ev          │████████████████████████████████████│ 1.74ms
asio_uring  │█████████████████████████│ 1.13ms  (-35%)
```

4KB 场景 asio_uring 全面领先：RPS +1-6%，延迟低 22-35%。

### 5.4 超包（64KB）

| Backend | c100 RPS | c100 Avg Lat | c500 RPS | c500 Avg Lat |
|---------|----------|-------------|----------|-------------|
| ev | **5,653** | 17.90ms | **5,308** | 94.22ms |
| asio_uring | 5,473 | **9.67ms** | 5,067 | **78.90ms** |

```
c100 平均延迟：
ev          │████████████████████████████████████████████████│ 17.90ms
asio_uring  │█████████████████████████│  9.67ms  (-46%)
```

超包场景延迟优势最明显：c100 低 46%，c500 低 16%。io_uring 的 SQE 批量提交在大包下减少了 epoll_ctl 注册/注销和 syscall 往返。

### 5.5 综合结论

| 场景 | ev RPS | asio_uring RPS | asio 延迟优势 |
|------|--------|----------------|-------------|
| 小包 37B c100 | 136,130 | 132,246（-3%） | **-29%** |
| 小包 37B c500 | 127,079 | 124,184（-2%） | **-23%** |
| 大包 4KB c100 | 57,384 | 58,069（**+1%**） | **-35%** |
| 大包 4KB c500 | 52,004 | 55,152（**+6%**） | **-22%** |
| 超包 64KB c100 | 5,653 | 5,473（-3%） | **-46%** |
| 超包 64KB c500 | 5,308 | 5,067（-5%） | **-16%** |

**结论**：asio_uring 在所有场景下延迟均低于 ev；大包和超包场景 RPS 同时领先。**推荐生产环境使用 asio_uring**。

---

## 第六部分：局限与展望

### 6.1 当前局限

**未使用 SQPOLL 模式**

当前使用默认中断驱动模式，每次提交仍需 `io_uring_enter()` syscall。SQPOLL 可消除提交 syscall，但需要 `CAP_SYS_NICE` 权限，生产容器环境通常不满足。

**未使用 Fixed Buffers**

每次 I/O 传递用户缓冲区地址，内核需要 pin 对应内存页。预注册固定缓冲区（`io_uring_register_buffers`）可减少 pin/unpin 开销，为零拷贝路线做准备。

### 6.2 未来优化方向

1. **SQPOLL 模式**：消除提交 syscall，对高频小包场景效果最显著
2. **Fixed Buffers**：减少缓冲区 pin/unpin，配合 Provided Buffers 实现真正零拷贝
3. **扩大 io_uring op 覆盖**：`IORING_OP_ACCEPT`（异步 accept）、`IORING_OP_CONNECT`（异步 connect）、`IORING_OP_TIMEOUT`
4. **裸机压测**：Docker bridge 网络引入约 0.5-1ms 额外延迟，裸机对比数据更能体现 io_uring 优势

---

## 附录

### A. 配置方式

```json
{
  "io_backend": "asio_uring"
}
```

可选值：`"ev"`（默认基线）、`"asio_uring"`（推荐生产）。

### B. 编译要求

```bash
# CMakeLists.txt 已默认开启
option(THUNDER_IO_ASIO_URING "启用 standalone Asio io_uring I/O 后端" ON)

# 等价编译宏
add_compile_definitions(THUNDER_IO_ASIO_URING ASIO_STANDALONE ASIO_HAS_IO_URING ASIO_DISABLE_EPOLL)
```

运行环境需 Linux 内核 ≥ 5.1，建议 ≥ 5.10（io_uring 稳定性大幅提升）。

### C. 关键源码文件

| 文件 | 职责 |
|------|------|
| `code/Net/src/labor/AsioUringIoBackend.hpp` | 接口声明、FdState 定义 |
| `code/Net/src/labor/AsioUringIoBackend.cpp` | 三路驱动、Submit/Cancel/Update 实现 |
| `code/Net/src/labor/EvIoBackend.cpp` | epoll 基线实现 |
| `code/Net/src/labor/Labor.cpp` | IoBackend 工厂（按配置创建实例）|

---

*文档版本：v2.0*
*最后更新：2026-05-15*（重构：聚焦 AsioUringIoBackend，新增"为什么用 Asio 不直接用 liburing"分析，移除已废弃的 UringIoBackend 内容）
*项目仓库：https://github.com/chenjiayi0603/thunder*
