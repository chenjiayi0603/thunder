# Thunder 框架 io_uring 使用与原理分析

## 摘要

本文档深入分析 Thunder 实时异步通信分布式后台框架中 io_uring 的集成设计与实现。Thunder 项目采用 C++20 开发，其核心事件循环基于 libev，通过抽象的 IoBackend 接口支持三种 I/O 后端：传统的 EvIoBackend（基线实现）、手写的 UringIoBackend（实验性）以及基于 Asio 的 AsioUringIoBackend（生产级推荐）。本文档从 io_uring 基础原理出发，详细剖析 Thunder 的 IoBackend 抽象设计、三种后端的实现细节、并发模型的演进历程，以及 benchmark 性能数据背后的设计权衡。

---

## 第一部分：io_uring 基础原理

### 1.1 io_uring 是什么

io_uring 是 Linux 内核 5.1（2019年3月）引入的新一代异步 I/O 接口，由内核开发者 Jens Axboe 设计。其核心目标是通过减少用户态与内核态之间的往返次数，解决传统 POSIX AIO 和 epoll 在高并发场景下的性能瓶颈。

传统的 I/O 模型存在两个根本问题：

| 模型 | 问题 |
|------|------|
| 同步 read/write | 阻塞或轮询，CPU 效率低 |
| epoll + non-blocking I/O | 系统调用次数多（每个 I/O 操作需要多次 syscall） |
| POSIX AIO (glibc) | 功能残缺，通常内部仍是线程池模拟异步 |

io_uring 通过**共享内存 ring buffer** 实现了真正的零拷贝内核交互：

```
┌─────────────────────────────────────────────────────────────────┐
│                         User Space                               │
│  ┌─────────────────┐                    ┌─────────────────┐      │
│  │  Submission Ring │◄──── mmap ──────►│ Completion Ring │      │
│  │      (SQ)        │                   │      (CQ)       │      │
│  └────────┬─────────┘                   └────────┬────────┘      │
│           │                                      │                │
│           ▼                                      ▼                │
│  ┌─────────────────┐                    ┌─────────────────┐      │
│  │   SQE Array     │                    │   CQE Array     │      │
│  │ (Submission     │                    │  (Completion    │      │
│  │  Queue Entry)   │                    │   Queue Entry)  │      │
│  └─────────────────┘                    └─────────────────┘      │
└─────────────────────────────────────────────────────────────────┘
                              │
                    shared memory (mmap)
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                         Kernel Space                              │
│  ┌─────────────────────────────────────────────────────────┐     │
│  │                    I/O Scheduler                         │     │
│  └─────────────────────────────────────────────────────────┘     │
└─────────────────────────────────────────────────────────────────┘
```

### 1.2 核心数据结构

#### Submission Queue (SQ)

用户态向内核提交 I/O 请求的队列。每个 SQE（Submission Queue Entry）包含：

```c
struct io_uring_sqe {
    __u8    opcode;      // 操作码：IORING_OP_READ, IORING_OP_WRITE, etc.
    __u8    flags;       // 标志位
    __u16   ioprio;      // I/O 优先级
    __s32   fd;          // 文件描述符
    __u64   off;         // 偏移量（相对文件/ socket）
    __u64   addr;        // 用户缓冲区地址
    __u32   len;         // 缓冲区长度
    union {
        __u32   rw_flags;    // read/write flags
        // ... 其他字段
    };
    // ...
};
```

#### Completion Queue (CQ)

内核向用户态返回 I/O 完成通知的队列。每个 CQE（Completion Queue Entry）包含：

```c
struct io_uring_cqe {
    __u64   user_data;   // 用户数据（用于关联请求与结果）
    __s32   res;         // 系统调用返回值（类似 errno）
    __u32   flags;
};
```

**关键设计**：`user_data` 字段允许用户将任意数据（通常是请求 ID 或指针）与 SQE 关联，在 CQE 返回时可以通过它找到对应的上下文。这是 Thunder 追踪 PendingOp 的基础。

### 1.3 两种工作模式

#### 中断驱动模式（Interrupt-Driven）

默认模式，SQ 和 CQ 都是基于内存的 ring buffer：

1. 用户填充 SQE 到 SQ
2. 调用 `io_uring_enter()` 或 `io_uring_submit()` 通知内核
3. 内核完成 I/O 后将 CQE 放入 CQ
4. 用户通过 `io_uring_peek_cqe()` 或 `io_uring_wait_cqe()` 收割结果

**特点**：每次提交和收割都需要 syscall，但比传统方案（每次 I/O 操作一次 syscall）高效得多，因为可以批量提交多个 SQE。

#### SQPOLL 模式（Kernel Polling）

通过 `io_uring_setup()` 时设置 `IORING_SETUP_SQPOLL` 标志启用：

1. 内核创建一个内核线程，专门轮询 SQ
2. 用户只需填充 SQE 到 SQ（无需 syscall 通知）
3. 内核线程自动处理SQ中的请求
4. CQE 通过 eventfd 或 epoll 通知用户

**特点**：完全零 syscall 提交，但增加一个常驻内核线程。对于短连接高频率场景效果显著。

### 1.4 零拷贝能力

io_uring 提供了高级零拷贝特性：

#### Fixed Buffers

预先注册一块内存区域作为缓冲区池，内核直接使用这些缓冲区，无需每次 I/O 时的 pin/unpin 操作：

```c
// 注册固定缓冲区
io_uring_register_buffers(ring, nbuffers, bufs, buffer_sizes);

// 使用固定缓冲区
io_uring_prep_read_fixed(sqe, fd, buf, len, offset, buffer_index);
```

#### Provided Buffers

通过 `IORING_OP_PROVIDE_BUFFERS` 操作码，可以在运行时动态提供和回收缓冲区，进一步优化内存使用。

---

## 第二部分：Thunder 的 IoBackend 抽象设计

### 2.1 为什么要抽象

Thunder 框架面临一个典型的设计决策：既需要使用成熟稳定的 libev 事件循环，又要探索 io_uring 带来的性能提升。直接改造现有代码会导致：

- **耦合过紧**：业务逻辑与 I/O 机制强绑定，难以切换和测试
- **维护困难**：条件编译 `#ifdef THUNDER_IO_URING` 散落各处
- **测试复杂**：无法在同一次运行中对比不同后端的性能

Thunder 通过 **IoBackend 抽象接口** 完美解决了这些问题：

```
┌────────────────────────────────────────────────────────────────┐
│                        Thunder Server                          │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │                    Network Layer                          │  │
│  │  ┌────────────────────────────────────────────────────┐  │  │
│  │  │              IoBackend Interface                    │  │  │
│  │  │  + Init()                                            │  │  │
│  │  │  + Destroy()                                         │  │  │
│  │  │  + SubmitRead(fd, buf) -> seq                       │  │  │
│  │  │  + SubmitWrite(fd, buf) -> seq                      │  │  │
│  │  │  + CancelFd(fd)                                      │  │  │
│  │  │  + HasPending(fd) -> bool                           │  │  │
│  │  │  + Name() -> string                                 │  │  │
│  │  └────────────────────────────────────────────────────┘  │  │
│  └──────────────────────────────────────────────────────────┘  │
│                              │                                   │
│         ┌────────────────────┼────────────────────┐             │
│         ▼                    ▼                    ▼             │
│  ┌─────────────┐     ┌─────────────┐     ┌─────────────────┐    │
│  │ EvIoBackend  │     │UringIoBackend│     │AsioUringIoBackend│   │
│  │  (Baseline)  │     │ (Experimental)│    │ (Production)    │    │
│  └─────────────┘     └─────────────┘     └─────────────────┘    │
└────────────────────────────────────────────────────────────────┘
```

### 2.2 IoBackend 接口详解

```cpp
// IoBackend.hpp
enum class IoOp { Read, Write };

using IoCompletionCallback = std::function<void(
    int fd,              // 文件描述符
    IoOp op,             // 操作类型
    const asemsg::Slice& buf,  // 数据缓冲区
    int res              // 操作结果（字节数或错误码）
)>;

class IoBackend {
public:
    virtual ~IoBackend() = default;
    
    // 生命周期管理
    virtual bool Init(struct ev_loop* loop) = 0;
    virtual void Destroy() = 0;
    
    // 异步 I/O 操作
    virtual void SubmitRead(int fd, std::shared_ptr<asemsg::IOBuf> buf,
                           IoCompletionCallback&& cb) = 0;
    virtual void SubmitWrite(int fd, std::shared_ptr<asemsg::IOBuf> buf,
                             IoCompletionCallback&& cb) = 0;
    
    // 连接管理
    virtual void CancelFd(int fd) = 0;
    virtual bool HasPending(int fd) const = 0;
    
    // 调试信息
    virtual std::string Name() const = 0;
};
```

**设计要点**：

1. **Callback 模式**：使用 `std::function` 而非模板回调，编译速度更快
2. **智能指针管理 Buffer**：通过 `shared_ptr<IOBuf>` 确保缓冲区生命周期
3. **统一的 fd 取消接口**：连接关闭时需要安全地取消所有待处理的 I/O
4. **HasPending 查询**：关闭连接前检查是否有未完成的 I/O

### 2.3 三种实现的定位

| 后端 | 定位 | 适用场景 | 代码成熟度 |
|------|------|----------|-----------|
| **EvIoBackend** | 基线/稳定 | 所有场景的首选备选 | ★★★★★ |
| **UringIoBackend** | 实验性探索 | 验证 io_uring API | ★★★ |
| **AsioUringIoBackend** | 生产级推荐 | Linux 高性能服务器 | ★★★★ |

### 2.4 与 libev 事件循环的集成

Thunder 使用 libev 作为主事件循环，io_uring 后端需要与之协作。核心技巧是：**将 io_uring 的 ring_fd 注册到 libev 的 epoll 实例中**。

```
┌─────────────────────────────────────────────────────────────────┐
│                        libev Event Loop                         │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                     epoll instance                        │    │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐        │    │
│  │  │ fd: 5   │ │ fd: 8   │ │fd: ring │ │ fd: 12  │        │    │
│  │  │(socket) │ │(socket) │ │(uring)  │ │(socket) │        │    │
│  │  └────┬────┘ └────┬────┘ └────┬────┘ └────┬────┘        │    │
│  └───────┼───────────┼───────────┼───────────┼─────────────┘    │
│          │           │           │           │                   │
└──────────┼───────────┼───────────┼───────────┼───────────────────┘
           │           │           │           │
           ▼           ▼           ▼           ▼
     socket data   socket data   CQ ready   socket data
     (epoll IN)    (epoll IN)    (polled)   (epoll IN)
```

当 io_uring 的 ring_fd 可读时，表示有 CQE 可收割。这是实现"主线程直驱"架构的关键。

---

## 第三部分：UringIoBackend 深度分析

### 3.1 初始化流程

```cpp
// UringIoBackend.cpp
bool UringIoBackend::Init(struct ev_loop* loop) {
    // 1. 初始化 io_uring，队列深度 256
    struct io_uring_params params = {};
    params.flags = 0;  // 暂不使用 SQPOLL
    if (io_uring_queue_init_params(kQueueDepth, &m_ring, &params) < 0) {
        return false;
    }
    
    // 2. 获取 ring_fd 并注册到 libev
    m_ringFd = m_ring.ring_fd;
    ev_io_init(&m_ringWatcher, RingEventCallback, m_ringFd, EV_READ);
    m_ringWatcher.data = this;
    ev_io_start(loop, &m_ringWatcher);
    
    return true;
}
```

**配置参数**：

```cpp
static constexpr int kQueueDepth = 256;      // SQ/CQ 容量
static constexpr int kMaxCqeBatch = 32;     // 单次收割上限
```

### 3.2 读操作全流程

```
┌────────────┐     ┌────────────┐     ┌────────────┐     ┌────────────┐
│ SubmitRead  │────►│ Fill SQE   │────►│  Submit to  │────►│ Wait for   │
│  (Caller)   │     │            │     │  Kernel SQ  │     │  CQ Ready  │
└────────────┘     └────────────┘     └────────────┘     └─────┬──────┘
                                                               │
                                    ┌──────────────────────────┘
                                    ▼
┌────────────┐     ┌────────────┐     ┌────────────┐     ┌────────────┐
│  Invoke    │◄────│ Callbacks  │◄────│  ReapCqes  │◄────│  RingEvent │
│  Complete  │     │  Stored    │     │  (batch)   │     │  Callback  │
└────────────┘     └────────────┘     └────────────┘     └────────────┘
```

**代码实现**：

```cpp
void UringIoBackend::SubmitRead(int fd, std::shared_ptr<asemsg::IOBuf> buf,
                                IoCompletionCallback&& cb) {
    // 1. 分配全局唯一序列号
    uint64_t seq = m_nextSeq++;
    
    // 2. 构造待处理操作上下文
    auto op = std::make_shared<PendingOp>(fd, seq, IoOp::Read, std::move(buf), std::move(cb));
    m_mapPending[seq] = op;
    
    // 3. 获取 SQE 并填充
    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    io_uring_prep_recv(sqe, fd, op->buf->Data(), op->buf->Capacity(), 0);
    sqe->user_data = seq;  // 关键：关联 seq
    
    // 4. 提交到内核
    io_uring_submit(&m_ring);
}

void UringIoBackend::RingEventCallback(struct ev_loop* loop, ev_io* w, int revents) {
    auto* self = static_cast<UringIoBackend*>(w->data);
    self->ReapCqes();
}

void UringIoBackend::ReapCqes() {
    struct io_uring_cqe* cqe;
    unsigned head;
    int count = 0;
    
    // 批量收割 CQE
    io_uring_for_each_cqe(&m_ring, head, cqe) {
        uint64_t seq = cqe->user_data;           // 取出序列号
        auto it = m_mapPending.find(seq);
        if (it != m_mapPending.end()) {
            auto op = std::move(it->second);
            m_mapPending.erase(it);
            
            // 调用完成回调
            op->callback(op->fd, op->op, op->buf->Slice(), cqe->res);
        }
        if (++count >= kMaxCqeBatch) break;  // 限制单次处理量
    }
    
    // 标记已消费的 CQE
    io_uring_cq_advance(&m_ring, count);
}
```

### 3.3 PendingOp 追踪机制

```cpp
struct PendingOp : std::enable_shared_from_this<PendingOp> {
    int                     fd;     // 文件描述符
    uint64_t                seq;    // 全局序列号
    IoOp                    op;     // 操作类型
    std::shared_ptr<IOBuf>  buf;    // 缓冲区
    IoCompletionCallback    callback;// 完成回调
    
    PendingOp(int fd_, uint64_t seq_, IoOp op_,
              std::shared_ptr<IOBuf> buf_, IoCompletionCallback&& cb)
        : fd(fd_), seq(seq_), op(op_), 
          buf(std::move(buf_)), callback(std::move(cb)) {}
};
```

**user_data 设计**：使用单调递增的 uint64 作为 seq，每次 Submit 递增 `m_nextSeq`。这确保了：

1. **全局唯一性**：即使 fd 被关闭后重用，seq 仍然不同
2. **紧凑性**：uint64 比指针更紧凑，适合做 map key
3. **可追溯性**：seq 的单调性便于调试和日志分析

### 3.4 CancelFd 实现

```cpp
void UringIoBackend::CancelFd(int fd) {
    // 遍历所有待处理操作，移除指定 fd 的条目
    for (auto it = m_mapPending.begin(); it != m_mapPending.end(); ) {
        if (it->second->fd == fd) {
            it = m_mapPending.erase(it);  // 安全删除
        } else {
            ++it;
        }
    }
}
```

**注意**：这里没有向 io_uring 提交取消请求（`io_uring_prep_cancel`），而是简单地移除追踪记录。当该操作的 CQE 返回时，会因为找不到对应的 PendingOp 而被忽略。

### 3.5 写操作设计决策

UringIoBackend 的 **Write 操作使用同步写入**：

```cpp
void UringIoBackend::SubmitWrite(int fd, std::shared_ptr<asemsg::IOBuf> buf,
                                  IoCompletionCallback&& cb) {
    // 直接同步写入，不走 io_uring
    ssize_t written = ::send(fd, buf->Data(), buf->Length(), 0);
    
    // 同步调用回调
    cb(fd, IoOp::Write, buf->Slice(), written);
}
```

**设计理由**（代码注释）：

> 注释：io_uring 异步写入与 TLS/codec 状态机交互不好，小包/缓冲发送内核本身同步完成

具体分析：

1. **TLS/codec 状态机冲突**：Thunder 使用自定义协议 codec，Write 操作涉及多阶段的协议处理。io_uring 的异步模型与这种状态机交互会导致复杂的生命周期管理问题。

2. **小包特性**：网络发送的数据往往是协议封包后的完整消息，单次 send 即可完成。内核对这种"小包同步发送"有专门优化（TCP_NODELAY、TCP_CORK）。

3. **缓存刷新**：即使 io_uring 异步写成功，用户空间的缓冲区也不能立即释放，同步模式更符合 Thunder 的 IOBuf 管理语义。

### 3.6 性能特征与限制

**优点**：

- 批量提交减少 syscall 次数
- 内存映射的 SQ/CQ 减少数据拷贝
- 编程接口简洁（相比 epoll + non-blocking read/write）

**局限**：

- 写操作退化为同步，未利用 io_uring 优势
- 未使用 SQPOLL 模式，仍有 syscall 开销
- 未使用 fixed buffers，缓冲区管理效率有限
- 单线程收割可能有瓶颈

---

## 第四部分：AsioUringIoBackend 深度分析

### 4.1 设计动机

UringIoBackend 证明了 Thunder 可以集成 io_uring，但其局限性（写操作同步）促使团队寻找更完整的解决方案。AsioUringIoBackend 应运而生，它基于 Boost.Asio 的 io_uring 后端，充分利用成熟的 Asio 生态。

**为什么不用 Asio 的 epoll 后端而专门用 io_uring 后端？**

- Asio 的 epoll 后端本质上是传统模型的封装
- Asio io_uring 后端提供了更现代的 API（async_read_some, async_write_some）
- 与现有 libev 架构的集成更自然

### 4.2 并发模型演进

这是理解 AsioUringIoBackend 设计的关键。

#### 第一代：独立线程 + ev_async 桥接

```
┌─────────────────────┐       ev_async        ┌─────────────────────┐
│   Main Thread       │◄─────────────────────►│  io_context Thread  │
│   (libev loop)      │       (wakeup)        │  (Asio dispatcher)  │
│                     │                       │                     │
│  ev_async watcher   │                       │   io_context.run() │
│  ─────────────────  │                       │         │          │
│  invoke callbacks   │                       │         ▼          │
└─────────────────────┘                       │   process CQEs     │
                                              └─────────────────────┘
```

**问题**：

- 跨线程 syscall：`ev_async_send()` 本身是 syscall
- 线程间数据传递需要加锁或 lock-free 队列
- 上下文切换开销
- 调试困难（多线程 + 异步回调交叉）

#### 第二代（最终方案）：主线程直驱

```
┌─────────────────────────────────────────────────────────────────┐
│                        Main Thread                                │
│  ┌───────────────────────────────────────────────────────────┐   │
│  │                    libev Event Loop                       │   │
│  │                                                             │   │
│  │   ev_prepare ──► asio::io_context::poll()                  │   │
│  │       │                     │                              │   │
│  │       │                     ▼                              │   │
│  │       │              ┌──────────────┐                      │   │
│  │       │              │  Asio       │                      │   │
│  │       │              │  Dispatcher │                      │   │
│  │       │              └──────────────┘                      │   │
│  │       │                     │                              │   │
│  │   ev_io(ring_fd) ◄──────────┘                              │   │
│  │   (CQ ready notify)                                        │   │
│  │       │                                                    │   │
│  │   ev_check ──► process CQE callbacks                      │   │
│  │                                                             │   │
│  └───────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

**优势**：

- **零锁**：所有操作在单线程完成
- **零线程跳**：无跨线程数据传递
- **零额外 syscall**：ring_fd 通过 epoll 整合到 libev
- **代码简洁**：逻辑顺序执行，易于调试

### 4.3 AsioUringIoBackend 的 io_uring 模式

**当前模式：默认中断驱动模式（Interrupt-Driven），非 SQPOLL。**

| 模式 | 是否使用 | 说明 |
|------|---------|------|
| 默认模式（中断驱动） | ✅ 当前使用 | 每次 SQE 提交需调用 `io_uring_enter()` syscall，CQ 完成通过 ring_fd 可读事件通知 |
| SQPOLL 模式 | ❌ 未使用 | 内核线程轮询 SQ，零 syscall 提交，但需要 CAP_SYS_NICE 权限 |
| IOPOLL 模式 | ❌ 未使用 | 仅用于 O_DIRECT 块设备，与网络 I/O 无关 |

**确认依据**：

1. `AsioUringIoBackend::Init()` 中需要调用 `FindIoUringRingFd()` 找到 ring_fd 并注册到 libev 的 epoll 实例——这是中断驱动模式的标志行为。SQPOLL 模式下内核线程主动轮询 SQ，不需要 ring_fd 通知机制。
2. Asio 的 io_uring 后端内部调用 `io_uring_queue_init(entries, &ring, 0)`，flags=0 不包含 `IORING_SETUP_SQPOLL`。
3. AsioUringIoBackend 注册了 `ev_io(ring_fd, EV_READ)` 来监听 CQE 到达——SQPOLL 模式不需要这样做。
4. `io_context{1}` 构造参数 hint=1 表示建议单线程运行，与 SQPOLL 无关。

### 4.4 并发模型：单线程主循环直驱

**核心事实：AsioUringIoBackend 只有一个线程——即 Worker 进程的主线程（libev 事件循环线程）。**

```
┌────────────────────────────────────────────────────────────────────────┐
│                        Worker 进程（单线程）                            │
│                                                                        │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                    libev Event Loop                              │  │
│  │                                                                  │  │
│  │   ┌──────────┐                                                   │  │
│  │   │ev_prepare│──► io_context.poll()                              │  │
│  │   └──────────┘    ├─ 提交待发送的 SQE（io_uring_enter）           │  │
│  │                   ├─ 收割已完成的 CQE                             │  │
│  │                   └─ 调用 completion handler                      │  │
│  │                          │                                       │  │
│  │                          ▼                                       │  │
│  │   ┌─────────────────────────────────┐                            │  │
│  │   │      epoll_wait (libev 核心)     │                            │  │
│  │   │  监听的 fd:                      │                            │  │
│  │   │    • ring_fd (io_uring CQ 通知)  │                            │  │
│  │   │    • pipe/信号/timer 等 libev fd │                            │  │
│  │   │    • 共享内存 eventfd            │                            │  │
│  │   └──────────┬──────────────────────┘                            │  │
│  │              │                                                   │  │
│  │              ▼                                                   │  │
│  │   ┌─────────────────────────────────┐                            │  │
│  │   │ev_io(ring_fd)                   │                            │  │
│  │   │  ring_fd 可读 → CQE 到达        │                            │  │
│  │   │  → io_context.poll()            │                            │  │
│  │   │    收割 CQE + 调用 handler       │                            │  │
│  │   └─────────────────────────────────┘                            │  │
│  │              │                                                   │  │
│  │              ▼                                                   │  │
│  │   ┌──────────┐                                                   │  │
│  │   │ev_check  │──► io_context.poll()                              │  │
│  │   └──────────┘    处理 epoll_wait 期间到达的 CQE                  │  │
│  │                                                                  │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│                                                                        │
│  涉及的线程：仅 1 个（主线程 = libev 事件循环线程 = io_context 线程）     │
│  额外线程数：0                                                          │
│  锁数量：0（单线程，无竞争）                                             │
└────────────────────────────────────────────────────────────────────────┘
```

**三路驱动的时序关系**：

```
时间 ──────────────────────────────────────────────────────────────►

     ┌─────────┐   ┌──────────────┐   ┌─────────────┐   ┌────────┐
     │ev_prepare│   │  epoll_wait  │   │ev_io(ring_fd)│   │ev_check│
     └────┬─────┘   └──────┬───────┘   └──────┬──────┘   └───┬────┘
          │                │                   │               │
          ▼                │                   │               │
     io_context.poll()    │                   │          io_context.poll()
     ├─submit SQEs       │                   │          ├─reap CQEs
     ├─reap CQEs         │                   │          └─call handlers
     └─call handlers     │                   │
                          │                   ▼
                          │          ring_fd 可读?
                          │          ├─ Yes → io_context.poll()
                          │          │   ├─reap CQEs
                          │          │   └─call handlers ←── 关键！
                          │          └─ No → 跳过
                          │
                     等待事件
                     (阻塞在此)
```

### 4.5 消息完整流程

#### 4.5.1 读操作完整流程

```
  网络层                   内核                     用户态 (AsioUringIoBackend)              用户态 (Worker)
    │                       │                            │                                      │
    │   客户端发送数据       │                            │                                      │
    │──────────────────────►│                            │                                      │
    │                       │ 数据到达 socket 缓冲区       │                                      │
    │                       │                            │                                      │
    │                       │              ◄─────────────│ SubmitRead(fd, buf, seq)               │
    │                       │                            │ ├─ EnsureFdState(fd)                   │
    │                       │                            │ ├─ sock.async_read_some(buffer,handler)│
    │                       │                            │ │  Asio 内部:                          │
    │                       │                            │ │  └─ 写 SQE(IORING_OP_RECV) 到 SQ     │
    │                       │                            │ │                                      │
    │                       │              ◄─────────────│ ev_prepare → io_context.poll()        │
    │                       │  io_uring_enter()          │ ├─ 提交 SQ 中的 SQE 到内核              │
    │                       │───────────────────────────►│ └─ 收割已有 CQE                       │
    │                       │                            │                                      │
    │                       │ 内核执行 recv               │                                      │
    │                       │ 数据从 socket buffer        │                                      │
    │                       │ 拷贝到用户 buffer           │                                      │
    │                       │ 生成 CQE(res=字节数)        │                                      │
    │                       │                            │                                      │
    │                       │  ring_fd 变为可读           │                                      │
    │                       │───────────────────────────►│                                      │
    │                       │                            │ epoll_wait 返回                       │
    │                       │                            │ ev_io(ring_fd) 触发                   │
    │                       │                            │ → io_context.poll()                   │
    │                       │                            │ ├─ reap CQE                           │
    │                       │                            │ └─ 调用 async_read_some handler       │
    │                       │                            │    ├─ wp.lock() 检查 FdState 存活     │
    │                       │                            │    ├─ buf->AdvanceWriteIndex(n)       │
    │                       │                            │    └─ m_callback(fd,seq,Read,n,ud) ──►│
    │                       │                            │                                      │ IoRead()
    │                       │                            │                                      │ → RecvDataAndDispose()
    │                       │                            │                                      │ → codec->Decode()
    │                       │                            │                                      │ → 业务逻辑
```

#### 4.5.2 写操作完整流程

```
  用户态 (Worker)           用户态 (AsioUringIoBackend)              内核                      网络层
      │                            │                                  │                          │
      │ 业务逻辑生成响应             │                                  │                          │
      │ SendTo()                    │                                  │                          │
      │──► codec->Encode()          │                                  │                          │
      │    HTTP 明文写入 CBuffer     │                                  │                          │
      │                            │                                  │                          │
      │──► SubmitWrite(fd,buf,seq) ─►│                                  │                          │
      │                            │ ├─ EnsureFdState(fd)              │                          │
      │                            │ ├─ sock.async_write_some(buffer,  │                          │
      │                            │ │     handler)                    │                          │
      │                            │ │  Asio 内部:                      │                          │
      │                            │ │  └─ 写 SQE(IORING_OP_SEND)到SQ  │                          │
      │                            │ │                                 │                          │
      │                            │ ◄─── ev_prepare/ev_check          │                          │
      │                            │      → io_context.poll()          │                          │
      │                            │ ├─ 提交 SQ 到内核                  │                          │
      │                            │ │  io_uring_enter() ──────────────►│                          │
      │                            │ │                                 │                          │
      │                            │ │                    内核执行 send  │                          │
      │                            │ │                    数据从用户     │                          │
      │                            │ │                    buffer 发出    │                          │
      │                            │ │                    生成 CQE       │    数据到达网卡            │
      │                            │ │                                  │──────────────────────────►│
      │                            │ │                    ring_fd 可读   │                          │
      │                            │ │◄───────────────────────────────── │                          │
      │                            │ │                                  │                          │
      │                            │ │ io_context.poll()                │                          │
      │                            │ │ ├─ reap CQE                      │                          │
      │                            │ │ └─ 调用 async_write_some handler │                          │
      │                            │ │    ├─ wp.lock() 检查 FdState     │                          │
      │                            │ │    ├─ buf->AdvanceReadIndex(n)   │                          │
      │                            │ │    └─ m_callback(fd,seq,Write,n) │                          │
      │                            │ │       ──────────────────────────►│                          │
      │ ◄─── 写完成回调 ──────────── │                                  │                          │
      │                            │                                  │                          │
```

#### 4.5.3 io_uring SQE/CQE 生命周期（关键）

```
用户态                                              内核态
  │                                                   │
  │  1. async_read_some()                             │
  │     Asio 写 SQE 到 SQ ring                        │
  │     (SQE: op=IORING_OP_RECV, fd, buf_addr, len)  │
  │                                                   │
  │  2. io_context.poll()                             │
  │     → io_uring_enter(to_submit=N, min_complete=0) │
  │──────────────────────────────────────────────────►│
  │                                                   │  3. 内核从 SQ 取出 SQE
  │                                                   │     执行 recv 操作
  │                                                   │     数据写入用户 buf
  │                                                   │     写 CQE 到 CQ ring
  │                                                   │     (CQE: user_data, res=字节数)
  │                                                   │     ring_fd 变为可读
  │                                                   │
  │  4. epoll_wait 返回 (ring_fd 可读)                  │
  │◄──────────────────────────────────────────────────│
  │                                                   │
  │  5. io_context.poll()                             │
  │     → io_uring_for_each_cqe()                     │
  │     → 读取 CQE                                    │
  │     → io_uring_cqe_seen() (确认消费)               │
  │     → 调用对应的 completion handler                │
  │                                                   │
  └───────────────────────────────────────────────────┘

关键系统调用：
  - io_uring_setup(): 初始化，创建 SQ/CQ 共享内存（仅一次）
  - io_uring_enter(): 提交 SQE + 可选等待 CQE（每次提交时调用）
  - ring_fd read event: CQ 有完成事件时的通知机制
```

#### 4.5.4 涉及的线程总结

| 线程 | 数量 | 职责 |
|------|------|------|
| **Worker 主线程** | 1 | 运行 libev 事件循环 + io_context.poll()，处理所有 I/O 回调和业务逻辑 |
| io_context 内部线程 | 0 | io_context 只在主线程上被 poll()，不创建额外线程 |
| Asio resolver 线程 | 0-1 | DNS 解析可能创建线程（Thunder 不使用 Asio DNS） |
| 内核 SQ 线程 (SQPOLL) | 0 | 未启用 SQPOLL，无此线程 |

**结论：AsioUringIoBackend 在正常网络 I/O 场景下只使用 1 个线程（Worker 主线程）。**

### 4.6 主线程直驱架构详解

```cpp
bool AsioUringIoBackend::Init(struct ev_loop* loop) {
    m_loop = loop;
    
    // 1. 创建 io_context（Asio 调度器）
    m_ioContext = std::make_unique<asio::io_context>(1);  // 1 个线程
    
    // 2. 找到 io_uring 的 ring_fd
    m_ringFd = FindIoUringRingFd(*m_ioContext);
    
    // 3. 注册三路驱动
    // 3.1 ev_prepare：在事件循环开始前处理 Asio 内部任务
    ev_prepare_init(&m_prepareWatcher, PrepareCallback);
    m_prepareWatcher.data = this;
    ev_prepare_start(loop, &m_prepareWatcher);
    
    // 3.2 ev_check：在事件处理后收割完成的回调
    ev_check_init(&m_checkWatcher, CheckCallback);
    m_checkWatcher.data = this;
    ev_check_start(loop, &m_checkWatcher);
    
    // 3.3 ev_io：监听 ring_fd 可读（表示 CQ 有完成事件）
    ev_io_init(&m_ringWatcher, RingFdCallback, m_ringFd, EV_READ);
    m_ringWatcher.data = this;
    ev_io_start(loop, &m_ringWatcher);
    
    return true;
}
```

**三路驱动职责**：

| 阶段 | 触发时机 | 职责 |
|------|---------|------|
| `ev_prepare` | 主循环开始前 | `io_context.poll()` 处理 Asio 内部队列（提交 SQ） |
| `ev_io(ring_fd)` | ring_fd 可读 | CQ 有完成事件，内核已处理完 SQE |
| `ev_check` | 主循环结束后 | `io_context.poll()` 收割完成的回调 |

### 4.7 FindIoUringRingFd 的实现技巧

Asio 的 io_uring 后端创建了一个匿名 inotify 来管理 ring_fd，我们需要通过 `/proc/self/fd` 遍历找到它：

```cpp
int AsioUringIoBackend::FindIoUringRingFd(asio::io_context& ioContext) {
    // 打开 /proc/self/fd 目录
    int dirFd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY);
    
    std::unique_ptr<DIR, decltype(&closedir)> dir(fdopendir(dirFd), closedir);
    struct dirent* entry;
    
    while ((entry = readdir(dir.get())) != nullptr) {
        // 跳过 . 和 ..
        if (entry->d_name[0] == '.') continue;
        
        // 读取 fd 符号链接
        char linkPath[256];
        snprintf(linkPath, sizeof(linkPath), "/proc/self/fd/%s", entry->d_name);
        
        char target[512];
        ssize_t len = readlink(linkPath, target, sizeof(target) - 1);
        if (len > 0) {
            target[len] = '\0';
            
            // 匹配 io_uring 匿名 inode
            if (strncmp(target, "anon_inode:[io_uring]", 19) == 0) {
                return std::stoi(entry->d_name);
            }
        }
    }
    
    return -1;
}
```

**为什么需要这个技巧？**

Asio io_uring 后端的 ring_fd 没有直接暴露给用户，需要通过遍历 fd 列表来发现。这是当前 Asio API 的局限性。

### 4.8 FdState 生命周期管理

每个 socket fd 对应一个 FdState：

```cpp
struct FdState : std::enable_shared_from_this<FdState> {
    asio::posix::stream_descriptor  stream;   // Asio 封装的文件描述符
    bool                            readPending = false;
    bool                            writePending = false;
    bool                            cancelled = false;
    
    std::weak_ptr<FdState>          weakSelf;  // 防止回调时对象已销毁
    
    FdState(asio::io_context& ioCtx, int fd) 
        : stream(ioCtx, fd) {}
};
```

**weak_ptr 防悬挂机制**：

```cpp
void AsioUringIoBackend::SubmitRead(int fd, std::shared_ptr<asemsg::IOBuf> buf,
                                     IoCompletionCallback&& cb) {
    // 获取或创建 FdState
    auto state = GetOrCreateFdState(fd);
    state->readPending = true;
    
    // 保存 weak_ptr 到回调中
    auto weakState = state->weakSelf;
    
    // 发起异步读
    state->stream.async_read_some(
        asio::buffer(buf->Data(), buf->Capacity()),
        [this, weakState, fd, buf, cb=std::move(cb)](
            const std::error_code& ec, size_t bytes) {
            
            // 检查 state 是否仍然有效
            auto state = weakState.lock();
            if (!state) return;  // 已销毁，忽略回调
            
            state->readPending = false;
            
            if (!ec && !state->cancelled) {
                cb(fd, IoOp::Read, buf->Slice(), static_cast<int>(bytes));
            }
        }
    );
}
```

**为什么要用 weak_ptr？**

考虑以下场景：

1. 异步读已提交
2. 连接关闭，`CancelFd` 被调用
3. 稍后内核返回读完成事件

如果没有 weak_ptr，第 3 步的回调仍会执行，可能访问已释放的资源。weak_ptr 确保回调执行时能检测到对象是否仍然存活。

### 4.9 async_read_some / async_write_some 映射到 io_uring

Asio io_uring 后端自动将 Asio 的异步操作映射为 io_uring 的 SQE：

```cpp
// 读操作映射
// Asio: async_read_some(buffer, handler)
// 底层: io_uring_prep_recv(sqe, fd, buffer, len, 0)

// 写操作映射
// Asio: async_write_some(buffer, handler)  
// 底层: io_uring_prep_send(sqe, fd, buffer, len, 0)
```

这意味着 **AsioUringIoBackend 的写操作是真正的异步**，解决了 UringIoBackend 的主要局限。

### 4.10 fix #2 前后关键变化对照

> fix #2（PR #3，closes issue #2）针对 Interface CPU 31% / diag.log 19GB 忙循环问题，
> 仅改动 `AsioUringIoBackend.cpp/.hpp`，共四项：

| 变化 | 旧实现 | 当前实现 |
|------|--------|---------|
| ring_fd 监听时机 | Init() 时立即 ev_io_start，永远监听 | 按需：SubmitRead/Write 时 start，无挂起 op 时 stop |
| SubmitRead/SubmitWrite 内联 poll() | 有，每次 Submit 同步触发 scheduler | 已删除，SQE 由 OnPrepare 批量 flush |
| diag_log 输出 | 无条件写文件 + fflush | 受 THUNDER_ASIO_URING_DIAG=1 env 门控，默认关 |
| 诊断计数器 | 无 | 每秒输出 ring_ready/ring_empty/ring_real 等指标 |

---

### 4.11 当前并发模型全景

#### 4.11.1 ev_run 主循环驱动图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                  Worker 进程主线程  —  libev ev_run 单循环               │
│                                                                         │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │ ① ev_prepare（epoll_wait 之前）                                  │   │
│  │                                                                  │   │
│  │   io_context.poll()                                              │   │
│  │     ├─ 若 ASIO 内部有待提交 SQE:                                 │   │
│  │     │    submit_sqes_op 被调度 → io_uring_submit() → 内核        │   │
│  │     └─ 若 CQ 中已有完成 CQE（少见，上轮漏收割）:                  │   │
│  │          收割 + 调用 completion handler                           │   │
│  └──────────────────────────────┬──────────────────────────────────┘   │
│                                 │                                       │
│                                 ▼                                       │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │ ② epoll_wait（libev 唯一阻塞点）                                 │   │
│  │                                                                  │   │
│  │   监听的 fd 集合:                                                 │   │
│  │     • ring_fd  ← 仅当 m_ringWatcherActive=true 时在集合内        │   │
│  │     • 其它 libev 管理的 fd / timer / signal                      │   │
│  │                                                                  │   │
│  │   阻塞直到：ring_fd 可读（有 CQE）或其它 fd 就绪                  │   │
│  └──────────────────────────────┬──────────────────────────────────┘   │
│                                 │ epoll_wait 返回                       │
│              ┌──────────────────┴───────────────────┐                  │
│              │                                       │                  │
│              ▼  ring_fd 在事件集中                    ▼  其它 fd 就绪   │
│  ┌───────────────────────────┐            ┌─────────────────────────┐  │
│  │ ③ ev_io(ring_fd) 触发    │            │  其它 libev 事件处理     │  │
│  │                           │            │  (timer / signal / 其他  │  │
│  │   OnRingReady:            │            │   socket fd 等)          │  │
│  │   io_context.poll()       │            └─────────────────────────┘  │
│  │   ├─ io_uring_peek_cqe   │                                          │
│  │   │    有 CQE → 收割       │                                          │
│  │   │    completion handler │                                          │
│  │   │    readPending=false  │                                          │
│  │   │    m_callback(...)    │                                          │
│  │   └─ 计数 ring_ready/     │                                          │
│  │       ring_empty/real     │                                          │
│  └───────────────────────────┘                                          │
│                                 │                                       │
│                                 ▼                                       │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │ ④ ev_check（epoll_wait 之后）                                    │   │
│  │                                                                  │   │
│  │   io_context.poll()   ← 捞漏网 CQE（epoll_wait 窗口期间到达的）  │   │
│  │   UpdateRingWatcher() ← 扫描 m_fds，全部 idle 则 ev_io_stop      │   │
│  │   g_stats.tick()      ← 每秒输出诊断计数                          │   │
│  └──────────────────────────────┬──────────────────────────────────┘   │
│                                 │                                       │
│                                 └──────────── 下一轮 ev_run ────────────┘
│                                                                         │
│  线程数：1（主线程 = libev 线程 = io_context 调用线程）                   │
│  锁数量：0（单线程，无竞争）                                              │
└─────────────────────────────────────────────────────────────────────────┘
```

#### 4.11.2 ring_fd 按需启停状态机

```
初始状态: m_ringWatcherActive = false（Init 不 start）
                │
    SubmitRead / SubmitWrite 调用
                │
                ▼
  ┌─────────────────────────────────────┐
  │  sp->readPending = true             │
  │  async_read_some()                  │
  │    └─ ASIO start_op: SQE 入内部队列 │
  └─────────────────┬───────────────────┘
                    │
                    ▼
          UpdateRingWatcher()
          hasOp = true（m_fds 有挂起 op）
                    │
        m_ringWatcherActive == false?
          ┌─── YES ──────┐
          ▼              │
  ev_io_start(ring_fd)   │  NO → 保持不变
  m_ringWatcherActive = true
          │
          ▼（下一轮 epoll_wait 包含 ring_fd）
          │
  [OnPrepare] io_context.poll()
          │  → submit_sqes_op 调度
          │  → io_uring_submit() 提交 SQE
          │
  [内核处理 IO]
          │
  [ring_fd 变可读] → epoll_wait 返回
          │
  [OnRingReady] io_context.poll()
          │  → io_uring_peek_cqe: 收割 CQE
          │  → ReadComplete/WriteComplete handler
          │     readPending = false
          │     m_callback(fd, seq, Read, n)
          │
  [OnCheck] UpdateRingWatcher()
          │  扫描 m_fds: 还有挂起 op?
          │
     ┌─── YES ──────────────────── NO ──────┐
     │                                       ▼
  保持 ev_io_start                   ev_io_stop(ring_fd)
                                     m_ringWatcherActive = false
                                     （下一轮 epoll_wait 不包含 ring_fd，
                                       NOP CQE 不再唤醒事件循环）
```

#### 4.11.3 SQE 提交时序（批量 flush 模式）

```
时间轴 ─────────────────────────────────────────────────────────────►

  SubmitRead(fd1)                    SubmitRead(fd2)
       │                                  │
       │ async_read_some()                │ async_read_some()
       │   SQE#1 → ASIO 内部队列          │   SQE#2 → ASIO 内部队列
       │ (不再有内联 poll)                  │ (不再有内联 poll)
       │                                  │
       └──────────────┬───────────────────┘
                      │
               ev_prepare 触发
                      │
              io_context.poll()
                      │
              submit_sqes_op 调度
                      │
              io_uring_submit()  ← 两个 SQE 批量一次提交
                      │
            ┌─────────▼─────────┐
            │  内核处理 SQE#1    │
            │  内核处理 SQE#2    │  ← 并发执行
            └─────────┬─────────┘
                      │（两个 IO 完成，各写一个 CQE）
              ring_fd 变可读
                      │
              ev_io(ring_fd) 触发
              io_context.poll()
                      │
              io_uring_peek_cqe × 2
              ReadComplete(fd1)
              ReadComplete(fd2)
              m_callback × 2


  对比（旧实现，有内联 poll）：
  每次 SubmitRead 末尾各触发一次 io_context.poll()
  → 每次 poll 触发 scheduler → interrupt() → NOP SQE
  → 两次 SubmitRead 产生 2 个额外 NOP，ring_fd 被持续唤醒
```

#### 4.11.4 完整读数据链路（端到端）

```
  网络对端        内核           ASIO/io_uring       Thunder Worker 主线程
     │             │                  │                       │
     │── TCP 数据 ─►│                  │                       │
     │             │                  │  SubmitRead(fd,buf)   │
     │             │◄─────────────────│  async_read_some()    │
     │             │  SQE(RECV,fd)    │  SQE 入内部队列        │
     │             │                  │                       │
     │             │                  │──── ev_prepare ───────│
     │             │  io_uring_submit │  io_context.poll()    │
     │             │◄─────────────────│  → flush SQE          │
     │             │                  │                       │
     │             │ recv(fd,buf,len) │                       │
     │             │ 数据写入 buf     │                       │
     │             │ 写 CQE(res=n)    │                       │
     │             │                  │                       │
     │             │ ring_fd 可读     │                       │
     │             │─────────────────►│                       │
     │             │                  │─── epoll_wait 返回 ───│
     │             │                  │─── ev_io(ring_fd) ────│
     │             │                  │    io_context.poll()  │
     │             │                  │    io_uring_peek_cqe  │
     │             │                  │    ReadComplete hdlr  │
     │             │                  │    readPending=false   │
     │             │                  │    buf.AdvanceWrite(n) │
     │             │                  │    m_callback(fd,n) ──►│
     │             │                  │                       │ RecvDataAndDispose()
     │             │                  │                       │ codec->Decode()
     │             │                  │                       │ → 业务逻辑
     │             │                  │─── ev_check ──────────│
     │             │                  │    UpdateRingWatcher  │
     │             │                  │    （若无挂起 op,      │
     │             │                  │      ev_io_stop）     │
```

---

### 4.12 设计原理

#### 4.12.1 为什么选择三路驱动（ev_prepare + ev_io + ev_check）

三路驱动是让 ASIO io_context 嵌入 libev 事件循环的最小且充分的接入方式：

| 驱动 | 插入点 | 不可缺少的原因 |
|------|--------|--------------|
| `ev_prepare` | epoll_wait **前** | ASIO 通过 `submit_sqes_op` 在 scheduler 里排队 SQE flush。若只靠 ring_fd 唤醒，SQE 永远不会提交到内核，IO 不会启动。ev_prepare 在每轮阻塞前强制 `poll()`，保证 SQE 被 flush。 |
| `ev_io(ring_fd)` | CQ **非空**时 | io_uring 的完成通知机制：CQ 有条目时 ring_fd 可读（电平触发）。没有这一路，completion handler 只能靠 ev_prepare/ev_check 碰巧执行，延迟不可控。 |
| `ev_check` | epoll_wait **后** | epoll_wait 窗口期间（ev_prepare 到 epoll_wait 返回）新到达的 CQE，ring_fd 的可读边沿已在本轮 epoll 之前发出，可能被漏掉。ev_check 兜底清空，保证完成事件不积压超过一轮。 |

三路互为补充，没有冗余：

```
ev_prepare ← 保证 SQE 不积压（发出请求）
ev_io(ring_fd) ← 保证 CQE 被及时响应（接收结果）
ev_check ← 保证窗口期 CQE 不漏收割（兜底清理）
```

#### 4.12.2 为什么 ring_fd 要按需启停（5.A）

ASIO 的 `io_context::poll()` 内部采用"poll 模式"调度（`wait_usec_==0`），其
`wake_one_thread_and_unlock()` 在每次 `post_immediate_completion` 时都会调用
`io_uring_service::interrupt()`，后者提交一个 `data==this` 的 NOP SQE。NOP 几乎
立即完成，使 CQ 非空，ring_fd 持续可读。

若始终监听 ring_fd，任何 `SubmitRead/Write` 都会触发 NOP → ring_fd 可读 →
epoll_wait 立即返回 → OnRingReady `poll()==0` → 下一轮 NOP …… 形成空转。

**按需启停的断环逻辑**：

```
有挂起 op → ring_fd 在 epoll 集合中 → 真实 IO 完成时被唤醒 ✓
无挂起 op → ring_fd 不在 epoll 集合中 → NOP CQE 不会唤醒 epoll_wait
           → ev_prepare/ev_check 仍会 poll()，NOP 在下一轮正常消费
           → epoll_wait 可以真正阻塞直到外部事件
```

代价：只有当有挂起 op 时才监听，最坏延迟 = 一轮 ev_run 迭代（亚毫秒），可接受。

#### 4.12.3 为什么移除内联 `m_ioCtx.poll()`（5.C）

旧实现在 `SubmitRead`/`SubmitWrite` 末尾各有一句 `m_ioCtx.poll()`，目的是
"立即将 SQE flush 到内核"。但这造成了二次放大：

```
SubmitRead 调用
  → async_read_some → start_op → post_submit_sqes_op
    → post_immediate_completion → interrupt() → NOP SQE#1
  → 内联 poll()
    → 调度 submit_sqes_op，内部再次 post_immediate_completion
    → 又一次 interrupt() → NOP SQE#2
```

每次 SubmitRead 额外产生一个 NOP，持续 S2S 读场景下 NOP 频率翻倍。

移除后，SQE 由下一个 ev_prepare 的 `poll()` 统一批量 flush：

```
N 次 SubmitRead（回调链中连续调用）
  → N 个 SQE 入 ASIO 内部队列
  → ev_prepare: io_context.poll() 一次 flush N 个 SQE
  → io_uring_submit() 批量一次 syscall
```

延迟损失 ≤ 一轮 ev_run 迭代，换来 syscall 次数与 NOP 频率的显著降低。

#### 4.12.4 为什么 diag_log 默认关（5.B）

`fflush()` 是同步磁盘 syscall。在忙循环场景下，OnRingReady 每秒触发数万次，
每次无条件 fflush 意味着数万次磁盘 IO，直接将 CPU 消耗推高至 31%，并产生 19GB 日志。

环境变量门控（`THUNDER_ASIO_URING_DIAG=1`）确保：
- 生产：无磁盘 fflush，无日志文件
- 调试：按需开启，获取完整 IO 诊断链路

#### 4.12.5 整体设计哲学：单线程零锁

```
┌──────────────────────────────────────────────────────────────┐
│  设计目标：最大化单核吞吐，最小化延迟抖动                      │
│                                                              │
│  关键选择                  结果                               │
│  ──────────────────────    ──────────────────────────────    │
│  io_context 只在主线程 poll() → 无锁，无 cache miss          │
│  libev epoll 统一管理 fd    → ring_fd 与业务 fd 同优先级      │
│  ev_prepare 批量 flush SQE  → 减少 io_uring_submit syscall  │
│  ring_fd 按需启停            → NOP CQE 不进入 epoll 热路径   │
│  completion handler 直调    → 无队列、无延迟                 │
│  weak_ptr 保护 FdState       → 关闭连接不需要全局锁           │
└──────────────────────────────────────────────────────────────┘
```

这套方案的核心约束是**所有 IO 操作必须在同一线程完成**。Thunder 的 Worker 进程
已经是多进程（Manager 派生多个 Worker），每个 Worker 独占一个 CPU 核，单线程模型
在这个架构下不是限制，而是恰好消除了多线程间的竞争开销。

### 4.10 与 UringIoBackend 的关键差异对比

| 特性 | UringIoBackend | AsioUringIoBackend |
|------|----------------|-------------------|
| **API 风格** | 直接使用 liburing | 封装在 Asio 中 |
| **写操作** | 同步 send() | async_write_some() 异步 |
| **缓冲区管理** | 手动 PendingOp | Asio 自动管理 |
| **SQPOLL** | 未使用 | 未使用（默认中断驱动模式） |
| **fixed buffers** | 未使用 | Asio 可配置 |
| **代码复杂度** | 较高（需自己管理 ring） | 较低（委托 Asio） |
| **成熟度** | 实验性 | 生产级 |

---

## 第五部分：性能 Benchmark 详解

### 5.1 测试环境与方法

**测试环境**：

- WSL2 (Windows Subsystem for Linux 2)
- Linux 内核：5.15+（支持 io_uring）
- 编译器：支持 C++20

**测试方法**：

- 使用 `wrk` 或类似工具进行 HTTP-like 请求压测
- 两个并发级别：c100（100 并发连接）、c500（500 并发连接）
- 测试三种 I/O 后端：`ev`、`uring`、`asio_uring`
- 记录 RPS（每秒请求数）和延迟（Avg/Stdev）

**测试说明**：WSL2 环境存在系统调用开销变异性，benchmark 数据可能波动。

### 5.2 小包测试（37B）

**场景特点**：最小化协议开销，测试 I/O 框架的调度效率。

**独立测试结果（asio_uring）**：

| 并发 | RPS | 平均延迟 | 标准差 |
|------|-----|---------|--------|
| c100 | 164,358 | 2.49ms | - |
| c500 | 164,086 | 1.75ms | 572μs |

**分析**：小包场景三者基本持平，因为：

1. 内核对小包的 send/recv 优化成熟
2. 瓶颈不在 I/O 系统调用，而在应用层协议处理
3. io_uring 的优势（批量提交）在小包场景不明显

### 5.3 大包测试（4KB）

**场景特点**：测试真实业务数据的传输性能。

**横向对比**：

| Backend | c100 RPS | c100 Avg | c500 RPS | c500 Avg |
|---------|----------|----------|----------|----------|
| **ev** | 73,137 | 1.51ms | 60,106 | 32.0ms |
| **uring** | 63,736 | 1.77ms | 49,152 | 11.3ms |
| **asio_uring** | 68,677 | 0.99ms | 68,679 | 17.2ms |

**数据解读**：

```
c100 延迟对比（越低越好）：
     ┌──────────────────────────────────────────┐
ev   │████████████████████████████████│ 1.51ms  │
uring│█████████████████████████████████│ 1.77ms  │  +17%
asio │███████████                      │ 0.99ms  │  -34%
     └──────────────────────────────────────────┘

c500 延迟对比（高并发场景）：
     ┌──────────────────────────────────────────┐
ev   │████████████████████████████████████████│ 32.0ms  │
uring│██████████████                            │ 11.3ms  │
asio │███████████████████                       │ 17.2ms  │
     └──────────────────────────────────────────┘
```

**关键发现**：

1. **asio_uring 在 c100 场景表现最佳**：0.99ms 延迟，比 ev 低 34%，比 uring 低 44%
2. **uring 在 c500 高并发场景翻身**：11.3ms vs ev 的 32.0ms（注意 ev 的 c500 延迟异常高）
3. **Asio 的集成更成熟**：uring（手写 liburing）表现不如 asio_uring，说明 Asio 的 io_uring 后端有更多优化

### 5.4 超包测试（64KB）

**场景特点**：大文件传输，测试内核缓冲区和网络栈的性能。

**横向对比**：

| Backend | c100 RPS | c100 Avg | c500 RPS | c500 Avg |
|---------|----------|----------|----------|----------|
| **ev** | 6,207 | 16.78ms | 6,370 | 76.42ms |
| **uring** | 5,688 | 17.65ms | 5,135 | 96.93ms |
| **asio_uring** | 6,675 | 2.32ms | 5,896 | 42.87ms |

**关键发现**：

```
64KB c100 延迟对比（asio_uring 碾压式领先）：
     ┌──────────────────────────────────────────┐
ev   │████████████████████████████████████████████████│ 16.78ms │
uring│█████████████████████████████████████████████████│ 17.65ms │
asio │█████                                      │  2.32ms  │  -86%
     └──────────────────────────────────────────┘
```

**结论**：

1. **asio_uring 的 64KB c100 延迟只有 ev/uring 的 14%**：这是一个巨大的性能优势
2. **大包场景下 io_uring 优势明显**：因为批量提交减少 syscall 次数、无需 epoll_ctl 注册/注销 fd、真正的异步写操作
3. **AsioUringIoBackend 使用默认中断驱动模式（非 SQPOLL）**：大包场景领先的真正原因是 io_uring 的批量提交/收割特性和 Asio 的 async_write_some 真正异步写，而非 SQPOLL

### 5.5 综合结论

**推荐使用 AsioUringIoBackend**，理由：

| 场景 | 推荐后端 | 原因 |
|------|---------|------|
| 小包（<1KB） | 三者皆可 | 差异不明显 |
| 大包（4KB） | asio_uring | c100 延迟最低 |
| 超包（64KB+） | asio_uring | 延迟领先 86% |
| 生产环境 | asio_uring | 代码成熟、API 友好 |

---

## 第六部分：架构演进与未来展望

### 6.1 当前 io_uring 集成的局限

#### 两个后端均未使用 SQPOLL 模式

当前 UringIoBackend 和 AsioUringIoBackend 均未启用 SQPOLL，每次提交仍需 `io_uring_enter()` syscall。

**证据**：
1. AsioUringIoBackend 需要通过 `FindIoUringRingFd()` 找到 ring_fd 并注册到 libev——如果用了 SQPOLL，就不需要 ring_fd 通知机制
2. Asio 的 io_uring 后端默认使用 `io_uring_queue_init(entries, &ring, 0)`（flags=0），不设置 `IORING_SETUP_SQPOLL`
3. SQPOLL 需要 `CAP_SYS_NICE` 权限或 root，生产环境通常不满足

**优化方向**：

```cpp
// 启用 SQPOLL 的配置
struct io_uring_params params = {};
params.flags = IORING_SETUP_SQPOLL;
params.sq_thread_idle = 2000;  // 空闲 2ms 后休眠
```

#### 未使用 Fixed Buffers

当前实现每次 I/O 都传递用户缓冲区地址，内核需要 pin 这些页面。

**优化方向**：

```cpp
// 预注册固定缓冲区
io_uring_register_buffers(&m_ring, nbuffers, buf_ptrs, sizes);

// 使用固定缓冲区
io_uring_prep_read_fixed(sqe, fd, buf, len, 0, buffer_idx);
```

#### 写操作差异

- UringIoBackend：同步写
- AsioUringIoBackend：异步写（通过 Asio）

### 6.2 与项目长期规划的关系

根据 `docs/architecture_design.md`，Thunder 的长期规划包括：

#### 零拷贝网络栈

io_uring 的 provided buffers 机制是实现零拷贝网络栈的关键：

```
传统路径（多次拷贝）：
  NIC DMA → 内核缓冲区 → 用户缓冲区 → 用户协议栈 → 用户应用缓冲区
                        ↑
                   一次拷贝

io_uring + Provided Buffers（零拷贝）：
  NIC DMA → 注册的用户缓冲区 → 直接传递给用户协议栈
            ↑
         零拷贝
```

#### 多线程事件循环

当前 Thunder 采用单进程单线程事件循环。io_uring 的 thread offset 特性支持多线程共享 SQ/CQ：

```
┌─────────────────────────────────────────────────────────┐
│  Process                                                  │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐                   │
│  │ Thread1 │  │ Thread2 │  │ Thread3 │  (各自独立的 ev)   │
│  └────┬────┘  └────┬────┘  └────┬────┘                   │
│       │            │            │                        │
│       └────────────┼────────────┘                        │
│                    │                                      │
│            ┌───────▼───────┐                             │
│            │  Shared SQ/CQ │  ← io_uring 支持多线程      │
│            └───────────────┘                             │
└─────────────────────────────────────────────────────────┘
```

### 6.3 未来优化方向

1. **完善 UringIoBackend 的异步写支持**
   - 解决 TLS/codec 状态机交互问题
   - 参考 AsioUringIoBackend 的实现

2. **启用 SQPOLL 模式**
   - 消除提交 syscall
   - 特别适合高频率小包场景

3. **实现 Fixed Buffers 机制**
   - 减少缓冲区 pin/unpin 开销
   - 为零拷贝网络栈做准备

4. **性能基准测试增强**
   - 在物理机（非 WSL2）上测试
   - 加入 CPU 利用率、内存带宽等指标
   - 测试更多包大小（1KB, 8KB, 128KB）

5. **探索 io_uring 的其他操作**
   - `IORING_OP_CONNECT`：异步 connect
   - `IORING_OP_ACCEPT`：异步 accept
   - `IORING_OP_TIMEOUT`：超时管理

---

## 附录：快速参考

### A. 配置方式

```json
{
  "io_backend": "asio_uring"  // 可选: "ev", "uring", "asio_uring"
}
```

### B. 编译要求

```bash
# UringIoBackend
#ifdef THUNDER_IO_URING
  需要 liburing-dev

# AsioUringIoBackend  
#ifdef THUNDER_IO_ASIO_URING
  需要 Boost.Asio + io_uring 支持
```

### C. 关键源码文件

| 文件 | 职责 |
|------|------|
| `IoBackend.hpp` | 接口定义 |
| `UringIoBackend.cpp` | 手写 liburing 实现 |
| `AsioUringIoBackend.cpp` | Asio io_uring 实现 |
| `EvIoBackend.cpp` | 基线实现 |

---

*文档版本：v1.1*  
*最后更新：2026-05-15*（新增 4.10～4.12 节：fix #2 前后对照、当前并发模型全景图、设计原理）  
*项目仓库：https://github.com/chenjiayi0603/thunder*
