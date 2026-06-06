# IO 模块性能基准测试

> 日期: 2026-06-06
> 环境: Linux 7.0.0-22-generic, 20 cores, gcc 15, -O2
> 脚本: `tests/benchmark/bench_shm_queue.sh`, `tests/benchmark/bench_asio_uring.sh`

---

## ShmRingQueue (共享内存无锁队列)

### 吞吐量 (SPSC 跨线程)

| 消息量 | 耗时 | 吞吐 |
|--------|------|------|
| 1M | 63~68ms | 14.7~15.9 M msg/s |
| 5M | 64~78ms | 12.8~15.6 M msg/s |
| 10M | 61~73ms | 13.7~16.4 M msg/s |

**平均: ~15 M msg/s** (SPSC 单生产者单消费者跨线程)

### 单消息延迟 (单线程 round-trip)

| 轮次 | 延迟 |
|------|------|
| 100K | 7.5 ns |
| 100K | 7.8 ns |
| 100K | 7.9 ns |

**平均: ~7.7 ns/round** (enqueue + dequeue 一次往返)

### 与 pipe 对比

| 指标 | ShmRingQueue | pipe(read/write) |
|------|-------------|-----------------|
| 数据路径 | 共享内存直接读写 | 用户→内核→用户(2次拷贝) |
| 吞吐 | ~15 M msg/s | ~1.7 M ops/s |
| 延迟 | ~7.7 ns | ~570 ns(逆推 1/1.7M) |
| 系统调用 | 0(纯用户态) | 2×(read+write) |
| 适用 | 同机进程间 IPC | 任意进程/网络 |

**结论**: ShmRingQueue 比 pipe 快约 **9 倍吞吐**,延迟低 **74 倍**。原因是零拷贝(共享内存直接读写,无内核态切换)。

### 对比特性能损耗

- **15 M msg/s**: 每条消息 128 字节 body → ~1.9 GB/s 有效带宽
- **7.7 ns**: 约 23 个 CPU 周期(3GHz),接近 L1 cache 访问延迟

---

## AsioUringIoBackend

### 单元测试 (条件编译: THUNDER_IO_ASIO_URING=ON)

| 测试 | 结果 |
|------|------|
| InitAndDestroy | ✅ |
| CreateListenSocket | ✅ |
| SubmitRead_InvalidFd | ✅ (io_uring 延后校验 fd) |
| CancelFd_NotFound | ✅ |
| HasPending_NotExist | ✅ |
| SubmitReadWrite_Pipe | ✅ |

**6/6 通过**

### IOPS 基准 (pipe)

```
pipe write→read 循环: 1,743,660 ops/s (100K rounds)
```

此为**同步** pipe 基准——作为 io_uring 异步 I/O 的对比参考。io_uring 的优势在**高并发批量**场景,单 pipe 无法体现。完整异步高并发 benchmark 需 `ev_loop` 驱动(下一步)。

### epoll vs io_uring 对比 (理论)

| 场景 | epoll | io_uring |
|------|-------|----------|
| 1 连接 read/write | ~1.7M ops(pipe) | 同(单连接无差异) |
| 1000 连接并发 | 1000×epoll_wait+read | 1×io_uring_enter |
| 大文件发送 | read+write(2 拷贝) | send_zc(0 拷贝) |
| 文件 I/O | 不支持 | 统一接口 |
| 内核版本 | 2.6+ | 5.1+ |

---

## 回归验证

| 模块 | 测试 | 结果 |
|------|------|------|
| ShmRingQueue | ctest -R ShmRingQueue | 13/13 ✅ |
| AsioUringIoBackend | ctest -R AsioUring | 6/6 ✅ |
| 全量 ctest | ctest -j1 | 304/304 ✅ |
