# 共享内存 IPC — ShmRingQueue + LoaderConfigVersionData

> 详细设计: `docs/architecture/31-shmringqueue-design.md`
> 完整基准: `docs/performance/09-shmringqueue-benchmark.md`
> 代码: `code/Net/include/labor/types/ShmRingQueue.hpp`
>       `code/Net/include/labor/types/LoaderConfigVersionData.hpp`

---

## 1. 两套机制对比

```
┌──────────┬─────────────────────────────────────┬─────────────────────────────────────┐
│          │            ShmRingQueue             │       LoaderConfigVersionData       │
├──────────┼─────────────────────────────────────┼─────────────────────────────────────┤
│ 传什么   │ 命令、请求、响应（实时消息）        │ 配置文件内容（热更新）              │
├──────────┼─────────────────────────────────────┼─────────────────────────────────────┤
│ 模式     │ SPSC，1写1读                        │ 1写多读（Loader→Manager+N×Worker）  │
├──────────┼─────────────────────────────────────┼─────────────────────────────────────┤
│ 触发方式 │ eventfd 通知，Worker 收到立即处理   │ 每轮事件循环轮询 seq 版本号         │
├──────────┼─────────────────────────────────────┼─────────────────────────────────────┤
│ 使用场景 │ Manager 下发路由/etcd变更，         │ 配置文件热更新                      │
│          │ Worker 回报状态                     │                                     │
└──────────┴─────────────────────────────────────┴─────────────────────────────────────┘
```

**一句话区分**：ShmRingQueue 传"消息"，LoaderConfigVersionData 传"配置文件"。两者配合覆盖 Manager↔Worker 所有 IPC 需求。

---

## 2. ShmRingQueue 性能

### 吞吐：vs pipe（小包 9.3×）

| 包大小 | pipe | ShmRingQueue | 加速比 |
|--------|------|-------------|--------|
| 64 B   | 0.85 M/s | **7.9 M/s** | **9.3×** |
| 256 B  | 0.82 M/s | **7.4 M/s** | **9.0×** |
| 1 KB   | 0.71 M/s | **6.9 M/s** | **9.7×** |
| 4 KB   | 0.65 M/s | **5.0 M/s** | **7.7×** |
| 8 KB   | 0.51 M/s | **3.5 M/s** | **6.8×** |

### 延迟：跨进程 RTT

| 方案 | RTT |
|------|-----|
| pipe | 1175 ns |
| ShmRingQueue（跨进程） | **~130 ns（9×）** |
| ShmRingQueue（同线程） | 7.7 ns（152×）|

### 根本原因

```
pipe 一次往返 = 2 syscall（~1000ns）+ 2 次内核拷贝
ShmRingQueue  = 0 syscall + 0 拷贝 + memcpy(body) + atomic op

64B：pipe 1175ns = 1000ns(syscall) + 175ns(copy)，syscall 占 85%
     ShmRingQueue ~130ns = memcpy(64B) + atomic fence
```

---

## 3. LoaderConfigVersionData 性能

### 数据流

```
Loader 进程
  SetServerConfigFile()   → memcpy 配置 JSON 到 SHM body（≤16KB）
  IncLoaderConfigVersion()→ seq_config.fetch_add(1, release)

Manager / 每个 Worker（每轮事件循环）
  IsConfigVersionChange() → seq_config.load(acquire) vs m_consumedSeq  ← ~1ns
  有变化时：
    GetServerConfigFile() → memcpy SHM body 到本进程字符串（≤16KB）    ← ~3μs
    Parse()               → JSON 解析到 m_oCurrentConf（进程内存）
```

### 性能分析

| 操作 | 开销 | 说明 |
|------|------|------|
| 版本号检查（无变化时）| **~1 ns** | 一次 atomic load(acquire)，无 syscall，无锁 |
| 配置读取（有变化时） | **~3 μs** | memcpy 16KB @ DDR4，约占 3% 内存带宽 |
| 并发写冲突 | **无** | Loader 是唯一生产者，消费者只读，无需 CAS |
| 多 Worker 扩展性 | **线性** | 每个 Worker 独立轮询，互不干扰 |

### 设计关键

- `seq_config = 0` 表示未发布，Loader 从 1 开始单调递增，不回绕（uint64 @ 10亿次/秒可用 584 年）
- 生产者协议：先写 body → 再 `release` 写 seq，保证消费者读到完整配置
- 消费者协议：先 `acquire` 读 seq → 再读 body，保证不读到半写状态
- 固定 16KB body（不动态分配）：SHM 不支持堆分配器，避免跨进程指针

---

## 4. 适用场景

| 场景 | 用哪个 | 原因 |
|------|--------|------|
| Manager 下发命令/etcd路由变更 | ShmRingQueue | 实时消息，eventfd 低延迟通知 |
| Worker 回报状态/心跳 | ShmRingQueue | 反向 SPSC |
| 配置文件热更新 | LoaderConfigVersionData | 1写多读，版本号驱动 |
| 跨机器通信 | ❌ 两者均不适用 | 共享内存不跨机器，用 socket |
| 一对多广播消息 | ❌ ShmRingQueue 不适用 | SPSC 不支持多消费者 |
| 超大消息（>4KB）| ❌ ShmRingQueue 不适用 | slot 固定大小，fallback 到 socket |

---

## 5. 质量测试

### 5.1 ASan — 堆越界检测

`ShmRingQueue` 直接操作共享内存指针，是内存错误高发区。`build-asan` 目录已用 `-fsanitize=address,undefined` 编译。

```bash
ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:leak_check_at_exit=1" \
    build-asan/bin/thunder_test_shm_queue
```

**无错时**（当前实际输出）：

```
[==========] 10 tests from 2 test suites ran. (26 ms total)
[  PASSED  ] 10 tests.
```

**有错时**（例如 `GetSlotData` 返回的指针越界写）：

```
==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x...
WRITE of size 128 at 0x... thread T0
    #0 0x... in ShmRingQueue::TryEnqueue(...)
          code/Net/include/labor/types/ShmRingQueue.hpp:187
    #1 0x... in ShmRingQueueUnit_Overflow_Test::TestBody()
          code/test/labor/test_shm_queue.cpp:45

0x... is located 0 bytes after 64-byte region [0x...,0x...)
allocated by thread T0 here:
    #0 0x... in ShmRingQueue::Create(unsigned int, unsigned int)
          code/Net/include/labor/types/ShmRingQueue.hpp:130

SUMMARY: AddressSanitizer: heap-buffer-overflow ShmRingQueue.hpp:187 in ShmRingQueue::TryEnqueue
```

调用栈里出现 `ShmRingQueue::` 前缀的函数 = Thunder 代码本身的问题，需要修。只出现 `OpenSSL::` 或第三方的可暂时忽略。

### 5.2 单元测试覆盖（24/24）

```bash
ctest --test-dir build -R shm_queue -v
```

| 类别 | 测试数 | 内容 |
|------|--------|------|
| 基本 | 11 | Create/Destroy/Enqueue/Dequeue/Full/Empty/BodyTooLarge |
| 边界 | 6 | nullptr/eventfd/Count/MaxBody/MultipleCreate/Fallback |
| 性能 | 4 | QPS 64B/256B/1KB/4KB |
| 延迟 | 1 | SPSC 1M 消息 latency |
| E2E | 2 | ForkedProducerConsumer/WorkerRestart |

**当前结果：24/24 全部通过**

### 5.3 性能基准回归

```bash
# 跑 ShmRingQueue 性能测试（含 pipe 对比）
./build/bin/thunder_test_shm_queue \
    --gtest_filter="*Bench*:*Latency*" --gtest_also_run_disabled_tests
```

预期：64B QPS ≥ 7M/s，跨进程 RTT ≤ 200ns。数字下降超过 20% 说明有回归。

### 5.4 静态阅读检查点

ShmRingQueue 有三处容易出错的并发点：

| 检查点 | 正确写法 | 错误后果 |
|--------|---------|---------|
| 生产者写 slot 后再推进 write_index | `store(wi+1, release)` | 消费者读到未写完的 slot |
| 消费者读 write_index 用 acquire | `load(acquire)` | 可见性破坏，漏消息 |
| eventfd 通知时机 | enqueue 完成后再 write(1) | 通知过早，消费者读空 |

按静态代码阅读 → 压测确认的顺序处理，见 `docs/quality/02-static-vs-profiler.md`。
