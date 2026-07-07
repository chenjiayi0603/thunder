# ShmRingQueue 对比测试

> 测试代码: `code/test/labor/test_shm_queue.cpp`
> 设计文档: `docs/architecture/31-shmringqueue-design.md`

---

## 1. 对比对象

ShmRingQueue 是 Thunder Manager↔Worker IPC 通道。对比三种替代方案:

| 方案 | 原理 | 拷贝次数 | syscall |
|------|------|---------|---------|
| **pipe** | 内核管道, read/write | 2 (用户↔内核) | 2/次 |
| **socketpair** | 同 pipe, 双向 | 2 | 2/次 |
| **POSIX mq** | 内核消息队列 | 2 | 2/次 |
| **ShmRingQueue** | 共享内存 + 环形缓冲 | 0 | 0 |

---

## 2. 吞吐对比

| 包大小 | pipe QPS | pipe 时延 | ShmRingQueue QPS | ShmRingQueue 时延 | 加速比 |
|--------|----------|----------|-----------------|------------------|--------|
| 64 B | 0.85 M/s | 1175 ns | **7.9 M/s** | ~126 ns | **9.3×** |
| 128 B | 0.83 M/s | 1200 ns | **7.5 M/s** | ~133 ns | **9.0×** |
| 256 B | 0.82 M/s | 1214 ns | **7.4 M/s** | ~135 ns | **9.0×** |
| 512 B | 0.79 M/s | 1266 ns | **7.1 M/s** | ~141 ns | **9.0×** |
| 1 KB | 0.71 M/s | 1405 ns | **6.9 M/s** | ~145 ns | **9.7×** |
| 2 KB | 0.69 M/s | 1440 ns | **6.0 M/s** | ~167 ns | **8.6×** |
| 4 KB | 0.65 M/s | 1548 ns | **5.0 M/s** | ~200 ns | **7.7×** |
| 8 KB | 0.51 M/s | 1953 ns | **3.5 M/s** | ~286 ns | **6.8×** |

> 测试: pipe Python 50K rounds, ShmRingQueue C++ gtest 500K rounds

---

## 3. 差距分析

```
pipe 一次往返 = 2 syscall + 2 内核拷贝
syscall: ~500ns × 2 = ~1000ns (固定, 不随包大小变化)
拷贝:   size / 带宽

64B 时: 1175ns = 1000ns(syscall) + 175ns(copy) → syscall 占 85%
8KB 时: 1953ns = 1000ns(syscall) + 953ns(copy) → syscall 占 51%

ShmRingQueue: 零 syscall + 共享内存直接读写
64B 时: ~126ns = memcpy(64B) + atomic op
8KB 时: ~286ns = memcpy(8KB) + atomic op
```

**加速比递减原因**: 包越大, syscall 节省的占比越小。64B 时省了 1000ns(占 85%), 8KB 时只省 1000ns(占 51%)。

---

## 4. 延迟对比

| 测试 | 结果 | 对比 |
|------|------|------|
| pipe RTT | **1175 ns** | 基线 |
| ShmRingQueue RTT (同线程) | **7.7 ns** | **152× 更快** |
| ShmRingQueue RTT (跨线程) | **~130 ns** | **9× 更快** |

> 同线程: enqueue+dequeue 在同一线程, 测纯队列开销
> 跨线程: SPSC 模式, 含 cache line bouncing

---

## 5. ShmRingQueue 吞吐详细

| 包大小 | QPS | 吞吐量 |
|--------|-----|--------|
| 64 B | 7.9 M/s | 484 MB/s |
| 256 B | 7.4 M/s | 1,795 MB/s |
| 1 KB | 6.9 M/s | 6,782 MB/s |
| 4 KB | 5.0 M/s | 19,530 MB/s |
| 8 KB | 3.5 M/s | **27,340 MB/s** |

- QPS 随包增大下降: 7.9M → 3.5M (memcpy 主导)
- 吞吐随包增大上升: 484 MB → 27.3 GB (大包带宽效率高)
- 8KB 时已达 DDR4 带宽 ~55%

---

## 6. 单元测试覆盖

| 类别 | 测试数 | 内容 |
|------|--------|------|
| 基本 | 11 | Create/Destroy/Enqueue/Dequeue/Full/Empty/BodyTooLarge |
| 边界 | 6 | nullptr/eventfd/Count/MaxBody/MultipleCreate/Fallback |
| 性能 | 4 | QPS 64B/256B/1KB/4KB |
| 延迟 | 1 | SPSC 1M 消息 latency |
| E2E | 2 | ForkedProducerConsumer/WorkerRestart |

**24/24 全部通过**

---

## 7. 适用场景

| 场景 | 推荐 | 原因 |
|------|------|------|
| Manager↔Worker IPC | ShmRingQueue | 零拷贝, fork 继承 |
| 跨机器通信 | socket | 共享内存跨不了机器 |
| 广播/多消费者 | pipe/mq | SPSC 不支持多消费者 |
| 超大消息 (>4KB) | socket | ShmRingQueue slot 固定大小 |
