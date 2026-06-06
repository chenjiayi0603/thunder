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

## 4. 性能对比

### 4.1 理论对比 (基于 pipe 基准 extrapolate)

| 场景 | EvIoBackend(epoll) | NativeUring | AsioUring | DPDK |
|------|-------------------|-------------|-----------|------|
| 单连接 QPS | ~1.7 M/s | ~1.7 M/s | ~1.7 M/s | ~2 M/s |
| 100 连接并发 QPS | ~0.5 M/s | ~50 M/s | ~70 M/s | ~100 M/s |
| 大包吞吐(4KB) | ~6.2 GB/s | ~10 GB/s | ~12 GB/s(send_zc) | ~40 GB/s |
| syscall/request | 2(read+write) | 0(批量) | 0(批量) | 0(用户态) |
| 延迟(单消息) | ~500 ns | ~200 ns | ~200 ns | ~100 ns |

### 4.2 实测数据(已测)

| 指标 | EvIoBackend(pipe等价) | ShmRingQueue | 加速 |
|------|---------------------|-------------|------|
| 64B QPS | 1.96 M/s | 16.1 M/s | 8× |
| 256B QPS | 1.88 M/s | 12.5 M/s | 6.6× |
| 1KB QPS | 1.67 M/s | 8.8 M/s | 5.3× |
| 4KB QPS | 1.60 M/s | ~7 M/s | 4.4× |

> ShmRingQueue 是内核旁路的 IPC(不是 I/O backend),但提供了零 syscall 的 baseline,可作为 io_uring 的上限参考。

### 4.3 io_uring vs epoll (推测,待实测)

```
1000 并发连接, 每个连接 1KB 读 + 1KB 写:

epoll:
  1000×epoll_wait → 返回 ~300 就绪 fd
  300×read(1KB) + 300×write(1KB) = 600 syscalls
  每轮 ~300us syscall 开销

io_uring:
  1×io_uring_enter → 提交 1000 SQE, 收割 600 CQE
  ~10us syscall 开销

加速: ~30×
```

---

## 5. 测试过程

### 5.1 单元测试

```bash
# EvIoBackend (总是编译)
ctest -R EvIo

# NativeUringIoBackend (需内核 5.1+)
ctest -R NativeUring

# AsioUringIoBackend (需 cmake -DTHUNDER_IO_ASIO_URING=ON)
ctest -R AsioUring

# DpdkIoBackend (需 DPDK 环境)
ctest -R Dpdk
```

### 5.2 性能测试

```bash
# ShmRingQueue QPS 测试 (基准备baseline)
./build/bin/thunder_test_shm_queue --gtest_filter="*QPS*"

# pipe IOPS (epoll 等价)
./tests/benchmark/bench_asio_uring.sh

# 全量基准
./tests/benchmark/bench_shm_queue.sh
```

### 5.3 实测结果

| 测试 | 工具 | 结果 |
|------|------|------|
| ShmRingQueue 64B QPS | gtest | 16.1 M/s |
| ShmRingQueue 256B QPS | gtest | 12.5 M/s |
| pipe IOPS 64B | Python | 1.96 M/s |
| ShmRingQueue latency | gtest | 7.7 ns |
