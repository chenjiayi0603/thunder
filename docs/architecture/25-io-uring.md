# Thunder io_uring 后端

> 代码: `code/Net/src/labor/NativeUringIoBackend.{cpp,hpp}`, `code/Net/src/labor/AsioUringIoBackend.{cpp,hpp}`
> 详细设计: `docs/architecture/30-asio-uring-design.md`

---

## 1. io_uring 简介

io_uring 是 Linux 5.1+ 的异步 I/O 接口。和 epoll 的核心区别:

| | epoll | io_uring |
|---|-------|----------|
| 通知模型 | 就绪通知 (`fd 可读了`) | 完成通知 (`读完了, 数据在这`) |
| 每次 I/O | read/write syscall | 批量 SQE 提交 + CQE 收割, 0 额外 syscall |
| 高并发 | syscall 次数瓶颈 | SQ/CQ 深度瓶颈 |

**两个环形队列**: SQ(用户→内核, 提交 I/O 请求), CQ(内核→用户, 返回完成结果)。共享内存, 无需系统调用即可读写。

---

## 2. Thunder 两套 io_uring 实现

| | NativeUringIoBackend | AsioUringIoBackend |
|---|---------------------|-------------------|
| 依赖 | liburing | stand alone ASIO |
| 代码量 | 534 行 | 745 行 |
| SQ/CQ | 手写 ring buffer | ASIO 管理 |
| watcher | 2 路 | 3 路 (多了 ev_check 补刀) |
| 零拷贝 | 需实现 | send_zc + fixed buffers |
| 空唤醒 | 存在 | UpdateRingWatcher 按需启停 |

### 2.1 NativeUring — 调用过程

```
Worker → SubmitRead(fd, buf)
  → 直接构造 io_uring_sqe → 写入 SQ ring
  → ev_prepare: io_uring_submit() 批量提交
  → epoll_wait → ring_fd 可读
  → ev_io: io_uring_peek_cqe() 取结果 → callback
```

### 2.2 AsioUring — 三路驱动

```
Worker → SubmitRead(fd, buf)
  → ASIO async_read_some → 生成 SQE (不提交)
  → ev_prepare: io_context.poll() ①批量提交 ②收割上一轮 CQE
  → epoll_wait → ring_fd 可读
  → ev_io: poll() 收割刚完成的 CQE → completion lambda
  → ev_check: poll() 补收 race window CQE + UpdateRingWatcher
```

详见 `docs/architecture/30-asio-uring-design.md`。

---

## 3. 实测性能 (wrk HTTP)

| backend | 空body | 1KB | 4KB |
|---------|--------|-----|-----|
| ev (epoll) | 109K | 59K | 23K |
| NativeUring | 90K | 68K | 24K |
| AsioUring | 108K | **71K** | **39K** |

- **空body: ev 最快** (109K) — syscall 开销小, epoll 简单高效
- **1KB+: AsioUring 反超** (71K vs ev 59K) — 批量提交 + 零拷贝体现优势
- **4KB: AsioUring 快 70%** (39K vs ev 23K) — 大包时 syscall 和拷贝开销主导

结论: 小请求用 ev, 大请求用 AsioUring。NativeUring 无零拷贝, 大包性能差于 AsioUring。

---

## 4. 选择建议

| 场景 | 推荐 | 原因 |
|------|------|------|
| 中小规模 (<1K conn) | **ev** | 简单, 实测最快 |
| 大并发 (1K-10K) | **AsioUring** | 批量提交 + 零拷贝 |
| 无 ASIO 依赖 | **NativeUring** | 编译快, 零外部依赖 |
| 极致 (>10M pps) | **DPDK** | 用户态网卡 |

---

## 5. DPDK

DPDK 需要独占网卡, Thunder 当前无 DPDK 测试环境。对比见 `docs/architecture/26-dpdk-design.md`。
