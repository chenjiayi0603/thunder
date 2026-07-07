# IoBackend 四后端对比

> 代码: `code/Net/src/labor/{Ev,NativeUring,AsioUring,Dpdk}IoBackend.{cpp,hpp}`
> 接口: `code/Net/include/labor/IoBackend.hpp`

---


## 1. 性能实测 (wrk HTTP 全链路, 2026-06-06)

**测试**: wrk -t4 -c100 -d5s, POST Echo, Hello 服务

**指标说明**:
- **QPS**: 每秒完成的请求数 (越高越好)
- **p50 时延**: 50% 请求的响应时间 ≤ 此值 (中位数, 衡量典型延迟)
- **p99 时延**: 99% 请求的响应时间 ≤ 此值 (长尾, 衡量最慢的 1%)
- **吞吐量**: 每秒传输的数据量

| backend | 100 conn QPS | 500 conn QPS | p50 时延 | p99 时延 | 吞吐量 |
|---------|------------|------------|---------|---------|--------|
| **ev (epoll)** | **109,574** | **102,357** | 0.79 ms | 2.12 ms | 68.1 MB/s |
| asio_uring | 108,000 | 82,707 | 0.88 ms | 18.5 ms | 67.5 MB/s |
| native_uring | 89,791 | 80,001 | 0.60 ms | 5.7 ms | 55.8 MB/s |

> p50 时延 = 50% 请求的响应时间 ≤ 此值 (中位数)
> p99 时延 = 99% 请求的响应时间 ≤ 此值 (长尾)

### 为什么是这个排名

### 不同包大小对比

| body | ev QPS | ev p50 | asio_uring QPS | asio_uring p50 | native_uring QPS | native_uring p50 |
|------|--------|--------|---------------|---------------|-----------------|-----------------|
| 空(18B) | **109,574** | 0.79 ms | 108,000 | 0.88 ms | 89,791 | 0.60 ms |
| 1KB | 58,598 | 2.81 ms | **71,465** | 1.28 ms | 67,678 | 13.91 ms |
| 4KB | 23,083 | 4.48 ms | **39,326** | 0.93 ms | 23,763 | 1.20 ms |

**结论变了**:

- 空body: ev 第一 (109K), 但 asio_uring 几乎持平 (108K)
- **1KB body: asio_uring 反超** (71K vs ev 58K, 快 22%)
- **4KB body: asio_uring 大幅领先** (39K vs ev 23K, 快 70%)

**大包时 io_uring 赢**。原因: 包越大, epoll 的 read/write syscall 开销越大(数据拷贝), io_uring 的批量提交 + 零拷贝优势越明显。

### 排名分析（按开销从小到大）:

1. **ev**: 每次 I/O 就是 epoll_wait + read/write。没有额外的调度层,没有 ring buffer 管理,**内核里跑了几十年,极致优化**。

2. **asio_uring**: 每次 poll 要多走一层 ASIO 调度(遍历 FdState → 构造 SQE → 写 SQ ring → io_uring_enter)。100 连接时,这层调度**比那几十个 syscall 还贵**——io_uring 省了 syscall, 但加的调度代码更贵。p99 延迟 18.5ms 长尾来自 ring_fd 空唤醒。

3. **native_uring**: 手写的 ring buffer 管理,无 ASIO 的对象池和编译器优化。每次手动操作 SQ tail/CQ head 指针,比 ASIO 的优化版本更慢。

**io_uring 的真正场景**: 不是 100 连接, 是 **10000 连接**。那时 epoll 要 10000 次 read, io_uring 1 次 enter——批量优势才体现。当前规模**不该用 io_uring**。


## 2. 四后端实现对比

### EvIoBackend — epoll (默认, 最简)

**怎么工作**:

```
应用层: SubmitRead(fd, buf)
  → 注册 fd 到 epoll (EPOLLIN)
  → epoll_wait 返回 fd 可读
  → read(fd, buf) → 触发 callback
```

- 每次 I/O = 1 次 epoll_wait + 1 次 read/write = **2~3 次系统调用**
- 100 个连接并发 = 1 次 epoll_wait + 100 次 read = **101 次系统调用**
- 优点: 简单,内核 2.6+ 都支持,无外部依赖
- 缺点: syscall 次数随连接数线性增长
- 代码: `EvIoBackend.{hpp,cpp}` (72+313=385 行)

### NativeUringIoBackend — 手写 io_uring (零依赖)

**怎么工作**:

```
应用层: SubmitRead(fd, buf)
  → 构造 SQE(read, fd, buf) 写入 SQ ring buffer
  → ev_prepare: io_uring_submit() 一次性提交所有 SQE
  → 内核处理, 完成后写 CQE 到 CQ ring buffer
  → ev_io(ring_fd): io_uring_peek_cqe() 收割 CQE → 触发 callback
```

- 每次 I/O = **0 次额外系统调用**(bulk submit)
- 100 个连接并发 = 1 次 io_uring_enter = **1 次系统调用**
- 和 EvIo 的关键区别: 不需要 epoll_wait 查就绪, 不需要逐 fd read/write
- 优点: 零外部依赖,纯 C API,编译快,完全可控
- 缺点: 手写 SQ/CQ ring buffer 管理,维护成本高
- 代码: `NativeUringIoBackend.{hpp,cpp}` (110+424=534 行)

### AsioUringIoBackend — ASIO 封装 io_uring (三路驱动)

**怎么工作**:

```
应用层: SubmitRead(fd, buf)
  → ASIO async_read_some → 生成 internal SQE (不提交)
  → ev_prepare: io_context.poll() 批量提交所有 SQE + 收割上一轮 CQE
  → epoll_wait (ring_fd 就绪)
  → ev_io(ring_fd): poll() 收割刚完成的 CQE → completion lambda
  → ev_check: poll() 补收 race window 的 CQE + 诊断
```

- 和 NativeUring 的核心区别: **用 ASIO 管理 SQ/CQ**,不手写 ring buffer
- 三路驱动: ev_prepare(投递) + ev_io(接货) + ev_check(补刀) — 确保零遗漏
- FdState: shared_ptr 生命周期管理, CancelFd 只需 erase(自动析构)
- Fixed Buffers: 预注册 16MB 内存池, send_zc 零拷贝地基
- 优点: ASIO 生态(shared_ptr/weak_ptr), 三路驱动低延迟
- 缺点: ASIO 依赖(编译慢 745 行)
- 代码: `AsioUringIoBackend.{hpp,cpp}` (172+573=745 行)

### DPDK — 未测试

DPDK 需要**独占网卡 + DPDK 兼容 NIC**(如 Intel X520/X710), 且需要 `meson` 编译。
当前测试环境无 DPDK 硬件, 无法实测。**以下对比仅限已测的三个后端**(ev/asio_uring/native_uring)。


### 工作流程图

**ev (epoll)**:
```
SubmitRead  SubmitWrite    ...
    │           │
    └─────┬─────┘
          ▼
    ev_prepare ── 无操作 (epoll 不需要 prepare)
          │
          ▼
    epoll_wait ──── fd 就绪? ──YES──► read/write ──► callback
          │
          ▼
    ev_check ── 无操作
```

**native_uring**:
```
SubmitRead  SubmitWrite    ...
    │           │
    └─────┬─────┘
          ▼
    ev_prepare ── io_uring_submit() ── 批量提交所有 SQE 到 SQ
          │                            同时收割上一轮 CQE
          ▼
    epoll_wait ── ring_fd 可读? ──► io_uring_peek_cqe() ──► callback
          │
          ▼
    ev_check ── 补收割 race window CQE
```

**asio_uring (三路驱动)**:
```
SubmitRead  SubmitWrite
    │           │         (只构造 async_op, 不提交)
    └─────┬─────┘
          ▼
  ┌─ ev_prepare ── io_context.poll() ── ① 提交所有 SQE
  │                                       ② 收割上一轮 CQE → callback
  ├─ epoll_wait ── ring_fd 可读?
  │       │
  ├─ ev_io ── poll() ── 收割 CQE → callback
  │       │            └─ 无挂起 op? → stop ring_fd 监听
  │       │
  └─ ev_check ── poll() ── 补收割 race window CQE
                            └─ UpdateRingWatcher()
                               └─ 有挂起? start : stop
```

**关键差異**: ev 只有 epoll_wait 做实际工作。native_uring 在 ev_prepare 做批量提交。
asio_uring 用三个 watcher 覆盖全部时机——提交(ev_prepare)、即时收割(ev_io)、兜底(ev_check)。

### 核心差异对比表

| | EvIo | NativeUring | AsioUring |
|---|------|-------------|-----------|------|
| I/O 模型 | 就绪通知 | 完成通知 | 完成通知 | 轮询 |
| syscall/请求 | 2~3 | 0(批量) | 0(批量) | 0 |
| SQ/CQ 管理 | 无 | 手写 | ASIO |
| 零拷贝 | 无 | 支持 | send_zc | DMA |
| 外部依赖 | 无(libev) | 无(liburing) | ASIO |
| 代码量 | 385 行 | 534 行 | 745 行 | 898 行 |
| 内核要求 | 2.6+ | 5.1+ | 5.1+ | 不需要 |
| 适用场景 | 默认,通用 | 高性能无依赖 | 高性能+生态 | >10M pps |

---

## 3. asio_uring 64K 大包异常分析

> 实测数据 (2026-06-13, #70 修复后, HTTP, 1 Worker):
>
> | backend | 64K QPS | 64K 延迟 |
> |---------|--------:|--------:|
> | ev | 69k | 1.44ms |
> | native_uring | 47k | 1.42ms |
> | **asio_uring** | **48k ⚠** | **18.6ms ⚠** |
> | Nginx | 81k | 1.22ms |
>
> asio_uring 延迟是 ev 的 **13 倍**，与小包表现（持平/领先）形成鲜明反差。

### 根因：8KB 上限 × 串行锁 → 8 次异步往返

```cpp
// AsioUringIoBackend.cpp SubmitRead()
buf->EnsureWritableBytes(8192);        // 每次最多读 8KB
size_t cap = buf->WriteableBytes();    // cap = 8192
sp->sock.async_read_some(asio::buffer(dst, cap), callback);
// readPending = true，回调完成前不能提交下一次读
```

64K 包的实际路径：

```
[64KB 数据在内核 buffer]

第1次: SubmitRead → SQE入队 → 等CQE → callback(8KB) → readPending=false
第2次: SubmitRead → SQE入队 → 等CQE → callback(8KB) → readPending=false
...
第8次: SubmitRead → SQE入队 → 等CQE → callback(8KB) → 请求才完整

延迟 = 8 × 单次SQE→CQE往返 ≈ 18.6ms
```

每次往返 ≈ 18.6ms ÷ 8 = **2.3ms**，是 ring_fd 通知 + ev 重新调度的开销。

### ev 为什么没这个问题：readv 双 buffer 技巧

```cpp
// CBuffer::ReadFD — ev 路径 (Util/src/util/CBuffer.cpp)
char extrabuf[32768];          // 栈变量：函数返回即消失，不属于任何连接
struct iovec vec[2];           // 单线程事件循环下同一时刻只有 1 个 32KB 在栈上
vec[0] = { CBuffer可写区, writable };
vec[1] = { extrabuf, 32768 };

int n = readv(fd, vec, writable > sizeof(extrabuf) ? 1 : 2);
// 一次 syscall 最多读 writable+32KB，64K 通常 2 次 readv 读完
// per-connection 内存成本 = 0（extrabuf 不常驻，Nginx 同理）
```

关键：多次 readv 在**同一个 ev 回调**里用 `goto read_again` 连续完成：

```
ev 触发 → callback
  → readv(~40KB)        ← 同步 syscall，零等待
  → goto read_again
  → readv(~24KB)        ← 同步 syscall，零等待
  → 请求完整 → 响应
← 全程同一调用栈，无任何异步调度开销 →
```

### 两种模型的本质差异

```
ev 后端（同步 readv）            asio_uring 后端（异步分块）
─────────────────────────        ─────────────────────────────────
ev触发                           SubmitRead(8KB)
  │                                │
  ▼                                ▼ SQE 入队
readv(40KB) ←── 一次syscall      等 CQE...（ring_fd通知）
  │                                │
  ▼ goto read_again               callback → SubmitRead(8KB)
readv(24KB) ←── 一次syscall        │
  │                                ▼ SQE 入队
  ▼ 完成                          等 CQE...
响应                               ×8 次
                                  完成
```

| 维度 | ev | asio_uring |
|:---|:---|:---|
| 读完 64K 需几次 syscall | 2 次 readv | 8 次 async_read_some |
| 多次读在哪完成 | 同一回调，goto 循环 | 8 个独立异步回路 |
| 每次额外开销 | 几乎零 | ring_fd 通知 + ev 重新调度 |
| 栈溢出缓冲 | 32KB extrabuf（零分配） | 无，完全依赖预分配 cap |

### 为什么 asio_uring 不能照搬 ev 的栈 extrabuf 技巧

看似可以这样改：

```cpp
char extrabuf[32768];
iovec vec[2] = { {dst, cap}, {extrabuf, 32768} };
async_readv_some(vec, 2, callback);   // ← 不存在
```

但这在异步模型下**结构上不可行**：

```
ev — 同步 readv：
  readv(fd, vec)          ← 内核写入 extrabuf（栈上）
  readv 返回              ← 栈帧仍在，extrabuf 有效
  Write(extrabuf, ...)    ← 安全读取
  函数返回 → 栈帧销毁

asio_uring — 异步 SQE：
  提交 SQE（内核记下 buffer 地址）
  函数返回 ← 栈帧已销毁！
  ...内核异步执行...
  内核向地址写数据 ← 该地址已是野指针
  CQE 完成
```

SQE 提交到 CQE 完成之间函数早已返回，栈帧不复存在。io_uring 的 registered buffers
要求 buffer 地址在整个操作期间固定有效（内核可能 DMA 直写），只有堆内存能保证这一点。
**extrabuf 技巧是同步 I/O 专属，对异步 I/O 结构上不可移植。**

### 为什么不能简单改成 65536

```
buf->EnsureWritableBytes(65536);   // ← 看似能修，实则不可行
```

- 1M 连接 × 64KB = **64GB**，内存不可接受
- 100 连接 × 64KB = 6.4MB，测试环境能跑，掩盖了问题

## 4. 百万连接内存估算

### 为什么把 tcp buffer max 设成 32KB

Linux 默认 `tcp_rmem = 4096 87380 6291456`（min/default/max），max 是 6MB。
内核会根据流量自动扩张 buffer（auto-tuning），高峰时 1 条活跃连接可以占到 6MB×2=12MB。

限制 max 的依据是吞吐公式：

```
最大吞吐/连接 = buffer_size ÷ RTT

32KB ÷ 0.1ms (局域网)  = 2.5Gbps  ← 够用
32KB ÷ 1ms  (同机房)   = 256Mbps  ← 够用
32KB ÷ 50ms (公网 WAN)  = 5Mbps   ← 不够，WAN 场景需要 256KB+
```

局域网微服务 RTT < 1ms，32KB 完全满足吞吐需求，同时把内存上限压到可控范围。

```bash
sysctl -w net.ipv4.tcp_rmem="4096 16384 32768"
sysctl -w net.ipv4.tcp_wmem="4096 16384 32768"
```

---

### 内核层 per-connection

内核 TCP buffer 是**懒分配**的——建连时只分配结构体，数据真正流动时才分配 buffer 页。
空闲连接 buffer 实际占用接近 min（4KB），活跃连接会扩到 max（32KB）。

```
空闲连接：
  struct tcp_sock + file + socket + epitem  ~3KB
  recv buffer（有连接但无数据，min=4KB）   ~4KB
  send buffer（同上）                       ~4KB
  ─────────────────────────────────────────────
  合计                                     ~11KB

活跃满载连接（正在传输大包）：
  struct tcp_sock + file + socket + epitem  ~3KB
  recv buffer（扩到 max）                  32KB
  send buffer（扩到 max）                  32KB  ← 收发各一个，所以是 ×2
  ─────────────────────────────────────────────
  合计                                     ~67KB
```

注意：**活跃小包连接不会涨到 67KB**。小包数据到达后被应用层立刻读走，
内核 buffer 里没有积压，实际占用仍接近空闲的 ~11KB。

---

### 应用层 per-connection（tagConnectionAttr）

CBuffer 也是懒分配——构造时 `m_buffer=NULL`，第一次 `EnsureWritableBytes(8192)` 才 malloc 8KB。

```
空闲连接（建连但未收发数据）：
  tagConnectionAttr struct 各字段   ~350B
  4× shared_ptr + CBuffer 对象      ~200B
  CBuffer 内部 buffer（未触发）       0B
  ev_io + ev_timer watcher          ~112B
  mapFdAttr 红黑树节点开销           ~96B
  ─────────────────────────────────────────
  合计                              ~750B ≈ 1KB

活跃连接（收发过数据，CBuffer 已分配）：
  上述 struct 开销                   ~750B
  pRecvBuff 内部 buffer              ~8KB（EnsureWritableBytes(8192) 触发）
  pSendBuff 内部 buffer              ~8KB
  pWaitForSendBuff                   ~8KB
  pClientData                        ~8KB
  ─────────────────────────────────────────
  合计                              ~33KB

说明：小包场景 CBuffer 分配了 8KB 但实际数据只有 ~200B，
内存已分配但大部分空着——这是预分配的代价，无法避免。
```

---

### 1M 连接总内存

| 场景 | 内核 | 应用层 | **合计** |
|:---|---:|---:|---:|
| 全部空闲 | 1M × 11KB = **11GB** | 1M × 1KB = **1GB** | **~12GB** |
| 全部活跃（大包满载，理论上限） | 1M × 67KB = **67GB** | 1M × 33KB = **33GB** | **~100GB** |
| **典型 API 服务（99% 小包活跃）** | | | |
| └ 990K 小包（buffer 不涨到 32KB） | 990K × 11KB ≈ **11GB** | 990K × 10KB ≈ **10GB** | ~21GB |
| └ 10K 大包（少数） | 10K × 67KB ≈ **0.7GB** | 10K × 33KB ≈ **0.3GB** | ~1GB |
| └ **合计** | | | **~22GB** |

**各行推导：**

**全部空闲（~12GB）**
每条连接只有 tcp_sock 结构体 + 最小 buffer（min=4KB），内核不预先分配大 buffer：
```
内核：2.5KB(tcp_sock) + 0.4KB(file) + 0.1KB(epitem) + 4KB(recv min) + 4KB(send min) ≈ 11KB
应用：350B(struct) + 200B(shared_ptr) + 0(CBuffer未触发) + 112B(watcher) + 96B(map) ≈ 1KB
合计：12KB × 1M = 12GB
```

**全部活跃大包满载（~100GB，理论上限）**
收发 buffer 都扩到 max=32KB，CBuffer 也全部分配：
```
内核：3KB(struct) + 32KB(recv max) + 32KB(send max) = 67KB   ← 收发各一个所以 ×2
应用：750B(struct) + 4×8KB(CBuffer) ≈ 33KB
合计：100KB × 1M = 100GB
```
要达到这个上限需要 100 万条连接**同时**在传输 64K+ 大包，实际不可能。

**典型 API 服务：99% 小包活跃（~22GB）**
小包（~200B body）到达后被应用层立刻读走，内核 buffer 无积压不扩张。
CBuffer 被首次读写触发分配（8KB），但实际只写了 ~200B，其余是预留空间。
```
990K 小包活跃，内核：
  2.5KB(tcp_sock) + 0.4KB(file) + 0.1KB(epitem) + 4KB(recv) + 4KB(send) = 11KB
  ← recv/send 无积压，不触发扩张，与空闲态相同

990K 小包活跃，应用层：
  750B(struct) + 8KB(pRecvBuff) + ~1KB(其余 CBuffer，小包未触发扩张) = ~10KB
  ← 8KB 已分配但只用了 ~200B，其余空着

990K × (11KB + 10KB) = 990K × 21KB ≈ 21GB

10K 大包活跃，内核：
  2.5KB(tcp_sock) + 0.4KB(file) + 0.1KB(epitem) + 32KB(recv max) + 32KB(send max) = 67KB

10K 大包活跃，应用层：
  750B(struct) + 4×8KB(CBuffer 全部触发) ≈ 33KB

10K × (67KB + 33KB) = 10K × 100KB ≈ 1GB

合计：21GB + 1GB = ~22GB
```

> **实际运行会比 22GB 更小**：活跃连接请求之间有空闲间隔，内核在 buffer 排空后
> 会收缩已分配的页（`tcp_moderate_rcvbuf=1` 默认开启，auto-tuning 双向生效）；
> 67KB 是瞬时峰值，非持续占用。
>
> **运维参考值：~15-22GB**（局域网微服务，1M 连接，32KB buffer 上限）。

**调整内核参数：**

```bash
sysctl -w net.ipv4.tcp_rmem="4096 16384 32768"
sysctl -w net.ipv4.tcp_wmem="4096 16384 32768"
# 效果：1M 空闲连接内核从默认 ~40GB 压到 ~11GB
```

> 注：默认 max=6MB 时，1M 活跃连接理论内核需求高达 ~12TB（不现实但不可预期）。
> 公网/WAN 场景（RTT > 10ms）建议 max=256KB，否则吞吐受限。

---

### 结论：当前接受，大包选 ev

两段式读（header 解析后按 Content-Length 扩容）理论可行，但有实际问题：
- 每个大包请求需 malloc + memcpy + free（buffer 从 8KB 扩到 N 字节）
- 需要 HTTP 解析器与 IO 层协作，改动面大
- 高 QPS 下 allocator 压力不容忽视

asio_uring 的优势在**小包低延迟**（64B: 220μs vs ev 424μs），64K 大包本就是 ev 更擅长的场景。
**需要大包性能时选 ev 后端**，无需修改 asio_uring 读路径。当前 8KB 上限标注为已知限制。

