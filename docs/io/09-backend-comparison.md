# IoBackend 四后端对比

> 代码: `code/Net/src/labor/{Ev,NativeUring,AsioUring,Dpdk}IoBackend.{cpp,hpp}`
> 接口: `code/Net/include/labor/IoBackend.hpp`

---

## 1. 统一接口

所有后端实现同一个接口,上层代码无感知切换:

```cpp
class IoBackend {
    virtual bool Init(ev_loop*, IoCompletionCallback, void*) = 0;
    virtual void Destroy() = 0;
    virtual int  CreateListenSocket(ip, port, reusePort, backlog) = 0;
    virtual int  Accept(listenFd, outPeerAddr) = 0;
    virtual bool SubmitRead(fd, shared_ptr<CBuffer>, seq) = 0;
    virtual bool SubmitWrite(fd, shared_ptr<CBuffer>, seq) = 0;
    virtual void CancelFd(fd) = 0;
    virtual void CloseFd(fd) = 0;
    virtual bool HasPending(fd) = 0;
};
```

配置切换: `"io_backend": "ev"` / `"asio_uring"` / `"native_uring"` / `"dpdk"`

---

## 2. 四后端对比

### 2.1 EvIoBackend (epoll)

| 维度 | 说明 |
|------|------|
| 原理 | libev 标准 epoll 封装 |
| 模型 | 就绪通知: epoll_wait → read/write |
| 线程 | 单线程, 和 libev 主循环同线程 |
| 批量 | 不支持, 每个 fd 独立 read/write |
| 零拷贝 | 不支持 |
| 代码量 | ~385 行 |
| 内核要求 | 2.6+ |
| 优点 | 最简单, 兼容性最好, 调试方便 |
| 缺点 | 高并发 syscall 开销大 |
| 适用 | **默认后端**, 所有场景 |

### 2.2 NativeUringIoBackend (手写 io_uring)

| 维度 | 说明 |
|------|------|
| 原理 | 直接调用 liburing API, 手写 SQ/CQ 管理 |
| 模型 | 完成通知: SQE 批量提交 → CQE 收割 |
| 线程 | 单线程, ev_io 监听 ring_fd |
| 批量 | 支持, ev_prepare 批量提交 SQE |
| 零拷贝 | 支持(需进一步实现) |
| 代码量 | ~534 行 |
| 内核要求 | 5.1+ |
| 优点 | 零外部依赖,完全可控,编译快 |
| 缺点 | 需手写 SQ/CQ 管理,维护成本高 |
| 适用 | 高性能 + 无 ASIO 依赖的场景 |

### 2.3 AsioUringIoBackend (ASIO + io_uring)

| 维度 | 说明 |
|------|------|
| 原理 | 基于 stand alone ASIO,三路驱动嵌入 libev |
| 模型 | 同 NativeUring, 但 SQ/CQ 由 ASIO 管理 |
| 线程 | 单线程, 三路 watcher(ev_prepare+ev_io+ev_check) |
| 批量 | 支持, ev_prepare poll() 批量提交 |
| 零拷贝 | send_zc + fixed buffers |
| 代码量 | ~745 行 |
| 内核要求 | 5.1+ |
| 优点 | ASIO 生态(生命周期管理/跨平台), 三路驱动低延迟 |
| 缺点 | ASIO 依赖(头文件多,编译慢) |
| 适用 | 高性能 + 愿意接受 ASIO 依赖 |

### 2.4 DpdkIoBackend (DPDK)

| 维度 | 说明 |
|------|------|
| 原理 | 用户态网络栈, 绕内核, 独占网卡 |
| 模型 | 轮询模式: 不停检查网卡 descriptor ring |
| 线程 | 多线程(每个 Worker 一个 lcore) |
| 批量 | 天然批量(descriptor ring) |
| 零拷贝 | 支持(用户态直接操作 DMA buffer) |
| 代码量 | ~898 行 |
| 硬件要求 | 独占网卡(需 DPDK 兼容 NIC) |
| 优点 | 极致性能(>10M pps), 零 syscall |
| 缺点 | 独占网卡, 改网络拓扑, 运维复杂 |
| 适用 | 极致性能(游戏网关 DDoS 防护, >1M pps) |

---

## 3. 设计差异根源

```
复杂度:  Ev < NativeUring < AsioUring < DPDK
性能:    Ev < NativeUring < AsioUring < DPDK
兼容性:  Ev > NativeUring ≈ AsioUring > DPDK
```

**为什么需要四个后端**: Thunder 面向不同场景——开发用 epoll(简单),生产用 io_uring(高性能),极致用 DPDK(>10M pps)。用户按需选择。

**NativeUring vs AsioUring**: 功能等价,依赖不同。NativeUring 零外部依赖,AsioUring 靠 ASIO 管理生命周期。两套保留是因为"零依赖"和"生态便利"各有用户。

---

## 4. 性能对比 — QPS + 时延 + 包大小分析

### 4.1 实测数据 (2026-06-06, Linux 7.0, 20 cores)

| 包大小 | pipe QPS | pipe MB/s | pipe 时延 | ShmRingQueue QPS | ShmRingQueue MB/s | 加速比 |
|--------|----------|-----------|----------|-----------------|-------------------|--------|
| 64 B   | 0.85 M/s | 52 MB/s   | 1175 ns  | **7.9 M/s**     | 484 MB/s          | **9.3×** |
| 128 B  | 0.83 M/s | 102 MB/s  | 1200 ns  | **7.5 M/s**     | 900 MB/s          | **9.0×** |
| 256 B  | 0.82 M/s | 201 MB/s  | 1214 ns  | **7.4 M/s**     | 1,795 MB/s        | **9.0×** |
| 512 B  | 0.79 M/s | 386 MB/s  | 1266 ns  | **7.1 M/s**     | 3,470 MB/s        | **9.0×** |
| 1 KB   | 0.71 M/s | 695 MB/s  | 1405 ns  | **6.9 M/s**     | 6,782 MB/s        | **9.7×** |
| 2 KB   | 0.69 M/s | 1,356 MB/s| 1440 ns  | **6.0 M/s**     | 11,720 MB/s       | **8.6×** |
| 4 KB   | 0.65 M/s | 2,524 MB/s| 1548 ns  | **5.0 M/s**     | 19,530 MB/s       | **7.7×** |
| 8 KB   | 0.51 M/s | 4,001 MB/s| 1953 ns  | **3.5 M/s**     | 27,340 MB/s       | **6.8×** |

> pipe: Python 实测(50K rounds), ShmRingQueue: C++ gtest(500K rounds SPSC 跨线程)
> pipe 数据模拟 epoll 单连接场景(每轮一次 read+write syscall)

### 4.2 分析: pipe 瓶颈

```
pipe write/read 往返 = 2×syscall + 2×内核拷贝
syscall 开销: ~500ns × 2 = ~1000ns (固定)
内核拷贝: size / 带宽

64B:  1175ns = 1000ns(syscall) + 175ns(copy) → syscall 占 85%
8KB:  1953ns = 1000ns(syscall) + 953ns(copy) → syscall 占 51%

pipe QPS 随包增大下降: 0.85→0.51 M/s — 内核拷贝线性增长
```

### 4.3 分析: ShmRingQueue 瓶颈

```
零 syscall + 共享内存直接读写

64B:  QPS=7.9M,  BW=484 MB/s — 瓶颈: 原子操作+cache一致性
8KB:  QPS=3.5M,  BW=27.3 GB/s — 瓶颈: 内存带宽(DDR4 ~50GB/s, 已用55%)

QPS 随包增大下降: 7.9→3.5M — memcpy 时间主导
BW 随包增大上升: 484→27,340 MB/s — 大包效率高
```

### 4.4 加速比为何递减

```
64B:  syscall占85% → 跳过收益最大 → 9.3×
8KB:  syscall占51% → 收益下降 + ShmRingQueue memcpy占比上升 → 6.8×

趋势: 包越大, syscall节省占比越小, 加速比收敛
```

### 4.5 ShmRingQueue 时延

| 测试 | 时延 | 分析 |
|------|------|------|
| 单消息 RTT | **7.7 ns** | ~23 CPU cycles, L1 cache级 |
| SPSC 跨线程 | **~130 ns** | cache line bouncing |

### 4.6 场景适用建议

| 场景 | 包大小 | 推荐后端 | 预期 QPS |
|------|--------|---------|----------|
| 小消息 IPC(Manager↔Worker) | 64~256B | ShmRingQueue | 7~8 M/s |
| API网关短请求 | 256~1KB | io_uring | 理论 >5 M/s |
| 文件传输/静态资源 | 4~8KB | io_uring send_zc | ~12 GB/s |
| 极致小包(游戏协议) | 64~128B | DPDK | >10 M pps |

### 4.7 io_uring vs epoll (理论推测)

```
单连接: 和 pipe 同(1次 io_uring_enter vs 2次 read+write)
100 连接并发: 1次 io_uring_enter vs 100次 read+write → QPS 高 ~50×

ShmRingQueue 9× 证明了"零 syscall = 巨大收益"
io_uring 把多连接 I/O 打包到一次 enter → 并发越高, 收益越大
```

## 5. 测试过程

```bash
# ShmRingQueue QPS (4 包大小)
./build/bin/thunder_test_shm_queue --gtest_filter=*QPS*

# pipe IOPS (epoll syscall baseline)
python3 -c "import os,time; r,w=os.pipe(); ..."

# ShmRingQueue 时延
./build/bin/thunder_test_shm_queue --gtest_filter=*Latency*

# 全量回归
ctest -j1  # 304/304 ✅
```

### 4.8 Backend 对等实测 (2026-06-06)

尝试用 C++ ev_loop benchmark 对比 Ev vs AsioUring, 结果不稳定:

| Backend | 64B | 256B | 1KB | 问题 |
|---------|-----|------|-----|------|
| EvIoBackend | 0.045 M/s | 0.044 M/s | 0.047 M/s | ev_run(NOWAIT) 未驱动 epoll 完成 |
| AsioUringIoBackend | 0.28 M/s | 1.30 M/s | 1.25 M/s | ev_prepare 批量提交, 数据偏大 |

**不稳定原因**: 单线程 pipe write→read 串行, 无法模拟真实并发。ev_run(EVRUN_NOWAIT) 对不同后端行为不同(EvIo 需要 fd 就绪才触发, AsioUring 在 ev_prepare 中同步 poll)。

**正确方法**: wrk HTTP 压测(见 §4.9)或专用 C++ benchmark(每连接独立线程+ev_loop 驱动)。

### 4.9 实际建议: wrk HTTP 压测

```bash
# 对比两个 backend 的真实业务吞吐
# 1. 配置 Hello.json "io_backend": "ev"
# 2. 启动服务 → wrk -t4 -c100 -d30s http://127.0.0.1:27006/hello
# 3. 配置 Hello.json "io_backend": "asio_uring"
# 4. 重启 → wrk 同样参数 → 对比 QPS

# 这才是真实 backend 性能对比——走完整 HTTP 栈+codec+S2S
```

**当前结论**: ShmRingQueue vs pipe 的 9× 加速证明了"零 syscall=巨大收益"。Backend 对等对比需 wrk 全链路压测。
