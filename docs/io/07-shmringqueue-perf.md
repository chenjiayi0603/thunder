# ShmRingQueue 性能测试报告

> 测试: `code/test/labor/test_shm_queue.cpp`
> 设计: `docs/io/07-shmringqueue-design.md`

---

## 1. 吞吐量 (SPSC 跨线程)

| 包大小 | QPS | 吞吐量 | 时延 |
|--------|-----|--------|------|
| 64 B | **7.9 M/s** | 484 MB/s | 126 ns |
| 128 B | **7.5 M/s** | 900 MB/s | 133 ns |
| 256 B | **7.4 M/s** | 1,795 MB/s | 135 ns |
| 512 B | **7.1 M/s** | 3,470 MB/s | 141 ns |
| 1 KB | **6.9 M/s** | 6,782 MB/s | 145 ns |
| 2 KB | **6.0 M/s** | 11,720 MB/s | 167 ns |
| 4 KB | **5.0 M/s** | 19,530 MB/s | 200 ns |
| 8 KB | **3.5 M/s** | 27,340 MB/s | 286 ns |

> 测试: 500K messages SPSC 跨线程, gtest, -O2

## 2. 延迟

| 测试 | 结果 | 分析 |
|------|------|------|
| 单消息 RTT (同线程) | **7.7 ns** | ~23 CPU cycles, L1 cache 级 |
| SPSC 跨线程 | **~130 ns** | cache line bouncing |

## 3. 对比 pipe (syscall baseline)

| 包大小 | pipe QPS | ShmRingQueue QPS | 加速比 |
|--------|----------|-----------------|--------|
| 64 B | 0.85 M/s | 7.9 M/s | **9.3×** |
| 256 B | 0.82 M/s | 7.4 M/s | **9.0×** |
| 1 KB | 0.71 M/s | 6.9 M/s | **9.7×** |
| 8 KB | 0.51 M/s | 3.5 M/s | **6.8×** |

**pipe 瓶颈**: syscall ~1000ns 固定, 占小包 85% 延迟
**ShmRingQueue 瓶颈**: 内存带宽, 8KB 时 ~27 GB/s (DDR4 上限 ~50 GB/s)

## 4. 单元测试

| 类别 | 数量 | 覆盖 |
|------|------|------|
| 基本功能 | 11 | Create/Destroy/Enqueue/Dequeue/Full/Empty |
| 边界 | 6 | nullptr/eventfd/Count/MaxBody/MultipleCreate |
| 性能 | 4 | QPS 64B~4KB |
| E2E | 2 | ForkedProducerConsumer/WorkerRestart |

**23/23 全部通过**
