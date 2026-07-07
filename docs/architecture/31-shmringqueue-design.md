# ShmRingQueue 详细设计

> **TL;DR** —— Manager↔Worker 的共享内存零拷贝 SPSC 消息队列。64B 往返 ~130ns、吞吐 7.9M msg/s（9.3× vs pipe）。
> 代码: `code/Net/include/labor/types/ShmRingQueue.hpp`
> 参见: [11 Manager-Worker IPC](11-manager-worker-ipc.md) · [perf/09 基准](../performance/09-shmringqueue-benchmark.md) · [quality/04 质量验证](../quality/04-shm-ring-queue.md)

---

## 1. 概述

ShmRingQueue 是一个**基于共享内存的环形无锁队列**，用于 Thunder 中 **Manager ↔ Worker 进程间通信（IPC）**。

```
Manager 进程                    Worker 进程
    │                              │
    ├── 写入 ShmRingQueue ────────► Worker 读取(命令/数据)
    │                              │
    ◄── Worker 写入 ShmRingQueue ──┤ Manager 读取(响应/通知)
    │                              │
    └── 共享内存(mmap MAP_SHARED | MAP_ANONYMOUS)
         父子进程 fork 后同一物理页 → 零拷贝
```

- **为什么不用 pipe/socket**：pipe 有内核缓冲区拷贝，大消息时性能差。共享内存直接读写，无拷贝。
- **为什么不用 POSIX mq**：消息队列有大小限制，且不支持 fork 后自动继承。

---

## 2. 原理

### 2.1 环形缓冲区

```
┌──── ControlBlock ────┐
│ magic:     0x53484D51│  ← "SHMQ" 魔数
│ slot_size: 4096      │  ← 每个槽大小(字节)
│ slot_count:128       │  ← 总槽数
│ write_pos: 5         │  ← 生产者写入位置
│ read_pos:  3         │  ← 消费者读取位置
└──────────────────────┘

┌──── Slots[128] ──────────────────────────────┐
│ Slot[0]: [cmd|seq|len|body...]               │
│ Slot[1]: [cmd|seq|len|body...]               │
│ Slot[2]: [空]                                 │  ← read_pos=3, 已消费
│ Slot[3]: [空]                                 │
│ Slot[4]: [cmd|seq|len|body...]               │  ← 未消费
│ Slot[5]: [写入中...]                          │  ← write_pos=5
│ ...                                          │
│ Slot[127]: [空]                               │
└──────────────────────────────────────────────┘
```

**单生产者单消费者（SPSC）**：Manager 只写不读一个方向，Worker 只读不写同一个方向；反过来另一个队列。双向通信需要两个 ShmRingQueue（一个 M→W，一个 W→M）。

### 2.2 内存布局：mmap + placement new

整个队列是一次 `mmap` 分配的连续内存：

```
mmap 区域: [ControlBlock][Slot[0]][Slot[1]]...[Slot[127]]
             ↑ sizeof(ShmRingQueue)  ↑ slot_size=4096 × 128
```

`ShmRingQueue` 对象通过 placement new 直接构造在 mmap 区域头部：

```cpp
void* mem = mmap(..., sizeof(ShmRingQueue) + slot_count * slot_size);
ShmRingQueue* q = new (mem) ShmRingQueue();  // 在 mmap 内存上构造
```

### 2.3 mmap + fork：零拷贝 IPC

```
Manager fork Worker:
  Manager: mmap(MAP_SHARED | MAP_ANONYMOUS, ...)
           → 创建 ShmRingQueue
           → fork()
  Worker:  fork 继承了父进程的 mmap 映射
           → 指针 q 指向同一个物理页
           → Manager 写入 → Worker 立即可见, 零拷贝
```

**限制**：只在父子进程间有效。如果想跨非父子进程共享，需要 `shm_open` + 文件 mmap，目前未实现。

### 2.4 无锁 SPSC

SPSC 模式天然无竞争：**一个写者 + 一个读者**，不需要互斥锁：

```
写者(Manager): 只修改 write_pos + 只写 Slot[write_pos] 的数据
读者(Worker):  只修改 read_pos  + 只读 Slot[read_pos] 的数据

读写的是**不同 slot**, 不会同时操作同一块内存
```

**内存序保证**：`write_pos` 用 `memory_order_release` 写入，`read_pos` 用 `memory_order_acquire` 读取。保证读者看到写者完成的数据，不会看到半写入的脏数据。

### 2.5 写入 / 读取流程

```
TryEnqueue(cmd, seq, body, body_len):
  ① body_len > slot_size? → 拒绝(消息太大)
  ② (write_pos + 1) % slot_count == read_pos? → 队列满,拒绝
  ③ 构造 Slot: 写入 cmd, seq, body_len, body 到 slot[write_pos]
  ④ 内存屏障: write_pos.store(write_pos+1, release)  ← 保证数据写入先于位置更新
  ⑤ 返回 true
```

**为什么 (write+1) % N == read 判满**：留一个空槽区分满和空。如果 write == read 则是空，如果 write+1 == read 则是满。

```
TryDequeue(cmd, seq, buf, out_len):
  ① read_pos == write_pos? → 队列空,返回 false
  ② 读取 Slot[read_pos]: 获取 cmd, seq, body_len, body
  ③ 内存屏障: read_pos.store(read_pos+1, acquire) ← 保证数据读取先于位置更新
  ④ 拷贝 body 到 buf, out_len = body_len
  ⑤ 返回 true
```

### 2.6 eventfd 通知

共享内存读写无系统调用 → 消费者不知道生产者何时写入了新数据。用 `eventfd` 做通知：

```
Manager 写入完 → write(eventfd, 1)  → 内核唤醒 epoll 上的 Worker
Worker epoll_wait 返回 → read(eventfd) → 消费通知 → TryDequeue 读队列
```

`EFD_NONBLOCK | EFD_SEMAPHORE`：非阻塞 + 信号量模式（每次 read 减 1，支持批量通知）。

### 2.7 Destroy 必须从控制块读尺寸

早期版本的 `Destroy(q, slot_count, slot_size)` 用**入参**计算 munmap 长度——如果 Create 和 Destroy 传的尺寸不同，munmap 长度不对 → 内存泄漏或崩溃。

修复后的 `Destroy(q)` 直接从 `q->ctrl` 读取：

```cpp
static void Destroy(ShmRingQueue* q) {
    size_t total = sizeof(ShmRingQueue)
                 + q->ctrl.slot_count.load(relaxed) * q->ctrl.slot_size.load(relaxed);
    munmap(q, total);
}
```

---

## 3. 配置

| 项 | 默认 | 说明 |
|----|------|------|
| `slot_count` | 128 | 总槽数；`(write+1)%count==read` 判满，实际可用 `count-1` |
| `slot_size` | 4096 B | 单槽固定大小，超过即拒绝 |
| eventfd flags | `EFD_NONBLOCK \| EFD_SEMAPHORE` | 非阻塞 + 信号量模式，支持批量通知 |

**最大消息限制**：每个 slot 固定大小（默认 4096 字节），超过的消息被拒绝（返回 false）。这是设计取舍——**固定大小换零拷贝 + 无锁**。如果需要大消息，上层负责分片（当前 Thunder 未实现分片）。

---

## 4. 示例

```cpp
// Manager 创建
ShmRingQueue* m2w = ShmRingQueue::Create();          // 默认: 128 slots × 4096B
ShmRingQueue* w2m = ShmRingQueue::Create(64, 8192);  // 自定义: 64 slots × 8KB

int efd_m2w = ShmRingQueue::CreateEventFd();         // 通知 fd

// fork Worker → Worker 继承 m2w, w2m 指针

// Manager 发消息给 Worker
m2w->TryEnqueue(cmd, seq, body, len);
ShmRingQueue::NotifyEventFd(efd_m2w);   // 唤醒 Worker

// Worker 收消息
uint32_t cmd, seq, out_len; char buf[4096];
m2w->TryDequeue(cmd, seq, buf, out_len);
```

---

## 5. 性能与权衡

实测（64B，详见 [perf/09](../performance/09-shmringqueue-benchmark.md)）：吞吐 **7.9M msg/s**、跨线程 RTT **~130ns**，相对 pipe **9.3×**。包越大加速比越小（8KB 时 6.8×），因为 syscall 节省占比下降、memcpy 主导。

| | pipe | socketpair | POSIX mq | ShmRingQueue |
|---|------|-----------|----------|-------------|
| 拷贝次数 | 2(用户↔内核) | 2 | 2 | 0(共享内存) |
| 最大消息 | 管道缓冲(~64KB) | socket缓冲 | mq_msgsize | slot_size(固定) |
| 锁 | 内核锁 | 内核锁 | 内核锁 | 无锁(SPSC) |
| fork继承 | 需要 dup | 需要 dup | 需要重新打开 | 自动继承 |
| 通知机制 | epoll | epoll | mq_notify | eventfd |

**取舍总结**：用「固定槽大小 + SPSC + 仅父子进程」换来「零拷贝 + 无锁 + fork 自动继承」。需要大消息、多消费者或跨机器时，改用 socket/pipe（见上表与[参见](#参见)）。

---

## 参见

- [11 Manager-Worker IPC](11-manager-worker-ipc.md) —— ShmRingQueue 在多进程交互中的位置
- [perf/09 ShmRingQueue 基准](../performance/09-shmringqueue-benchmark.md) —— vs pipe/socket/mq 完整数据
- [quality/04 共享内存 IPC 质量验证](../quality/04-shm-ring-queue.md) —— ASan + 单测覆盖
