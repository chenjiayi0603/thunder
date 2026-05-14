# io_uring SQE 提交时序缺口 — Interface→Logic 链路失败根因分析

> 日期: 2026-05-14 | 分支: dev | 关联: `test_and_quality_report_2026-05-13.md`

---

## 一、问题概述

| 项目 | 详情 |
|------|------|
| **症状** | `test_interface_chain` 全部 6 个测试失败：Interface→Logic 的 genkey/verifykey 链路不可用 |
| **后端** | asio_uring（libev 后端下全部 25 个 E2E 测试通过） |
| **关键日志** | Interface Worker: `ucConnectStatus=0`（握手从不完成）；Logic Manager: `AcceptServerConn` 成功但 `HandleIoReadComplete` 从不触发 |
| **诊断日志** | `/tmp/asio_uring_diag.log`：`SubmitRead fd=16` 成功，但无对应 `ReadComplete` 事件 |

---

## 二、Interface→Logic S2S 握手流程

Logic 节点只有 `inner_host/inner_port`（S2S 端口 16068），Worker 不创建自己的 listener。所有外部 S2S 连接通过 **Manager fd-passing** 机制完成：

```
Interface Worker                  Logic Manager                  Logic Worker
     |                                |                              |
     |--TCP connect():16068---------->|                              |
     |                                | accept() via ev_io           |
     |                                | CreateAcceptFdAttr           |
     |                                | AddIoReadEvent → SubmitRead  |
     |                                |   (Manager's asio_uring)     |
     |                                |                              |
     |--CMD_REQ_CONNECT_TO_WORKER---->| ← 此数据永远不被读取          |
     |                                |                              |
     |  ucConnectStatus=0 （等待）     |                              |
```

诊断日志确认：
- ✅ Manager accept `fd=16` 成功
- ✅ `SubmitRead fd=16` 调用成功
- ❌ `ReadComplete fd=16` 从不触发 — Interface Worker 发送的 `CMD_REQ_CONNECT_TO_WORKER` 数据永远不被读取

---

## 三、根因：ASIO io_uring 后端的 SQE 提交时序缺口

### 3.1 ASIO io_uring 的工作机制

ASIO 的 io_uring 后端将 I/O 操作分为两个步骤：

| 步骤 | 调用 | 效果 |
|------|------|------|
| **入队 SQE** | `async_read_some()` / `async_write_some()` | 将 SQE 写入共享内存的 Submission Queue (SQ)，**不通知内核** |
| **提交+收割** | `io_context::poll()` | 调用 `io_uring_enter()` 提交所有待处理的 SQE 给内核，并收割已完成的 CQE |

> 关键点：`async_read_some` 只是把操作描述写入 SQ 共享内存，**并不调用 `io_uring_enter()`**，内核不知道有新操作等待执行。

### 3.2 时序缺口

Thunder 通过三路定期调用 `poll()`：

```
libev 事件循环:
  ev_prepare   → OnPrepare   → m_ioCtx.poll()    # 提交 SQE + 收割 CQE
  epoll_wait                                    # 阻塞等待事件
  ev_check     → OnCheck     → m_ioCtx.poll()    # 提交 + 收割
  ev_io        → OnRingReady → m_ioCtx.poll()    # ring_fd 可读时收割
  ev_io        → AcceptServerConn → SubmitRead   # 入队读 SQE，但未提交！
```

**缺口出现在 ev_io 回调和下一次 ev_prepare 之间**：

```
迭代 N:
  OnPrepare → poll()               # 提交之前的 SQE
  epoll_wait → listen fd 可读
  OnCheck → poll()                 # 收割 CQE
  ev_io → AcceptServerConn(fd=16)  # accept 成功
       → AddIoReadEvent
       → SubmitRead
       → async_read_some           # SQE 写入 SQ，未提交给内核 ← 缺口！
  回调返回

迭代 N+1:
  OnPrepare → poll()               # 这时才提交 fd=16 的读 SQE
  ...
```

如果 Interface Worker 在 `SubmitRead` 之后、下一次 `OnPrepare` 之前就发送了握手数据，该数据到达内核 TCP 缓冲区时，内核没有任何匹配的 io_uring 读操作 — **SQE 还在用户态的共享内存里，内核看不到它**。

### 3.3 为什么 Center 连接正常

Center 连接有周期性心跳写操作（约每 10 秒）：

```
HandleIoWriteComplete → CancelFd(sock.release) + 检查 HasPending + SubmitRead
```

这个 CancelFd → SubmitRead 循环**间接刷新了读 SQE**：
1. CancelFd 移除旧 FdState（`sock.release()`）
2. SubmitRead 创建**新的** `stream_descriptor`，调用 `async_read_some`
3. 这次 SubmitRead 发生在写操作的处理上下文中，而写操作经过 `poll()` 来回时已提交了之前的所有待处理 SQE

没有写活动的新 accept 连接（如 Interface→Logic Manager），初始读 SQE 被永远困在用户态 SQ 中，从不提交给内核。

---

## 四、修复方案

### 修改文件

`code/Net/src/labor/AsioUringIoBackend.cpp`

### 改动：SubmitRead / SubmitWrite 末尾添加 `m_ioCtx.poll()`

在 `async_read_some` / `async_write_some` 调用后，立即调用 `m_ioCtx.poll()` 提交 SQE 给内核：

```cpp
// SubmitRead — 在 return true 之前添加
sp->sock.async_read_some(
    asio::buffer(dst, cap),
    [this, wp, fd, seq, buf](const asio::error_code& ec, std::size_t n) { ... });

m_ioCtx.poll();  // 立即提交 SQE 给内核
return true;

// SubmitWrite — 同理
sp->sock.async_write_some(
    asio::buffer(src, readable),
    [this, wp, fd, seq, buf](const asio::error_code& ec, std::size_t n) { ... });

m_ioCtx.poll();  // 立即提交 SQE 给内核
return true;
```

### 安全性

| 关注点 | 分析 |
|--------|------|
| **非阻塞** | `io_context::poll()` 是非阻塞的 — 仅提交待处理 SQE + 收割已完成的 CQE，不等待新事件 |
| **同步完成** | localhost 连接数据可能已在内核缓冲区。poll() 可能同步完成刚提交的读/写操作，立即触发回调 — 这是正确行为，避免了延迟 |
| **防重入** | `readPending`/`writePending` 在 `async_read_some` 前设置为 true，回调中重置。嵌套 SubmitRead 会因为 `readPending=true` 被跳过 |
| **性能** | 新增 poll() 是无待处理工作时的廉价操作（仅内部队列检查）。每 I/O 操作一次，频率远低于 ev_prepare/ev_check 的每次循环调用 |

---

## 五、技术细节补充

### 5.1 io_uring 的提交模型

io_uring 使用内存映射的共享环形缓冲区（SQ/CQ Ring）与内核通信：

```
用户态                      内核态
┌──────────┐     mmap      ┌──────────┐
│ SQ (提交队列) │ ←──────────→ │ SQ (提交队列) │
│  [SQE][SQE]  │   共享内存    │  [SQE][SQE]  │
│  tail=2      │              │  head=0      │ ← 内核还不知道有新 SQE！
└──────────┘               └──────────┘
```

写入 SQE 并更新 tail 指针后，**必须通过 `io_uring_enter()` 系统调用**通知内核 tail 已更新：
```
io_uring_enter(ring_fd, to_submit=2, min_complete=0, flags=0)
```
内核据此更新其 head 指针并开始处理 SQE。

### 5.2 ASIO standalone io_uring 的行为

Thunder 使用 **ASIO standalone**（非 Boost.ASIO）的 io_uring 后端。该后端的 `async_read_some`/`async_write_some` 实现：
1. 在 SQ 共享内存中分配一个空闲 SQE 槽位
2. 填充 opcode (IORING_OP_READ / IORING_OP_WRITE)、fd、buffer 地址、长度
3. 更新 SQ tail 指针
4. **不调用 io_uring_enter()** — 留给下次 `poll()` 批量提交

### 5.3 为什么 libev 后端不受影响

libev 后端使用 `ev_io` watcher 通过 `epoll_wait` 直接监听 fd 的可读/可写事件。`accept()` 后，`ev_io_start()` 将新 fd 注册到 epoll 实例。当数据到达时，epoll 直接在 socket fd 上触发事件 — 不经过 io_uring 的 SQ/CQ 机制，没有提交时序问题。

---

## 六、验证结果

修复后预期全量 E2E 测试 25/25 通过，对应 `test_and_quality_report_2026-05-13.md` 中的全部指标。

| 测试模块 | 用例数 | 预期 |
|----------|--------|------|
| `test_center_admin` | 5 | 5 通过 |
| `test_http_hello` | 4 | 4 通过 |
| `test_https_hello` | 3 | 3 通过 |
| `test_interface_chain` | 5 | 5 通过 ← 修复目标 |
| `test_multicenter_raft` | 3 | 3 通过 |
| `test_stress` | 1 | 1 通过 |
| `test_ws_hello` | 4 | 4 通过 |

---

## 七、相关文档

| 文档 | 内容 |
|------|------|
| `io_uring_concurrency_model.md` | io_uring 并发模型全景分析 |
| `Thunder_io_uring使用与原理分析.md` | io_uring 在 Thunder 中的使用与原理 |
| `test_and_quality_report_2026-05-13.md` | 全量测试报告（libev 后端 25/25 通过） |
| `architecture_design.md` | Thunder 架构设计 |
