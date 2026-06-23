# IO 模块测试报告

> 日期: 2026-06-06

---

## ShmRingQueue

### 单元测试 (11 cases)

| 测试 | 覆盖 |
|------|------|
| CreateAndDestroy | 默认尺寸(128×4096)创建+销毁 |
| EnqueueDequeueSingle | 单消息入队出队, cmd/seq/body 一致性 |
| EmptyDequeueReturnsFalse | 空队列出队返回 false |
| FullQueueRejectsEnqueue | 满队列拒绝入队 |
| BodyTooLargeRejected | 超 slot_size 消息拒绝 |
| CreateDestroyNonDefaultSize | 非默认尺寸创建(16×2048)→正确 munmap |
| EventFdCreateClose | eventfd 创建/读写/关闭 |
| SPSC Threaded | 跨线程 SPSC 1M 消息,O(100K) rounds |
| ForkedProducerConsumer | fork 父子进程双向通信 |
| FallbackWhenQueueFull | 满队列 fallback |
| WorkerRestartSimulation | 销毁→重建,数据不泄漏 |

**结果**: 11/11 通过

### 性能测试 (2 cases)

| 测试 | 结果 |
|------|------|
| Throughput_1M_Messages | **15.38 M msg/s** (SPSC 跨线程 1M 消息) |
| Latency_SingleMessage | **6.23 ns/round** (单线程 100K rounds enqueue+dequeue) |

**环境**: Linux 7.0.0, gcc 15, -O2

### 运行

```bash
cmake --build build --target thunder_test_shm_queue -j1
./build/bin/thunder_test_shm_queue
```

---

## AsioUringIoBackend

### 单元测试 (6 cases)

| 测试 | 覆盖 |
|------|------|
| InitAndDestroy | 初始化 io_uring io_context + 销毁 |
| CreateListenSocket | 创建监听 socket(绑 127.0.0.1:19999) |
| SubmitRead_InvalidFd | 无效 fd 提交读(io_uring 延后验证,提交成功) |
| CancelFd_NotFound | 取消不存在 fd→不崩溃 |
| HasPending_NotExist | 查询不存在 fd→false |
| SubmitReadWrite_Pipe | pipe 读写提交 + HasPending 验证 |

**结果**: 6/6 通过

### 编译要求

```bash
cmake -DTHUNDER_IO_ASIO_URING=ON
```

无此 flag 时测试自动 SKIP。

### 已知限制

- io_uring 的 CQE 收割需要 ev_loop 运行(`ev_run`),单元测试中简化验证了提交和状态查询,未做完整的异步收割验证(需 ev_loop 驱动,适合 E2E)
- fixed buffers 测试需 `THUNDER_ASIO_URING_FIXEDBUF=1` 环境变量,当前未覆盖

---

## 回归验证

```bash
# ShmRingQueue
ctest -R ShmRingQueue --output-on-failure  # 13/13

# AsioUringIoBackend (需 io_uring 编译)
ctest -R AsioUring --output-on-failure     # 6/6 (或全部 SKIP)

# 全量
ctest -j1                                   # 304/304 (0 fail)
```
