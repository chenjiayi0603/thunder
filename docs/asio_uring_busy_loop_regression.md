# AsioUringIoBackend ring_fd 忙循环回归问题

> 状态：**未解决** — 根因分析已提供，待继续排查
> commit: 3e6939a (fix: Worker 侧 ENOTCONN 修复引入后触发)

---

## 一、问题现象

### 1.1 触发条件

提交 3e6939a（Worker 侧 ENOTCONN 修复）部署后，Interface 节点 `Interface_robot_W0` 进程 CPU 升至 31%（此前正常为 <5%）。`/tmp/asio_uring_diag.log` 迅速增长至 19GB，内容为 `OnRingReady` 回调的重复刷屏。

### 1.2 日志特征

```
[07:17:03] [IODIAG AsioUring OnRingReady ring_fd=13 poll=0
[07:17:03] [IODIAG AsioUring OnRingReady ring_fd=13 poll=0
[07:17:03] [IODIAG AsioUring OnRingReady ring_fd=13 poll=0
...（每秒数万行）
```

关键特征：
- `ring_fd=13` — 同一次启动中 ring_fd 不变
- `poll=0` — `io_context::poll()` 返回 0，即**没有完成事件可处理**
- 刷屏前约 13 分钟正常（07:04 启动，07:17 开始刷屏），此期间成功的读操作日志可见

### 1.3 功能影响

| 测试 | 结果 | 说明 |
|------|------|------|
| `test_interface_http_co20_echo` | ✅ PASS | 纯 HTTP echo，无 S2S 转发 |
| GenKey 链式测试 (4 个) | ❌ FAIL | HTTP 连接无响应 |
| 直接 curl Interface | ❌ 超时 | 服务不响应 |

---

## 二、忙循环机制 — 死循环调用链

```
libev ev_run 主循环
  └─ epoll_wait()                            ← 立即返回（ring_fd 可读）
       └─ ev_io callback: OnRingReady         ← ring_fd=13 EV_READ 触发
            ├─ m_ioCtx.poll()                 ← ASIO poll（无超时）
            │    └─ io_uring_service::run(0, ops)
            │         ├─ io_uring_peek_cqe(&ring_, &cqe)
            │         │    └─ 返回 -EAGAIN   ← CQ 为空！
            │         ├─ 跳过 CQE 处理循环 (result != 0)
            │         └─ return 0             ← count = 0
            │
            └─ diag_log()                     ← 写入 + fflush 磁盘
                 └─ OnRingReady poll=0
  └─ 下一轮 ev_run → epoll_wait 立即返回 ...
```

**核心矛盾**：`epoll_wait` 认为 ring_fd 可读，但 `io_uring_peek_cqe` 返回 `-EAGAIN`（无 CQE）。libev 使用**电平触发**（level-triggered）监控 ring_fd，只要 `ring->cq.head != ring->cq.tail`（内核视角的 CQ 环形缓冲区状态），epoll 就会持续返回可读。

---

## 三、相关代码路径

### 3.1 AsioUringIoBackend 初始化三路驱动

```cpp
// AsioUringIoBackend::Init()
ev_io_init(&m_ringWatcher, &OnRingReady, m_ringFd, EV_READ);  // ring_fd ev_io
ev_prepare_init(&m_prepare, &OnPrepare);                       // epoll_wait 前
ev_check_init(&m_check, &OnCheck);                             // epoll_wait 后
```

### 3.2 三路回调

```cpp
// OnPrepare — epoll_wait 前拉取
void AsioUringIoBackend::OnPrepare(ev_prepare* w, int) {
    auto* be = static_cast<AsioUringIoBackend*>(w->data);
    auto n = be->m_ioCtx.poll();  // 有完成事件才打日志 (n>0)
}

// OnCheck — epoll_wait 后拉取
void AsioUringIoBackend::OnCheck(ev_check* w, int) {
    auto* be = static_cast<AsioUringIoBackend*>(w->data);
    auto n = be->m_ioCtx.poll();
}

// OnRingReady — ring_fd 可读触发  ← 这是刷屏的来源
void AsioUringIoBackend::OnRingReady(ev_io* w, int) {
    auto* be = static_cast<AsioUringIoBackend*>(w->data);
    auto n = be->m_ioCtx.poll();
    diag_log("[IODIAG AsioUring OnRingReady ring_fd=%d poll=%zu\n",
             be->m_ringFd, n);  // ← 无条件日志！
}
```

### 3.3 ASIO io_uring_service::run() — CQE 收割核心

```cpp
void io_uring_service::run(long usec, op_queue<operation>& ops)
{
    // 1. 非阻塞窥探 CQE
    ::io_uring_cqe* cqe = 0;
    int result = (usec == 0)
        ? ::io_uring_peek_cqe(&ring_, &cqe)    // -EAGAIN if empty
        : ::io_uring_wait_cqe(&ring_, &cqe);

    // 2. 循环收割
    int count = 0;
    while (result == 0 || local_ops > 0)
    {
        if (result == 0)
        {
            io_queue* io_q = static_cast<io_queue*>(::io_uring_cqe_get_data(cqe));
            io_q->set_result(cqe->res);
            ops.push(io_q);
        }
        ::io_uring_cqe_seen(&ring_, cqe);  // 标记 CQE 已消费
        ++count;
        result = (count < complete_batch_size)
            ? ::io_uring_peek_cqe(&ring_, &cqe) : -EAGAIN;
    }

    decrement(outstanding_work_, count);
}
```

**关键行为**：
- `io_uring_peek_cqe` 返回 `-EAGAIN` 表示 CQ 为空
- `io_uring_cqe_seen` 逐个标记 CQE 已消费（更新 `ring->cq.khead`）
- 循环结束后**不会显式调用** `io_uring_cq_advance`

### 3.4 ASIO eventfd 注册

```cpp
// io_uring_service::init_ring()
event_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
result = ::io_uring_register_eventfd(&ring_, event_fd_);

// io_uring_service::register_with_reactor()
reactor_.register_internal_descriptor(reactor::read_op,
    event_fd_, reactor_data_, new event_fd_read_op(this));
```

`io_uring_register_eventfd` 让内核在每次 CQE 入队时写入 eventfd。ASIO 内部通过自己的 epoll reactor 监控 eventfd，并在 `event_fd_read_op::do_perform()` 中：
1. 读取 eventfd 计数器
2. 调用 `run(0, ops)` 收割 CQE
3. 返回 `not_done`（持续重新调度，保持 eventfd 监听活跃）

---

## 四、根因分析 — 核心假设

### 假设 1：`io_uring_cqe_seen` 未更新共享内存 ring head（最可能）

**机理**：

```
epoll_wait 判断 ring_fd 可读的依据：
  io_uring ring 共享内存中的 ring->cq.head（内核写） vs ring->cq.tail（用户态读）

ASIO 的 CQE 收割：
  io_uring_cqe_seen(ring, cqe)
    └─ 在较新 liburing 中只更新本地 khead 计数器
       └─ 不写回共享内存 ring->cq.head

结果：
  内核看到的 ring->cq.head（旧值）≠ ring->cq.tail → ring_fd 永远可读
```

**验证方法**：检查 liburing 版本中 `io_uring_cqe_seen` 的实现是仅仅更新本地计数器，还是同时写回共享内存。

### 假设 2：eventfd + ring_fd 双重唤醒竞争

`io_uring_register_eventfd` 注册后，每次 CQE 入队同时触发：
- eventfd 写入 → ASIO 内部 reactor 唤醒
- ring_fd 可读 → libev ev_io 唤醒

如果 ASIO 内部 reactor 先消费 CQE（通过 eventfd_read_op），ring_fd 仍然可读（因为 CQE 已被收割但 ring head 未推进），libev 侧触发空轮询。

### 假设 3：repeat-op 导致的持续提交

`event_fd_read_op::do_perform` 返回 `not_done`，ASIO reactor 会重新提交该 op。每次重新提交都可能触发一次 `io_uring_enter`，产生新的 CQE（op completion），写入 eventfd，形成自激振荡。

### 假设 4：修复前后差异 — asio_uring 实际使用量变化

修复前 ENOTCONN bug 导致 Worker 所有 outgoing 连接在 `SubmitRead(unconnected)` → `poll()` → 同步 ENOTCONN → `DestroyConnect` 路径中被快速销毁，**asio_uring 实际上没有处理真正的 I/O**。修复后连接成功建立，真正的读/写操作开始在 asio_uring 上执行，暴露了此前隐藏的 ring_fd 管理问题。

---

## 五、排查方向

### 5.1 确认 liburing 的 CQE 消费语义

```bash
# 检查 liburing 版本中 io_uring_cqe_seen 的实现
grep -A10 "io_uring_cqe_seen" /path/to/liburing/io_uring.h

# 关键问题：
# - 是否只更新 ring->cq.khead（本地）？
# - 是否写回 ring->cq.ring->head（共享内存）？
```

### 5.2 添加 ring_fd 状态日志

在 `OnRingReady` 的 `poll()` 前后，打印 ring->cq 的 head/tail 状态：

```cpp
// poll 前后状态对比
LOG("ring_fd=%d BEFORE poll: head=%u tail=%u",
    be->m_ringFd, ring->cq.ring->head, ring->cq.ring->tail);
be->m_ioCtx.poll();
LOG("ring_fd=%d AFTER  poll: head=%u tail=%u",
    be->m_ringFd, ring->cq.ring->head, ring->cq.ring->tail);
```

### 5.3 检查 eventfd 状态

```bash
# 查看 eventfd 描述符及当前值
ls -la /proc/<Interface_robot_W0_pid>/fd | grep eventfd
```

### 5.4 试关闭 eventfd 路径

设置 `ASIO_HAS_IO_URING_AS_DEFAULT` 宏，让 ASIO 跳过 `io_uring_register_eventfd` 注册，仅依靠 ring_fd 的 direct polling。观察忙循环是否消失。

### 5.5 改为边缘触发

若 libev 支持，将 `ev_io_init` 改为边缘触发（EPOLLET），观察是否消除空轮询。若消除，则确认是电平触发 + ring head 不推进的组合问题。

### 5.6 perf 确认 CPU 热点

```bash
perf top -p <Interface_robot_W0_pid>
# 预期热点：ev_io callback → poll() → diag_log fflush
```

---

## 六、相关代码文件索引

| 文件 | 说明 |
|------|------|
| `code/Net/src/labor/AsioUringIoBackend.cpp` | 三路驱动实现，OnRingReady 回调（行 266-271） |
| `code/3party/asio/include/asio/detail/io_uring_service.hpp` | ASIO io_uring_service 类声明 |
| `code/3party/asio/include/asio/detail/impl/io_uring_service.ipp` | run()/start_op()/init_ring() 实现 |
| `code/3party/asio/include/asio/detail/impl/io_uring_service.ipp:542` | `io_uring_register_eventfd` 调用 |
| `code/3party/asio/include/asio/detail/impl/io_uring_service.ipp:564` | `event_fd_read_op` 类定义 |
| `code/Net/src/labor/Worker.cpp` | ENOTCONN 修复（5 处编辑，commit 3e6939a） |
| `docs/io_uring_concurrency_model.md` | 并发模型完整分析（三路驱动、eventfd、ENOTCONN 等） |
