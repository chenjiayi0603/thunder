# AsioUring 后端设计

> 源码: `code/Net/src/labor/io/AsioUringIoBackend.{hpp,cpp}` (745 行)
> 接口: `code/Net/include/labor/IoBackend.hpp`
> 配置: `"io_backend": "asio_uring"`

---

## 1. 两种 I/O 模型

### epoll: 就绪通知，提交和收割在同一个调用里

```
epoll_wait(所有fd)
  → "fd 42 可读了"
  → read(fd42, buf)     ← 提交 + 收割 一次完成, 1 个 syscall
  → 数据已经在 buf 里了
  → 结束
```

epoll 只做一件事：等 fd 就绪。真正干活的是 read/write，它们内部完成"提交请求→内核执行→返回结果"全流程。一个 syscall 搞定，零中间状态。

### io_uring: 完成通知，提交和收割是两次独立操作

```
写 SQE 到 SQ ring    ← 用户态, 零 syscall, 但内核不知道
   ↓
io_uring_enter       ← 通知内核"有新 SQE", 顺便收割 CQE
   ↓
内核: 取 SQE → 执行 read → 写 CQE 到 CQ → 通知 ring_fd
   ↓
ring_fd 可读          ← epoll_wait 被唤醒
   ↓
收割 CQE              ← 读结果: 多少字节, 成功还是失败
```

io_uring 把"提交请求"和"收割结果"拆成了两步，中间夹着内核异步处理。这是**模型决定的**，不是谁的实现选择——SQE 写进共享内存后内核不会自动感知，必须有人调 io_uring_enter 通知它。

SQE 和 CQE 本身是什么：

```
SQE (64字节, 提交队列条目):         CQE (16字节, 完成队列条目):
  opcode    = IORING_OP_READ           user_data = PendingOp*  ← 原样带回
  fd        = 42                       res       = 8192        ← 结果
  addr      = buf 指针                 flags     = ...         ← F_MORE/NOTIF等
  len       = 8192
  user_data = PendingOp*

SQE = 给内核的指令条 (读/写到哪/多少字节)
CQE = 内核的回执 (做完了, 结果是什么)
业务数据不经过 SQ/CQ, 直接走内核缓冲区 ↔ buf
```

### 两层缓存：Ring 管控制，Buffer 管数据

io_uring 把"指挥系统"和"货运系统"拆成了两条完全独立的通道：

```
                    io_uring 两条数据通道

控制面 (metadata)                    数据面 (payload)
─────────────────                    ─────────────────

SQ Ring (共享内存)                    用户态 CBuffer
┌──────────────────┐                 ┌──────────────────┐
│ SQE[0]: op=READ  │──── 指针 ─────→│ "GET / HTTP/1.1" │
│   fd=42          │                 │  用户真正的数据   │
│   addr=0x7f... ──┘                 └──────────────────┘
│   len=8192       │
│ SQE[1]: op=WRITE │──── 指针 ─────→ 另一个 CBuffer
│   fd=42          │
│   addr=0x7f... ──┘
└──────────────────┘

CQ Ring (共享内存)
┌──────────────────┐
│ CQE[0]: res=8192 │  ← 只告诉你 "读了 8192 字节"
│   user_data=ptr  │  ← 带回 PendingOp*, 用来找到对应的 CBuffer
└──────────────────┘
```

| | SQ/CQ Ring | CBuffer (用户 buffer) |
|---|---|---|
| **存什么** | 指令 + 结果 (元数据) | 真正的业务数据 |
| **条目大小** | SQE=64B, CQE=16B (固定) | 8KB~64KB+ (可变) |
| **内存来源** | `io_uring_setup` → `mmap` 内核分配 | `new`/`malloc` 用户分配 |
| **谁写** | SQ: 用户写, 内核读 / CQ: 内核写, 用户读 | 读: 内核 DMA→buffer / 写: buffer→内核 DMA |
| **共享方式** | 内核+用户态映射同一物理页 | 用户分配后, **指针**传给内核 (SQE.addr) |
| **syscall 替代** | Ring 让提交和收割零 syscall | 数据本身永远不经过 Ring |

**为什么要分开：**

SQ/CQ Ring 是固定大小的环形队列，每个条目只有几十字节，设计目标是极低延迟的元数据传递。如果把 4KB 的 HTTP 响应也塞进去，一个条目就占满几十个 slot，ring 瞬间打满，且读写同一块内存导致缓存行竞争。

所以 io_uring 只传指针：`SQE.addr = buf->GetRawWriteBuffer()`。内核拿到指针后，通过 DMA 直接把数据写到用户 buffer，不经过 SQ/CQ Ring。

```
一次 SubmitRead 的全路径:

  ① 写 SQE 到 SQ ring          ← 控制面, 64B write, 零 syscall
      SQE.addr = buf 指针       ← 只传指针, 数据不动

  ② io_uring_enter 通知内核    ← 唯一 syscall

  ③ 内核: 读 SQE → DMA 数据到 buf.addr  ← 数据面, 内核直写用户 buffer

  ④ 内核: 写 CQE 到 CQ ring    ← 控制面, 16B write

  ⑤ CQE 收割: res=8192
     → buf->AdvanceWriteIndex(8192)     ← 数据已经在 buffer 里了
```

**这就是 io_uring 批量提交能省掉 N-1 次 syscall 的根本原因：** 控制面走 Ring 共享内存（极快，零 syscall），数据面走 DMA（也快），两者互不阻塞。唯一需要 syscall 的就剩 `io_uring_enter` 通知内核"有新指令了"这一次。

### 为什么要 io_uring

Thunder 是游戏网关: 高并发(数万连接) + 低延迟(毫秒级) + 大流量(GB/s)。

epoll 每次 I/O = epoll_wait + read/write，N 个连接 = N×2 次 syscall，线性增长。
io_uring 一次 io_uring_enter 提交/收割所有请求，N 个连接 = 1 次 syscall。

批量优势随连接数放大。

---

## 2. 对接 libev: 两段式模型的必然代价

### 为什么需要 ev_prepare + ev_io(ring_fd)

io_uring 的两段式模型（提交 SQE → 收割 CQE）直接决定了最少需要两个钩子：

```
libev 每次迭代:

  ┌  ev_prepare  ──► io_uring_enter                       ┐
  │  提交积压的 SQE, 收割已到的 CQE                          │
  │  (必须要有, 不然 SQE 永远到不了内核)                      │ 这段是 io_uring
  │                                                        │ 的模型代价
  │  ev_io(ring_fd) ──► CQE 到了, poll() 收割               │
  │  (必须要有, 不然不知道 I/O 做完了)                        │
  └────────────────────────────────────────────────────────┘

  epoll_wait ── 等 fd / ring_fd / timer

  ev_check ── poll() 兜底收割 (可以砍, 漏了下一轮会补)

  invoke_pending
```

ev_prepare 和 ev_io(ring_fd) 是两段式模型决定的，砍不掉。ev_check 是可以砍的优化项。

每个阶段调哪个回调、底层做什么，对应关系如下：

```
libev 阶段              回调                    底层做了什么
─────────────────────────────────────────────────────────────────────
ev_prepare             OnPrepare()             ASIO poll()
  (epoll_wait 之前)                            → io_uring_enter
                                               提交所有积压 SQE 到内核
                                               顺便收割已到 CQE

backend_poll           无回调                    epoll_wait(fds, timeout)
  (阻塞等待)                                    ring_fd 可读 → 返回
                                               业务 fd 可读/可写 → 返回
                                               timer 到期 → 返回

timers_reify           无回调                    把到期 timer 加入 pending 队列
  (整理定时器)

ev_check               OnCheck()               ASIO poll()
  (epoll_wait 之后)                            补收 ②③ 之间新到的 CQE (兜底)
                                               UpdateRingWatcher()
                                               每秒诊断 (THUNDER_ASIO_URING_DIAG=1)

EV_INVOKE_PENDING      所有 pending 回调 统一执行:
  (执行排队的回调)       ev_check → OnCheck() 先执行
                       ev_io(ring_fd) → OnRingReady()
                         → poll() 收割 CQE
                         → lambda → m_callback → Worker::OnIoComplete()
                       ev_io(业务fd) → IoCallback() (legacy epoll 路径)
                       ev_timer → IoTimeoutCallback() / StepTimeoutCallback()
```

这个顺序是 libev 主循环 `ev_run()` 写死的，不是碰运气：

```c
// libev/ev.c ev_run() 主循环

do {
    // 1. ev_prepare — 必定最先执行
    queue_events(prepares, EV_PREPARE);
    EV_INVOKE_PENDING;                    // → OnPrepare()

    // 2. epoll_wait — 阻塞等事件
    backend_poll(waittime);               // → epoll_wait()

    // 3. 整理 timer/periodic
    timers_reify();

    // 4. ev_check — epoll_wait 之后
    queue_events(checks, EV_CHECK);

    // 5. 统一执行所有回调
    EV_INVOKE_PENDING;                    // → OnCheck(), OnRingReady(), 业务回调...
                                          //    ev_check 优先于 ev_io 执行
} while (activecnt);
```

### 三后端流程对比

```
EvIoBackend (epoll):        NativeUringIoBackend          AsioUringIoBackend
                             (liburing → io_uring):        (ASIO → io_uring):
                                │                             │
                                │ SubmitRead/Write             │ SubmitRead/Write
                                │ → 写 SQE 到 SQ ring           │ → 写 SQE 到 SQ ring
                                │ → io_uring_submit()           │   (不提交, 零 syscall)
                                │   (立即提交, 1个/次)            │
                                │                             │
                                │ (无 ev_prepare)               │ ev_prepare
                                │                             │ → io_uring_enter
                                │                             │   批量提交所有 SQE
                                │                             │
epoll_wait ── "fd 可读"          │                             │
    │                           │                             │
read(fd, buf)               epoll_wait                   epoll_wait
    │                      (等 eventfd 可读)             (等 ring_fd 可读)
结束                              │                             │
                                │                             │
                           ev_io(eventfd)               ev_io(ring_fd)
                           → drain eventfd              → poll() 收割 CQE
                           → ReapCqes() 收割 CQE        → ASIO lambda
                                │                        → 数据到手
                           ev_check                          │
                           → ReapCqes() 兜底                ev_check
                                │                        → poll() 兜底
                           m_callback                        │
                           → 数据到手                    invoke_pending
                                │                             │
                            结束                            结束
```

| | EvIoBackend | NativeUringIoBackend | AsioUringIoBackend |
|---|---|---|---|
| watcher | 1 (ev_io) | 2 (ev_io + ev_check) | 3 (prepare + io + check) |
| ev_prepare | — | 无 | OnPrepare → ASIO poll() → io_uring_enter |
| SQE 写 | — | SubmitRead/Write 中 | SubmitRead/Write 中 |
| SQE 提交 | — | 立即 `io_uring_submit()` 每次1个 | ev_prepare 批量 `poll()` N个一起 |
| CQE 通知 | epoll fd 就绪 | eventfd → ev_io(OnEvfd) | ring_fd → ev_io(OnRingReady) |
| CQE 收割 | read() 返回即得 | OnEvfd: ReapCqes() | OnRingReady: poll() |
| ev_check | — | OnCheck: ReapCqes() 兜底 | OnCheck: poll() 兜底 |
| syscall/请求 | ~2 | ~2 | ~1/N |

NativeUringIoBackend 每请求 syscall 数和 ev 一样（~2 次），却多了 SQE 构造、ring 指针操作、eventfd 路径的开销，实测比 ev 差 13%。io_uring 的核心优势（批量提交）完全未发挥。保留它只为 send_zc 零拷贝写和架构验证，性能场景应选 asio_uring。

### 核心优势：AsioUringIoBackend vs EvIoBackend

| 维度 | EvIoBackend (epoll) | AsioUringIoBackend | 优势 |
|------|-------------------|-------------------|------|
| syscall | 每请求 ~2 次，N 连接 = N×2 次 | N 个请求 1 次 io_uring_enter | **syscall 减少 ~N×** |
| 延迟 (64B) | 424μs | 220μs | **低 48%** |
| 大包 (4KB) | 23k QPS | 39k QPS | **快 70%** |
| 零拷贝 | 无 | send_zc + Fixed Buffers | **大响应省一次内核拷贝** |
| 业务代码 | 不变 | 不变 | **一行配置切换** |
| 内核要求 | 2.6+ | 5.1+ | ev 更兼容 |

核心就两点：
1. **批量提交**：ev_prepare 把 N 个 I/O 的 syscall 压成 1 次，连接越多优势越大
2. **零拷贝**：send_zc 让网卡直接 DMA 用户态 buffer，跳过内核 socket buffer

### 批量提交省了什么：100 连接为例

```
ev 路径 (100 个连接有数据可读):

  epoll_wait()         1 次 syscall  ← 等 fd 就绪
  read(fd1, buf)       1 次 syscall  ← 从内核拷数据到用户态
  read(fd2, buf)       1 次 syscall
  ...
  read(fd100, buf)     1 次 syscall
  ─────────────────────────────────
  合计: 101 次 syscall


asio_uring 路径 (同样的 100 个连接):

  SubmitRead × 100     0 次 syscall  ← 写 100 条 SQE 到共享内存 SQ ring
                                           (纯用户态内存写, 无 syscall)
  ev_prepare:
    io_uring_enter()   1 次 syscall  ← 100 条 SQE 一次提交给内核
  epoll_wait()         1 次 syscall  ← 等 ring_fd (内核干完活通知)
  ev_io(ring_fd):
    poll()             0 次 syscall  ← 用户态, 读共享内存 CQ ring
                       读出 100 条 CQE (每条 16B: 结果+字节数)
                       内核干活前已经写好, 不用再调 syscall
  ─────────────────────────────────
  合计: 2 次 syscall
```

省掉的是 **100 次 read() syscall**——原本每个连接要从内核把数据拷到用户态，现在内核批量干活，一次通知全部搞定。10000 连接时就是 10001 次 vs 2 次的差距。

---

## 3. 数据流

### 一次读请求

```
时序  发生了什么                                            syscall
──────────────────────────────────────────────────────────────────
 ①    Worker 调用 SubmitRead(fd, buf)
      写 SQE 到 SQ ring:                                      0 次
        opcode=READ, fd=42, addr=buf, len=8192

 ②    libev 下一轮, ev_prepare:
      io_uring_enter → 提交这轮所有 SQE + 收割遗留 CQE           1 次

 ③    内核: 取 SQE → read(42, buf, 8192) → 写 CQE
      标记 ring_fd 可读

 ④    epoll_wait 被 ring_fd 唤醒                              1 次

 ⑤    ev_io(ring_fd):
      poll() 收割 CQE → res=8192                               0 次
      ASIO lambda: buf->AdvanceWriteIndex(8192)
      → m_callback(fd, seq, Read, 8192)

 ⑥    Worker::OnIoComplete → HandleIoReadComplete
      Decode → Dispose → Encode → SubmitWrite (回包)            0 次
──────────────────────────────────────────────────────────────────
```

### 一次写请求

和读对称，区别：SQE opcode=WRITE，结果回调走 HandleIoWriteComplete。

```
 ①    SubmitWrite → SQE: opcode=WRITE, addr=数据区, len=字节数   0 次
 ②    ev_prepare → io_uring_enter 提交                         1 次
 ③    内核: write → CQE
 ④    epoll_wait 唤醒                                         1 次
 ⑤    ev_io → poll() 收割 → m_callback(Write)                 0 次
 ⑥    HandleIoWriteComplete → 没写完继续 SubmitWrite
──────────────────────────────────────────────────────────────────
```

---

## 4. 与 Thunder 的结合

Worker 通过 IoBackend 策略接口调用，不碰 socket API:

```
m_pIoBackend->SubmitRead(fd, buf, seq)   提交异步读
m_pIoBackend->SubmitWrite(fd, buf, seq)  提交异步写
m_pIoBackend->CancelFd(fd)               取消 fd 所有事件
...
```

回调: `m_callback → Worker::OnIoComplete()`

配置一行切换: `"io_backend": "asio_uring"` (或 `"ev"` `"native_uring"`)

---

## 5. 性能数据

环境: i9-12900H, 原生, 1 Worker, wrk -t4, `/hello/raw` Fast-Path

### HTTP (64B payload)

| 后端 | QPS | p50 |
|------|-----|-----|
| ev (epoll) | 232k | 424μs |
| asio_uring | 235k | 220μs |
| native_uring | 203k | 427μs |
| Nginx (参考) | 214k | 466μs |

**为什么 QPS 差不多，延迟差一倍？**

QPS 由 CPU 吞吐上限决定——64B 小包下，业务处理（Fast-Path JSON 拼响应、TCP 协议栈）消耗的 CPU 远超 I/O 本身，两者都在满负荷跑，每秒能处理的请求总数接近。

延迟差在 **syscall 的阻塞串行效应**。ev 的每次 syscall 都是同步点——请求必须停在那等内核返回才能走下一步：

```
一次 syscall 的开销:
  用户态 → 内核态 (保存寄存器, 切换栈)
  内核: 执行实际操作
  内核态 → 用户态 (恢复寄存器, 切换栈)
  ────────────────────────────
  不只是 CPU 开销 (~100-300ns), 更重要的是请求被卡住了
  ——切态期间请求完全停滞, 等内核回来才能继续

ev 每请求的 syscall:
  epoll_wait(共享) + read(fd) + write(fd)
  ≈ 4 次阻塞点 / 请求, 每个点都卡一下

asio_uring 每请求的 syscall:
  io_uring_enter(批量) + epoll_wait(共享)
  ≈ 2 次 / 一整轮, 请求本身不被卡 —— 内核异步干活,
  用户态继续处理其他请求, CQE 到了再回来收

结论: 不是 CPU 吃不消 syscall, 是 syscall 把请求流水线切成了一段一段,
     每段都要等内核返回才能继续。asio_uring 消除了这些同步点,
     请求不再被卡 → 延迟砍近一半。

直观对比:

```
ev (epoll):                  asio_uring:

epoll_wait (syscall)         写入 SQE (零 syscall)
    ↓                              ↓
read(fd, buf) (syscall)      io_uring_enter (1次, 批量)
    ↓                              ↓
处理 → 写响应 (syscall)       CQE 收割 (零 syscall)
    ↓                              ↓
总 syscall: ~4/请求          处理 → 写响应 (也走批量, 零 syscall)
                                  ↓
                            总 syscall: ~2/轮 (平摊到 N 个请求)
```

结论: 少 2~3 次 syscall/请求 → 每次省 ~几百 ns + 减少缓存污染
      → 延迟从 424μs 降到 220μs, 少了将近一半
      → 延迟从 424μs 降到 220μs, 少了将近一半

### HTTPS

| 后端 | 64B 延迟 | 4KB 延迟 |
|------|---------|---------|
| ev | 803μs | 1230μs |
| asio_uring | 402μs | 247μs |
| native_uring | 394μs | 218μs |

SSL 加密是 CPU 瓶颈, 吞吐持平。uring 系延迟 (~400μs) 显著优于 ev (803μs)。

### 结论

- asio_uring 延迟 220μs, 比 ev 424μs 低 48%, 比 Nginx 466μs 低 53%
- native_uring 每次只提交 1 个 SQE (`io_uring_submit()`), 批量优势未发挥, 比 ev 差 13%
- asio_uring 通过 ev_prepare 批量提交, N 个 I/O → 1 次 io_uring_enter

---

## 6. 后端对比

| | EvIoBackend | AsioUringIoBackend |
|---|---|---|
| I/O 模型 | epoll 就绪通知 | io_uring 完成通知 |
| 提交+收割 | read/write 一次完成 | 两次独立操作 |
| watcher | 1 个 (ev_io) | 2 个 (ev_prepare + ev_io) |
| syscall/请求 | ~2 | ~1/N (批量化) |
| 零拷贝 | 无 | send_zc |
| 内核要求 | 2.6+ | 5.1+ |

---

## 7. 编译运行

```bash
cmake -DENABLE_ASIO_URING=ON
# Hello.json: "io_backend": "asio_uring"
# 诊断: THUNDER_ASIO_URING_DIAG=1 → /tmp/asio_uring_diag.log
```
