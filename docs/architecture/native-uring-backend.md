# NativeUringIoBackend
> 源码: `code/Net/src/labor/io/NativeUringIoBackend.{hpp,cpp}` (539 行)
> 配置: `"io_backend": "native_uring"`, 直接调用 liburing, 不经 ASIO
---
## 0. io_uring 最小示例
io_uring 是 Linux 5.1+ 的异步 I/O 接口。
`io_uring_queue_init()` 在内核 `mmap` 两片共享内存：
| 结构 | 数量 | 内容 | 读写方向 |
|------|:---:|------|---------|
| SQ Ring | 1 片 | SQE 事件（做什么） | 用户写 → 内核读 |
| CQ Ring | 1 片 | CQE 事件（做完了） | 内核写 → 用户读 |
| Data Buffer | N 个 | 真实数据（CBuffer） | 用户分配。普通路径：内核 sk_buf ↔ memcpy ↔ CBuffer；send_zc：TCP 协议栈在 pin 住的 CBuffer 页上直接操作，省掉 sk_buf memcpy。PendingOp::buf (shared_ptr) 保活 |
SQ/CQ 是事件队列，不存数据本体。Data Buffer 是用户态独立内存，每个 I/O 操作一个，SQE 里只携带指针。三者共用一个 `struct io_uring` 句柄。
```
       mmap 共享内存 (2 片 ring)                用户态独立内存 (N 个 buffer)
  ┌──────────────────────────────┐    ┌──────────┐  ┌──────────────┐
  │  SQ Ring (1 片)              │    │ read_buf │  │ response_buf │
  │  [读fd=5,len=4096] ──指针──→ │    └──────────┘  └──────────────┘
  │  [写fd=5,len=150]  ──指针──→ │          ↑              ↑
  │                              │      SQE 里只存指针, 内核按指针直接读写
  │  CQ Ring (1 片)              │
  │  [读完了, 4096字节]          │
  │  [写完了, 150字节]           │
  └──────────────────────────────┘
                                │
  共 2 次拷贝: 1 次 CPU memcpy + 1 次 DMA
  SQ/CQ: mmap 事件队列, 存"做什么、做完了没", 不存数据
  Data Buffer: 用户态独立内存 (CBuffer)。普通路径内核 sk_buf ↔ memcpy ↔ CBuffer；
               send_zc 时TCP 在 pinned CBuffer 页上直接操作, 省掉 sk_buf memcpy (真正零拷贝)
               PendingOp::buf (shared_ptr) 保活
```
提交侧（写 SQE）：io_uring_get_sqe → prep_read/write → set_data → io_uring_submit。
每次 I/O 操作都是这 4 步，不管 read 还是 write——唯一的区别是 prep 函数的参数。
完成侧（收割 CQE）：eventfd 通知 → peek_cqe → 取 res + user_data → cqe_seen。同样 3 步，不管什么操作类型。
下面是一个用 eventfd + epoll 驱动的事件循环，模拟一次"收到 HTTP 请求 → 回包"的完整闭环：
```c
int evfd = eventfd(0, 0);
struct io_uring ring;                            // 一个句柄, 管理 SQ + CQ 两片 mmap 事件队列
io_uring_queue_init(256, &ring, 0);
//                       ↑
//              SQ 深度 = 256 (用户可同时排队的最大 I/O 数)
//              CQ 深度 = 256×2 (内核自动分配, 留余量防止溢出)
io_uring_register_eventfd(&ring, evfd);   // 内核每写 CQE 就 write(evfd, 1)
//                             ↑                                                                                                   
//        两片环, 同一个 &ring, 不同入口:                                                                                          
//        io_uring_get_sqe(&ring)        → 从 SQ 取空闲 SQE slot (用户写) 
//        io_uring_peek_cqe(&ring, &cqe) → 从 CQ 取完成的 CQE      (内核写)     
struct epoll_event ev = { .events = EPOLLIN, .data.fd = evfd };
epoll_ctl(epfd, EPOLL_CTL_ADD, evfd, &ev);
// 启动: 投一个 read, 等客户端发数据。
// get_sqe → prep → set_data → submit 四步完成一次异步 I/O 投递。
// 此时只投了 read——write 在步骤③收割到 read 的 CQE 后触发。
struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);   // 从 SQ 取空闲 slot (用户写)
io_uring_prep_recv(sqe, client_fd, read_buf, 4096, 0);
io_uring_sqe_set_data(sqe, read_ctx);                  // user_data, 内核原样带回 CQE
io_uring_submit(&ring);
while (1) {
    // ① epoll_wait: 等内核通知 "有 CQE 可收割"
    struct epoll_event events[64];
    int n = epoll_wait(epfd, events, 64, -1);
    for (int i = 0; i < n; i++) {
        if (events[i].data.fd != evfd) continue;
        uint64_t cnt;
        read(evfd, &cnt, sizeof(cnt));     // drain eventfd
        // ② 收割 CQE: 从 CQ 取内核写的完成结果 (与 get_sqe 写 SQ 方向相反, 同一个 &ring)
        struct io_uring_cqe *cqe;
        while (io_uring_peek_cqe(&ring, &cqe) == 0) {
            int    res = cqe->res;                        // 读/写的字节数 (<0 表示错误)
            void  *ctx = io_uring_cqe_get_data(cqe);      // SQE 里塞的 user_data
            int    fd  = get_fd_from_ctx(ctx);            // fd 也来自 ctx (投递时记录)
            io_uring_cqe_seen(&ring, cqe);
            // ③ 处理 CQE 结果 → 写新 SQE 闭环
            if (is_read_op(ctx)) {
                // Read 完成 → 解析请求 → 写回包 SQE (sqe 已在启动时声明)
                sqe = io_uring_get_sqe(&ring);
                io_uring_prep_send(sqe, fd, response_buf, response_len, 0);
                io_uring_sqe_set_data(sqe, write_ctx);
                io_uring_submit(&ring);
            } else {
                // Write 完成 → 写 keep-alive read SQE
                sqe = io_uring_get_sqe(&ring);
                io_uring_prep_recv(sqe, fd, read_buf, 4096, 0);
                io_uring_sqe_set_data(sqe, read_ctx);
                io_uring_submit(&ring);
            }
        }
    }
    // ④ 兜底: ③ 中新写的 SQE 如果在 drain 之后才被内核完成,
    //    内核又写了 eventfd, 但已经在本次 epoll_wait 返回之后——
    //    此时再 peek 一轮, 不积压到下一次循环
    struct io_uring_cqe *cqe;
    while (io_uring_peek_cqe(&ring, &cqe) == 0) {
        handle_cqe(cqe);  // 同上逻辑
        io_uring_cqe_seen(&ring, cqe);
    }
}
```
`submit_read` / `submit_write` 做的事情就是第 1 章的 SubmitRead / SubmitWrite：取 SQE → prep → set_data → io_uring_submit。整个循环的核心就是这四个阶段——① 等 CQE → ② 收割 → ③ 处理 + 写新 SQE → ④ 兜底收割——这也正是 §3 数据流闭环的基础。
## 1. 设计定位
只用两个 libev watcher（`ev_io` + `ev_check`），每次 `SubmitRead/SubmitWrite` 末尾立即 `io_uring_submit`，不积压 SQE。
与 AsioUringIoBackend 的核心差异：**立即提交 vs 批量提交**。立即提交省掉了 `ev_prepare` watcher 和 SQE 积压逻辑，代价是每次提交多一次 syscall。作为 io_uring 的最小可行实现，验证两段式模型如何嵌入 libev 主循环。
## 2. 核心数据结构
```cpp
// 每 fd 状态
struct FdState {
    uint32_t seq;
    int      readPending, writePending;
    bool     cancelled;
};
// 每次 I/O 上下文 — 生命周期由 CQE 驱动
struct PendingOp {
    int                        fd;
    uint32_t                   seq;
    IoOp                       op;
    std::shared_ptr<CBuffer>   buf;         // Data Buffer: 普通路径内核 memcpy ↔ CBuffer
                                             // send_zc: TCP 在 pinned CBuffer 页上操作, 省 sk_buf memcpy
                                             // shared_ptr 保活, 防止 CQE 到达前被释放
    // send_zc 零拷贝专用
    bool  isZc;
    char* zcBuf;
    int   zcBytes;
    bool  gotResult;                        // 是否已收到结果 CQE
};
```
`PendingOp*` 通过 `io_uring_sqe_set_data()` 写入 SQE，内核原样带回 CQE —— 相当于内核级回调闭包。`shared_ptr<CBuffer>` 保证 send_zc 场景下 TCP 协议栈 pin CBuffer 页期间 buffer 不被释放。
## 3. 数据流闭环
```
libev 主循环 (ev_run)
  ①  epoll_wait ─ 阻塞等 eventfd 可读
      │           (内核每写完 CQE 就 write(eventfd))
      │
  ②  ev_check ── OnCheck() → ReapCqes()         ← 窗口期 CQE 兜底
      │
  ③  invoke_pending ─ ev_io(eventfd) → OnEvfd()
      │
      drain eventfd → ReapCqes()                 ← 收割 CQE (共享内存, 0 syscall)
        │
        m_callback(fd, seq, op, result)          ← 通知 Worker
        │
        v
      Worker::OnIoComplete()
        │
        ├─ Read 完成:  Decode → Dispose → SubmitWrite(回包)
        └─ Write 完成: 写完 → SubmitRead(keep-alive)
                                        │
        ┌───────────────────────────────┘
        │
        v  ④ 写 SQE + io_uring_submit   ← 立即提交, 不积压
        │
        v  更新 SQ tail 指针, 通知内核有新 SQE 可消费
        │
        └────→ 回到 ①
  ⑤  ev_check ── OnCheck() → ReapCqes()         ← 再次兜底
```
| 阶段 | 操作 | syscall |
|------|------|:---:|
| ① | 等内核通知 "CQE 就绪" | epoll_wait |
| ②③ | 收割 CQE → 回调 Worker → 业务处理 | 无 |
| ④ | 写 SQE → io_uring_submit | io_uring_enter |
| ⑤ | 窗口期 CQE 兜底收割 | 无 |
**关键设计点：**
- **eventfd 通知**：内核每写完一批 CQE 就 write(eventfd)，epoll_wait 精确唤醒。比 ring_fd 跨内核版本一致性更好
- **ev_check 兜底**：OnEvfd 执行完到下一轮 epoll_wait 之间新到的 CQE，由 ev_check 收割，不积压
- **CancelFd 延迟回收**：Cancel 只标记不释放。等 CQE 到达检测 fd 已不在 m_fds，跳过回调仅 delete——防止TCP pin 页期间 UAF
---
## 4. send_zc 零拷贝写

通过 `IORING_OP_SEND_ZC` 让 NIC 直接从用户态 CBuffer DMA 读取，省掉 CBuffer→sk_buf 的 CPU memcpy。
一次提交产生双 CQE（结果 + NOTIF），默认关闭，`THUNDER_URING_ZC=1` 开启，`THUNDER_URING_ZC_DIRECT=1` 启用真零拷贝（跳过 bounce buffer）。

### 4.1 实测数据

> 环境: i9-12900H, Ubuntu 26.04, Linux 7.0, 1GbE, wrk -t4 -c100, 10s
> ZC=DIRECT: 阈值=0 强制所有尺寸走 send_zc

```
大小      ev (RPS/P50)        ZC=OFF (RPS/P50)      ZC=DIRECT (RPS/P50)      ZC vs OFF
────────────────────────────────────────────────────────────────────────────────────────
16B       190,976 / 366μs     191,341 / 387μs       185,788 / 368μs         −2.9%
64B       186,251 / 395μs     193,174 / 382μs       184,701 / 430μs         −4.4%
256B      181,980 / 383μs     186,106 / 417μs       176,932 / 406μs         −4.9%
1K        168,558 / 426μs     166,202 / 517μs       157,892 / 543μs         −5.0%
4K        140,049 / 511μs     139,560 / 593μs       130,992 / 628μs         −6.1%
16K       76,754  / 1.07ms    75,595  / 1.11ms      76,069  / 920μs         +0.6%
64K       24,774  / 3.12ms    24,270  / 3.61ms      24,605  / 3.11ms        +1.4%
────────────────────────────────────────────────────────────────────────────────────────
```

- **小包 (≤4K): ZC=DIRECT 净亏 3-6%。** GUP 固定开销 (~0.5-1μs) > memcpy 节省 (16B 仅 ~0.05μs)
- **大包 (16K-64K): ZC=DIRECT 微弱优势 (<2%)。** 省掉 4μs memcpy 但仍在 1GbE NIC 瓶颈压制下

**64KB 3 轮平均验证（含 bounce 对照）:**

```
配置                                         Avg RPS    Avg P50
─────────────────────────────────────────────────────────────────
ev (::send, 基线)                           25,183     3.37ms
native_uring ZC=OFF (io_uring_prep_send)    24,605     2.96ms
native_uring ZC=BOUNCE (send_zc + bounce)   24,694     3.14ms
native_uring ZC=DIRECT (send_zc, 真零拷贝)  24,659     2.87ms
─────────────────────────────────────────────────────────────────
最大差异                                     2.3%       —
```

四种配置差异 < 3%，bounce 模式因多一次 memcpy 到 malloc 缓冲反而比 ZC=OFF 更差。

### 4.2 为什么收益微乎其微

当前 `send_zc` **没有调用 `io_uring_register_buffers()`**，每次 IO 都要走完整的 GUP 路径：

```
普通 send                                send_zc (当前实现)                send_zc + register_buffers (缺失)
─────────────────────────────────────    ─────────────────────────────    ─────────────────────────────────
                                         io_uring_prep_send_zc()          Init: io_uring_register_buffers()
io_uring_prep_send()                           │                               └─ get_user_pages() ×全页
      │                                  pin_user_pages_fast() ←每send     存入内核固定缓冲表
      │                                    ├─ mmap_lock                     (仅一次, ~1μs × N页)
memcpy 用户buf→sk_buf (~0.05-4μs)           ├─ PGD→PUD→PMD→PTE 遍历
      │                                    ├─ get_page() 原子加引用         每次 send_zc:
      │                                    └─ mmap_unlock                    lookup_fixed_buffer()
io_uring_submit()                          (~0.5-1μs 每send)                 └─ O(1) 哈希 → page* 已就绪
      │                                         │                           (无页表遍历, 无原子操作)
      │                                    DMA 用户页→NIC
内核 DMA sk_buf→NIC                             │
      │                                    NOTIF CQE: put_page() 原子减引用
CQE 收割                                   双CQE处理 (~0.5μs)
──────                                ─────────────────────────           ─────────────────────────────
开销: 仅 memcpy                        开销: GUP + 双CQE + memcpy(0)       开销: memcpy(0), GUP(0)
                                          ≈ 1-1.5μs 固定                  ≈ 0 固定
```

没有 registration，pin 开销从 "Init 时摊销一次" 变成 "每次 IO 都付"，省掉的 memcpy 收益被 GUP 税吃掉大半。**16K 处 P50 改善 17% (920μs vs 1110μs) 是真实信号——说明零拷贝一旦 GUP 开销被包大小摊薄，确实能降低延迟——但 1GbE NIC 吞吐上限导致 RPS 无法同步提升。**

### 4.3 有价值的前提

| 前提 | 当前 | 说明 |
|------|:---:|------|
| `io_uring_register_buffers()` | ❌ | 无则每 send 必付 GUP 税 |
| NIC ≥ 10Gbps | ❌ 1GbE | NIC 先瓶颈, CPU 省下的时间变 idle |
| 多机部署 | ❌ 单机 | 单机 wrk 夸大了 CPU 竞争 |

以上三项齐备时，64KB 流式发送可消除 30%+ CPU 的 memcpy 开销及关联的 L1/L2 cache 污染。**当前的最优策略：保持 ZC 默认关闭，用 16KB 阈值门控；优先补齐 `io_uring_register_buffers`。**
## 5. 立即提交 vs 批量提交
```
立即提交 (NativeUring):              批量提交 (AsioUring):
SubmitRead/Write                       SubmitRead/Write
  → GetSqe → 写 SQE                       → 写 SQE (积压)
  → io_uring_submit()  ← 立即             → 不提交
                                       ev_prepare
  (无 ev_prepare)                         → io_uring_enter
                                           N 条 SQE 一次提交
```
立即提交的代价是每次多一次 syscall，收益是代码极简（539 行 vs 745 行）。批量提交的理论 syscall 降低在运行时受 epoll 事件粒度影响，实际批量效率取决于 ev_prepare 触发频率。


## 完整数据流

### 汇总

| 操作 | CPU memcpy 次数 | DMA 次数 | 数据路径 |
|------|:---:|:---:|------|
| 普通 send | 1 | 1 | CBuffer →memcpy→ sk_buf →DMA→ NIC 板载内存 → 线缆 |
| 普通 recv | 1 | 1 | 线缆 → NIC 板载内存 →DMA→ sk_buf →memcpy→ CBuffer |
| send_zc | 0 | 1 | CBuffer(pin) →DMA→ NIC 板载内存 → 线缆 |

> DMA 发生在系统 RAM 和 NIC 板载内存之间, 由 NIC 硬件执行, CPU 只发指令不参与搬运。
> CPU memcpy 是 CPU 执行 rep movsb 指令逐字节复制, 阻塞 CPU 管线。

### 发送路径 (一次 send)

```
用户态                              内核态                          NIC 硬件

  CBuffer (用户内存)
      │
      │  第1次拷贝: CPU memcpy (CPU 逐字节复制)
      ↓
  sk_buf (内核 socket 缓冲区, 系统 RAM 中)
      │
      │  第2次拷贝: DMA (NIC DMA 引擎从系统 RAM 搬运到 NIC 板载内存)
      │            CPU 只发指令, 不参与数据搬运
      ↓
      └────────────→  NIC 板载内存
                                │
                                │  ③ NIC PHY 发送 (电平信号, 不是拷贝)
                                ↓
                            网络线缆
```
> 共 2 次拷贝: 1 次 CPU memcpy + 1 次 DMA

### 接收路径 (一次 recv)

```
网络线缆                              内核态                          用户态
   │
   │  ③ NIC PHY 接收 (电平信号, 不是拷贝)
   ↓
  NIC 板载内存
   │
   │  第1次拷贝: DMA (NIC DMA 引擎从 NIC 板载内存搬运到系统 RAM)
   │            CPU 只发指令, 不参与数据搬运
   ↓
  sk_buf (内核 socket 缓冲区, 系统 RAM 中)
   │
   │  第2次拷贝: CPU memcpy (CPU 逐字节复制)
   ↓
  CBuffer (用户内存)
```
> 共 2 次拷贝: 1 次 DMA + 1 次 CPU memcpy

### 零拷贝捷径 (send_zc)

```
用户态                              内核态                          NIC 硬件

  CBuffer (pin 住的用户页)
      │
      │  仅 1 次拷贝: DMA (NIC DMA 引擎直接从用户页搬运到 NIC 板载内存)
      │              TCP 协议栈在 pin 页上原地操作头信息, 不拷贝载荷数据
      │              省掉了第 1 次拷贝 (CBuffer→sk_buf 的 CPU memcpy)
      ↓
      └────────────→  NIC 板载内存  ──→  网络线缆
```
> 共 1 次拷贝: 1 次 DMA, 0 次 CPU memcpy
