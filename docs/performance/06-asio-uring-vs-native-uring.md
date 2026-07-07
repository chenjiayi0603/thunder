# AsioUring vs NativeUring 性能对比分析

> 数据来源: `docs/performance/10-vs-nginx-benchmark-20260610.md`（2026-06-12 复测，#70 修复后）
> 环境: i9-12900H，原生运行，P-core 4-9 绑核，INFO log，1 Worker，`/hello/raw` Fast-Path

---

## 1. 数据

### HTTP（64B payload）

| 后端 | 吞吐 (RPS) | 平均延迟 | vs ev | vs asio_uring |
|------|-----------|---------|-------|---------------|
| ev (epoll) | 232k | 424μs | 基准 | — |
| **native_uring** | **203k** | **427μs** | **−13%，延迟持平** | **−14%，延迟差 1.9×** |
| **asio_uring** | **235k** | **220μs** | **+1%，延迟好 1.9×** | 基准 |
| Nginx 1w | 214k | 466μs | — | — |

**结论：native_uring HTTP 全线垫底，吞吐不如 ev，延迟与 ev 相同。**

### HTTPS（SSL 加密，native_uring 延迟略好于 asio_uring）

| 后端 | 吞吐(64B) | 延迟(64B) | 延迟(4K) |
|------|----------|---------|---------|
| ev | 141k | 803μs | 1230μs |
| native_uring | 133k | 394μs | 218μs |
| asio_uring | 133k | 402μs | 247μs |
| Nginx-ssl | 149k | 752μs | 824μs |

**HTTPS 三后端吞吐持平（SSL 加解密是 CPU 瓶颈），uring 系延迟均显著优于 ev；native_uring 与 asio_uring 差距 8~29μs，可忽略。**

---

## 2. 根因

### 2.1 核心差距：SQE 提交粒度

**native_uring：每次 I/O 立即提交（批量=1）**

```cpp
// NativeUringIoBackend.cpp — SubmitRead / SubmitWrite 末尾
::io_uring_submit(&m_ring);   // ← 每个 SQE 单独一次 syscall
```

每次 `SubmitRead` 或 `SubmitWrite` 都在末尾调用 `io_uring_submit()`，N 个 I/O = N 次 `io_uring_enter` syscall，与 epoll+read/write 的 syscall 数量相同，io_uring 的核心优势（批量合并）**完全未发挥**。

**asio_uring：攒满后一次提交（批量 = 本轮事件循环所有 I/O）**

```
SubmitRead / SubmitWrite
  └→ 仅构造 SQE 写入内存 SQ ring，零 syscall

ev_prepare (OnPrepare) — epoll_wait 之前
  └→ io_context.poll()
       └→ 一次 io_uring_enter 批量提交本轮所有 SQE
```

每次 `SubmitRead`/`SubmitWrite` 只把 SQE 写入内存 SQ ring（用户态操作），**不触发 syscall**。所有 SQE 积攒到 `ev_prepare` 回调时一次性 `io_uring_enter` 提交。100 个连接的读写 → 1 次 syscall。

### 2.2 CQE 收割效率

**native_uring：eventfd 触发，每次 peek 一个 CQE**

```
内核 CQE → io_uring 注册 eventfd 可读
  → epoll_wait 唤醒
  → ev_io OnEvfd 回调
  → ReapCqes() 循环 peek_cqe
```

eventfd 充当"通知铃"，libev 被唤醒后才批量 peek。但提交时已经是逐个 submit，延迟在提交侧而非收割侧。

**asio_uring：三路协同，提交与收割解耦**

```
第 1 路 ev_prepare  → 批量提交 SQE + 抢先收割已到 CQE
第 2 路 ev_io(ring_fd) → 内核完成后 epoll 边沿触发，排空整批 CQE
第 3 路 ev_check    → 补收 race window 内到达的 CQE
```

ring_fd 边沿触发保证一次唤醒排空整批 CQE；ev_prepare 抢先收割减少延迟；ev_check 兜底无遗漏。

### 2.3 为什么 HTTP 下 native_uring 甚至比 ev 差

ev 每次 I/O：1 次 `read` / `write` syscall  
native_uring 每次 I/O：1 次 `io_uring_submit` syscall + SQE 构造开销 + eventfd 路径

当批量=1 时，io_uring 的 SQE 内存写入、ring 指针更新、CQE 收割链路全是**额外开销**，没有任何收益，净亏。

### 2.4 为什么 HTTPS 下两者差距缩小

SSL 握手和 BIO 层每条 TLS record 产生多次小 read/write。native_uring 在这里**仍然每次单独 submit**，但 SSL 加解密的 CPU 开销（~0.5ms）远大于 syscall 差异，将两者的 syscall 代价都淹没了。因此 HTTPS 下 uring 系吞吐持平、延迟接近，只有 vs ev 的差距（批量收割减少 SSL BIO 排队）仍然显著（ev 803μs vs uring ~400μs）。

---

## 3. 设计对比

| 维度 | native_uring | asio_uring |
|------|-------------|------------|
| SQE 提交时机 | 每次 SubmitRead/Write 末尾立即提交 | ev_prepare 统一批量提交 |
| 批量大小 | 固定 = 1 | = 本轮事件循环内所有 I/O |
| syscall / 请求 | ~2（submit + 间接 enter） | ~1/N（N 个 I/O 共用一次 enter） |
| CQE 收割驱动 | eventfd → ev_io → peek loop | ring_fd 边沿触发 + ev_prepare + ev_check 三路 |
| 实现依赖 | liburing（自行管理 SQ/CQ） | ASIO io_uring_service（托管 SQ/CQ 生命周期） |
| 代码量 | 高（手动管理 PendingOp、ring 状态）| 低（ASIO 封装生命周期，shared_ptr + lambda） |
| send_zc 支持 | ✅（bounce + direct 两种模式） | ✅（FIXEDBUF 注册池） |

---

## 4. 结论

- **HTTP 场景**：native_uring 批量=1，syscall 数与 ev 相同但有额外开销，**比 ev 差 13%，比 asio_uring 差 14%**，没有使用价值。
- **HTTPS 场景**：SSL CPU 掩盖了 syscall 差异，native_uring 延迟略好于 asio_uring（8~29μs），无实用意义。
- **asio_uring 的核心优势**：ev_prepare 批量提交将 N 次 syscall 压为 1 次，HTTP 延迟比 ev 低 1.9×，比 Nginx 低 2.1×。
- **native_uring 的保留价值**：作为可插拔后端架构的演示，验证 `IoBackend` 接口可扩展性；不用于性能场景。
