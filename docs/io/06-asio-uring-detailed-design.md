# AsioUringIoBackend 详细设计

> 代码: `code/Net/src/labor/io/AsioUringIoBackend.{hpp,cpp}`
> 前提: 理解 epoll 基础,知道 I/O 多路复用概念

---

## 1. 背景: 为什么需要 io_uring

### 1.1 epoll 的局限

epoll 是"就绪通知"模型:内核告诉你 fd 可读了,你再去 `read()`。每次 `read()` 都是一次系统调用。

```
epoll 模型:
  epoll_wait() → "fd 42 可读了"
  read(fd42, buf) → 内核拷贝数据到 buf
  处理数据
  epoll_wait() → "fd 43 可读了"
  read(fd43, buf) → ...
```

高并发时,大量 fd 同时就绪 → 大量 `read()`/`write()` 系统调用 → CPU 浪费在内核态切换上。

### 1.2 io_uring 的改进

io_uring 是"完成通知"模型:你把 I/O 请求提交进队列,内核处理完通知你。**一次系统调用可以提交几十个请求,收割几十个结果**。

```
io_uring 模型:
  构造 SQE[0]: read(fd42, bufA)
  构造 SQE[1]: read(fd43, bufB)
  构造 SQE[2]: write(fd44, bufC)
  io_uring_enter() → 一次性提交 3 个请求
  ...内核并行处理...
  io_uring_enter() → 一次性收割 3 个完成的 CQE
```

**核心数据结构**:两个共享内存环形队列

```
         用户态                    内核态
    ┌──────────────┐         ┌──────────────┐
    │  SQ (提交队列) │ ──写入──► │  内核处理    │
    │              │         │              │
    │  CQ (完成队列) │ ◄──写入── │              │
    └──────────────┘         └──────────────┘
```

- **SQ**(Submission Queue): 用户态写,内核读。放 SQE(要做什么 I/O)
- **CQ**(Completion Queue): 内核写,用户态读。放 CQE(I/O 做完了,结果是什么)

两个队列都在用户态和内核态共享的内存中,**无需系统调用即可读写**(只在必要时 `io_uring_enter` 通知内核)。

### 1.3 和 epoll 对比

| | epoll | io_uring |
|---|-------|----------|
| 通知模型 | 就绪通知(告诉你"可以读了") | 完成通知(告诉你"读完了,数据在这") |
| 每次 I/O 系统调用 | read/write 各 1 次 | 0 次(批量提交+收割) |
| 高并发吞吐 | syscall 次数限制 | SQ/CQ 深度限制 |
| 零拷贝 | 需要 splice/sendfile | send_zc 原生支持 |
| 文件 I/O | 不支持(epoll 只管 fd 就绪) | 支持(统一接口) |
| 内核版本 | 2.6+ | 5.1+ |

---

## 2. Thunder 的集成: 三路驱动

io_uring 需要一个事件循环来驱动——什么时候提交 SQE,什么时候收割 CQE。Thunder 已经有 libev 事件循环,所以把 io_uring 嵌入 libev 的每次迭代中。

### 2.1 三个 watcher 的协作

libev 一次事件循环分几个阶段,Thunder 在三个阶段各插入一个 watcher:

```
libev 事件循环一次迭代:
  │
  ├─ ev_prepare ─────────────────────────────── [第①路: 投递]
  │  调用时机: epoll_wait() 之前
  │  做的事:   将攒下的读写请求批量打包成 SQE,一次性写入 SQ
  │            同时收割上一轮已完成但还没收的 CQE
  │
  ├─ epoll_wait ─────────────────────────────── (libev 阻塞等待)
  │  ring_fd 就绪 → io_uring 有新 CQE 到达
  │  fd 就绪 → accept/connect 事件
  │  timer 超时 → Step 超时、心跳
  │
  ├─ ev_io(ring_fd) ─────────────────────────── [第②路: 接货]
  │  调用时机: ring_fd 可读时
  │  做的事:   批量收割 CQE,触发 completion callback
  │            callback 中 buf->AdvanceIndex → 通知上层 "读/写完成"
  │            按需启停: 无挂起 I/O 时 stop ring_fd 监听
  │
  ├─ ev_check ───────────────────────────────── [第③路: 补刀]
  │  调用时机: epoll_wait() 之后
  │  做的事:   收割①②之间的 race window 可能到达的 CQE
  │            更新 ring_fd 监听状态
  │            每秒输出诊断统计(THUNDER_ASIO_URING_DIAG=1)
  │
  └─ invoke_pending ─────────────────────────── 所有排队的回调统一执行
```

### 2.2 为什么是三个,不是两个

如果只有①②(没有 ev_check): ev_prepare 提交 SQE 后立即检查结果,但那时 SQE 刚写入 SQ ring buffer,内核还没处理——CQ 里大概率是空的。所以把 CQE 收割拆成两个时机:ev_prepare 收割上一轮的,ev_check 做兜底补收。

如果只有①③(没有 ev_io): 只能靠定时 poll(),无法及时响应完成事件。ev_io(ring_fd) 让 epoll 在 CQE 到达时立刻唤醒 libev,**延迟最低**。

**三路缺一不可**:投递(攒批提交)、接货(及时收割)、补刀(防遗漏)。

---

## 3. SQE 和 CQE 的完整生命周期

以一个读请求为例,从调用到完成:

```
Step 1: Worker 发起读
  SubmitRead(fd=42, buf) → 构造 ASIO async_read_some → 生成内部 SQE(不提交)

Step 2: 批量提交 (ev_prepare)
  poll() → 将攒下的所有 async_op 批量转化为 SQE → 写入 SQ ring buffer
         → io_uring_enter(IORING_ENTER_GETEVENTS) → 提交 SQ + 收割 CQ

Step 3: 内核处理
  内核从 SQ 取走 SQE → 执行 read(fd42, buf) → 完成后写一条 CQE 到 CQ
  同时通过 eventfd 通知 ring_fd 可读

Step 4: epoll 唤醒 (epoll_wait)
  ring_fd 可读 → libev 从 epoll_wait 返回

Step 5: 收割 CQE (ev_io)
  poll() → 收割 CQE → ASIO 触发 completion lambda
  lambda 中: buf->AdvanceWriteIndex(n) → m_callback(fd, seq, IoOp::Read, n)
  Worker::OnIoComplete → codec 状态机处理

Step 6: 补刀收割 (ev_check)
  再次 poll() → 收割 ④⑤ 之间可能到的 CQE(race window)
  UpdateRingWatcher() → 挂起 op 全清了 → stop ring_fd 监听
```

**关键设计:延迟提交**。SubmitRead 只构造请求,不提交。ev_prepare 攒一批后一次 `io_uring_enter` 全提交。高并发时几百个 fd 同时读写,一次 syscall 搞定。

---

## 4. 关键设计决策

### 4.1 FdState 生命周期: shared_ptr + weak_ptr

每个 fd 对应一个 `shared_ptr<FdState>`,存储在 `m_fds` map 中。completion lambda 持有 `weak_ptr<FdState>`:

```
m_fds[fd] = shared_ptr<FdState>  ──引用──►  FdState 对象

completion lambda = [weak = weak_ptr<FdState>](...) {
    if (auto sp = weak.lock()) {  // 尝试提升,失败说明已释放
        处理 CQE  // 对象还活着
    } else {
        return  // 对象已析构,丢弃这个 CQE
    }
}
```

**为什么不用裸指针**: `CancelFd(fd)` 只需 `m_fds.erase(fd)`——shared_ptr 引用计数 -1。如果还有在途 lambda,对象活着;如果全部 lambda 都执行完了,对象自动析构。不需要显式等待所有异步 op 完成,也不需要引用计数管理。

### 4.2 CancelFd 不用 sock.cancel()

ASIO 的 `sock.cancel()` 会向 SQ 提交 `IORING_OP_ASYNC_CANCEL`。问题是:

```
时间线:
  T1: SubmitRead(fd42, buf1)      → 注册 SQE_A
  T2: CancelFd(fd42)              → 提交取消 SQE
  T3: SubmitRead(fd42, buf2)      → 注册 SQE_B  ← 刚注册的读!
  T4: 内核处理取消 SQE           → 把 SQE_B 也取消了! ← 意外!
```

改为:**不提交取消 SQE**。而是 `release()` + `erase()` + 设置 `cancelled=true` 标志。completion lambda 检查标志——已取消就直接丢弃,不触发上层回调。

### 4.3 RingWatcher 按需启停: 消除空唤醒

ASIO 的 waitable reactor 通过 NOP-SQE 向内核注册 ring_fd 的可读性。即使没有任何用户 I/O,内核也可能因为内部管理 CQE 让 ring_fd 变为可读 → epoll 唤醒 → poll()=0(空转) → 无意义的 CPU 消耗。

解决:

```
UpdateRingWatcher():
  遍历 m_fds, 检查是否有 readPending||writePending
  有 → ev_io_start(ring_fd)  ← 有货要收
  无 → ev_io_stop(ring_fd)   ← 空闲, 不监听
```

**空转怎么发生的**: ASIO 内部会在没有用户 op 时注册一个 NOP-SQE,内核完成这个 NOP 后写 CQE 到 CQ → eventfd 触发 → ring_fd 可读 → epoll 唤醒 → poll() 收割到 NOP 的 CQE(没有实际用户数据) → CPU 空转。

**stop ring_fd 之后**: NOP 不注册 → epoll 不唤醒 → CPU 空闲 → 等下次 SubmitRead/SubmitWrite 时 UpdateRingWatcher 重新 start。

### 4.4 Fixed Buffers: 零拷贝地基

通过 `io_uring_register(IORING_REGISTER_BUFFERS)` 预注册一组内存页。后续 I/O 操作只需传 buffer index,内核跳过虚拟地址→物理地址转换。

```
配置: THUNDER_ASIO_URING_FIXEDBUF=1
池大小: slot_size=64KB × slot_count=256 = 16MB
超限回退: 请求超过 64KB → 自动走非注册路径(普通 buffer)
```

用于 `send_zc`(零拷贝发送):直接引用 fixed buffer 中的用户态内存,绕过内核 socket buffer。适合大响应(文件下载、静态资源)。

---

## 5. 编译与运行

```bash
# 编译启用
cmake -DENABLE_ASIO_URING=ON

# 配置
Hello.json: "io_backend": "asio_uring"

# 诊断
THUNDER_ASIO_URING_DIAG=1 → /tmp/asio_uring_diag.log
# 诊断内容: 每秒 poll 次数、空唤醒率、挂起 op 数、fixed buffer 使用率
```

## 6. 与 EvIoBackend(epoll)对比

| | EvIoBackend | AsioUringIoBackend |
|---|-----------|-------------------|
| 每次 I/O syscall | read/write 各 1 次 | 0(批量) |
| 高并发吞吐 | syscall 次数瓶颈 | SQ/CQ 深度瓶颈 |
| 零拷贝 | splice/sendfile | send_zc |
| 代码量 | ~300 行 | ~500 行 |
| 内核要求 | 2.6+ | 5.1+ |
| 适用 | 通用,兼容好 | 高吞吐,需新内核 |

---

## 7. 设计优势总结

### vs epoll (EvIoBackend)

| 维度 | epoll | io_uring | 优势 |
|------|-------|----------|------|
| syscall 次数 | N次(N=就绪fd数) | 1次(批量提交+收割) | io_uring 减少 ~N× syscall |
| 用户态↔内核态 | 数据拷贝(read/write) | 可选零拷贝(send_zc) | io_uring 省内存带宽 |
| 文件I/O | 不支持(epoll只管fd就绪) | 统一接口 | io_uring 可直接异步读文件 |
| 并发1000连接 | 1000×epoll_wait+read | 1×io_uring_enter | io_uring ~1000×效率 |
| 代码量 | ~300行 | ~500行 | epoll更简单 |
| 内核要求 | 2.6+ | 5.1+ | epoll更兼容 |

### 为什么选 io_uring 做高性能后端

Thunder 是**游戏网关**——高并发(数万连接)、低延迟(毫秒级)、大流量(GB/s)。epoll 在高并发时 syscall 开销成为瓶颈。io_uring 的批量提交+零拷贝天然适合这个场景。

**但是epoll 仍然保留**(EvIoBackend)——兼容性:老内核、简单场景、调试方便。用户通过配置选: `"io_backend": "asio_uring"` 或 `"io_backend": "ev"`。

### 为什么不用 DPDK

DPDK 是用户态网络栈,bind 独占网卡,需要改网络拓扑。io_uring 基于标准 socket API,业务代码零改动——改配置即可。DPDK 用于极致性能场景(>10M pps),io_uring 用于高性能场景(1~5M pps)。

### 为什么用 ASIO 而非手写 io_uring

手写 io_uring(NativeUringIoBackend)是 Thunder 自己的实现,好处是无外部依赖。ASIO 是 C++ 标准提案库,生态成熟,维护成本低。两套后端并存,用户自选。

ASIO 的优势:
- 对象生命周期管理(shared_ptr/weak_ptr)
- 跨平台抽象(posix::stream_descriptor)
- 无需手写 SQ/CQ 环形缓冲区操作

手写 io_uring 的优势:
- 零依赖,编译更快
- 完全可控,无 ASIO 内部调度开销
