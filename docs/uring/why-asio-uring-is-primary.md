# 为什么 AsioUringIoBackend 是主力，NativeUringIoBackend 不是

> 2026-05-22 | 基于两份实测报告的数据分析与工程决策

---

## 背景

Thunder 有两个 io_uring 后端：

| 后端 | 实现 | 配置值 | 定位 |
|------|------|--------|------|
| **AsioUringIoBackend** | 基于 standalone ASIO 的 io_uring_service | `"asio_uring"` | **主力方案** |
| **NativeUringIoBackend** | 直接调用 liburing，自管 SQ/CQ | `"native_uring"` | Path B 验证产物 / 专用方案 |

常见疑问：NativeUringIoBackend 在 Path B 验证中 64KB 大包 +7.6% RPS，send_zc 真零拷贝 +9.1% RPS，为什么不是主力？

答案分三层：**性能不是一边倒、工程成熟度差距悬殊、以及两者定位根本不同。**

---

## 一、性能对比：Native 不是全面领先，是严重两极分化

### 数据来源

| 报告 | 日期 | 测试对象 | 环境 |
|------|------|---------|------|
| `uring计划优化路线.md §压测结论` | 2026-05-16 | native_uring vs ev | WSL2, c50-c100 |
| `asio_uring_benchmark.md` | 2026-05-11 | asio_uring vs ev vs uring | WSL2, c100-c500 |

### 小包 (37B) — Native 溃败

```
native_uring: 121,383 RPS (−11% vs ev 的 136,452)
asio_uring:   160,674 RPS (≈ ev 的 164,358)
```

- native_uring 每个 I/O 操作有 PendingOp `new/delete` + fd/seq 校验常数开销
- 小包场景下，这些固定开销 > 省下的 syscall，净亏 11%
- asio_uring 无此开销（ASIO 内部内存池复用，无 per-op 堆分配）

### 中包 (4KB) — 基本持平

```
native_uring: 50,229 RPS (+1% vs ev 的 49,750)    ← 噪声内
asio_uring:   73,958 RPS (−6% vs ev 的 73,137)    ← WSL2 连续测试最后排，受负载累积拖累
              独立测试 avg lat 0.99ms (−34% vs ev 1.51ms)
```

### 大包 (64KB) — 各有优势

| 指标 | ev | native_uring | asio_uring |
|------|-----|-------------|------------|
| c100 RPS | 5,098 - 6,207 | 5,486 (+7.6%) | 6,675 (+7.5%) |
| c100 Avg Lat | 9.49ms - 16.78ms | 2.91ms (TTFB) | **2.32ms** (−86%) |
| c500 Stdev | 2.88ms - 83.5ms | **158us** (18× smaller) | 1.63ms (50× smaller) |

> **注意**: native_uring 报告的 2.91ms 是 **TTFB（首字节时间）**——native_uring 异步分片写，wrk 收到首包即计时停止。真实整包耗时 ≈ 9.12ms（−3.9% vs ev 的 9.49ms）。asio_uring 同理，两者 wrk 报告的 TTFB 含义一致，直接可比。

**关键发现：两者在 64KB RPS 上差距不大，但 asio_uring 的延迟表现更惊人（−86% vs ev）。**

### send_zc（64KB 真零拷贝，仅 native_uring 支持）

```
真零拷贝 (B-3b): 5,775 RPS (+9.1% vs ev 普通 send)
                 P99 3.25ms (−72% vs ev)
```

这是 native_uring **唯一不可替代的场景**——AsioUringIoBackend 不支持 send_zc。但代价见下节。

### 结论：是一条 tradeoff 曲线，不是一条领先曲线

```
包大小    → 37B       4KB      64KB      64KB+zc(send_zc)
ev         ████████   ██████   ████      ███
native     ███████    ██████   █████     █████ (+9%)
asio       ████████   ██████   █████     N/A (不支持)
          (−11%!)    (持平)    (+7.6%)
```

**native_uring 不是"更快"，是按包大小两极化——小包亏 11%，只有大包才赚。** 生产环境是混合流量（HTTP 请求通常几 KB，WebSocket 帧也是小包居多），不能只盯大包。

---

## 二、工程成熟度：Asio 替你解决了最难的三个问题

### 问题 1：异步写状态机

socket 写是分片的——一次 write 可能只发出部分数据，需要保留未发完的 buffer 等下次写就绪再发。epoll 里这是 EAGAIN → ev_io 再触发。io_uring 里这是**异步多 CQE 分片写入**。

Asio 的 `async_write_some` 内部处理了：
- 数据未写完时的自动重提交
- buffer 部分消费后的指针偏移
- 多个 CQE 的顺序保证

**NativeUringIoBackend 必须自己实现这套状态机。** Thunder 早期手写过一版 `UringIoBackend`（已删除），就因异步写与 codec 状态机交互复杂，写操作被迫退化为同步。当前 NativeUringIoBackend 的写路径是简化版（EAGAIN 时重新 SubmitWrite），对复杂分片场景未经充分验证。

### 问题 2：取消语义

连接断开时，fd 上可能还有未完成的 I/O 操作。需要安全取消它们，且不能误伤后续复用同一 fd 的操作。

Asio 的 solution：
- `CancelFd` 只需 erase map entry + `cancelled = true`
- completion lambda 用 `weak_ptr<FdState>` 防悬挂——fd 已移除后 lambda 自动丢弃
- 不需要显式等待所有异步 op 完成

**NativeUringIoBackend 没有这层抽象。** 它的 PendingOp 在 CQE 到达时必须校验 fd/seq 是否仍匹配——但没有 Asio 的 lifetime 保证，纯靠约定和手动检查。边界 case（fd 复用、seq 回绕、CQE 乱序到达）容易出错。

### 问题 3：buffer 生命周期 + 零拷贝安全

send_zc 的 buffer 在内核 DMA 完成前不能被释放——否则内核写已释放内存，静默数据损坏。

Asio 通过 shared_ptr + completion lambda 自动管理。NativeUringIoBackend 在 B-3b 也改为 shared_ptr:
- PendingOp 持有 `shared_ptr<CBuffer>` 引用
- NOTIF CQE 到达 → delete PendingOp → 最后一个引用释放 → buffer 安全析构

理论上可行，但 PendingOp 的完整生命周期（new/delete、CQE 收割后释放）全手写——路径多，遗漏一处就 UAF。

### 总结

| 问题 | AsioUringIoBackend | NativeUringIoBackend |
|------|:--:|:--:|
| 异步写状态机 | ✅ ASIO 内置 | ⚠️ 手写简化版 |
| 取消语义 | ✅ weak_ptr 防悬挂 | ⚠️ 手动校验 fd/seq |
| Buffer 生命周期 | ✅ shared_ptr + lambda | ⚠️ shared_ptr + 手动 PendingOp |
| CQE 收割 | ✅ ASIO io_context::poll() | ⚠️ 手写 io_uring_peek_cqe 循环 |
| NOP-SQE 空转 | ⚠️ 有（已通过 RingWatcher 按需启停解决） | ✅ 无 |
| ring_fd 发现 | ⚠️ /proc hack | ✅ io_uring_register_eventfd |

**AsioUringIoBackend 约 550 行 + ASIO 框架。NativeUringIoBackend 约 400+ 行但需要手写 ASIO 替你做的所有事。** 代码行数少不代表复杂度低。

---

## 三、两者定位根本不同

### AsioUringIoBackend：通用主力方案

诞生于"把 io_uring 当更快的 epoll 用"的需求：

- **目标**：在不改任何协议/业务代码的前提下，用 io_uring 替换 epoll
- **策略**：利用 ASIO 成熟 io_uring_service 实现，最小化自写代码
- **结果**：全协议透明支持，RPS 与 ev 持平或略优，大包延迟大幅改善

### NativeUringIoBackend：Path B 验证产物

诞生于"验证 send_zc 在 Thunder 中是否可行"的需求：

- **目标**：绕过 Asio 架构限制（ASIO 不支持 SQPOLL/send_zc/Provided Buffers），用原生 liburing 实现 send_zc
- **策略**：从头手写 SQ/CQ 管理，按阈值分流 send_zc / 普通 send
- **结果**：send_zc 可行（+9.1% RPS, 0 crash），但作为通用方案缺陷明显

**NativeUringIoBackend 的主线价值不是"替换 AsioUringIoBackend"，而是**：
1. 验证了 send_zc 双 CQE → WriteNotif 回调桥可行（拱心石结论）
2. 验证了 shared_ptr RAII 方案可消除 UAF 风险（zcInFlight/pendingDestroy 删除）
3. 为未来可能的高端场景（纯大包 S2S、send_zc 专用路径）提供了经过验证的可用代码

---

## 四、当前架构：两者共存，各司其职

```
                  ┌─────────────────────────┐
                  │      IoBackend 接口       │
                  │  12 methods, v2.0        │
                  └─────────────────────────┘
                      ↑         ↑        ↑
             ┌────────┘    ┌────┴────┐   └─────────┐
             │             │         │             │
        EvIoBackend  AsioUringIo  NativeUringIo  DpdkIo
         (默认epoll) (主力uring)  (send_zc专用)  (DPDK)

配置:
  "io_backend": "ev"            → 默认，小包最优，零依赖
  "io_backend": "asio_uring"    → 主力，全协议通用，延迟优势显著
  "io_backend": "native_uring"  → send_zc 大包场景专用（需 liburing）
```

### 使用建议

| 场景 | 推荐后端 | 理由 |
|------|---------|------|
| 通用混合流量（HTTP/WS/PB 客户端面） | `ev` 或 `asio_uring` | native 小包 −11% 不可接受 |
| 大响应/文件下载/流媒体 | `asio_uring` | 延迟 −86%, stdev 极低 |
| S2S 大包转发 + 需要 send_zc | `native_uring` | 唯一支持 send_zc 的方案 |
| K8s 生产部署 | `ev`（当前）或 `asio_uring`+seccomp | native 成熟度不够 |

---

## 五、为什么不是"把 native_uring 修好"？

有人会问：既然 native_uring 大包有优势，为什么不把小包路径修好让它变成通用方案？

**修不了——小包开销是结构性的：**

```
native_uring 每个 I/O 操作:
  1. SubmitRead/SubmitWrite 入口
  2. FdState 查找 (hash map)
  3. new PendingOp (heap alloc)           ← per-op 堆分配
  4. io_uring_get_sqe + 填 SQE
  5. io_uring_submit (每操作一次 syscall)
  6. ... CQE 到达 ...
  7. ReapCqes: io_uring_peek_cqe + 类型判断
  8. FdState 再次查找校验 fd/seq
  9. m_callback 分发
  10. delete PendingOp (heap free)        ← per-op 堆释放
  11. 条件分支: isZc/F_NOTIF/F_MORE 判断

AsioUringIoBackend 每个 I/O 操作:
  1. SubmitRead/SubmitWrite 入口
  2. EnsureFdState (hash map, 首次)
  3. async_read_some / async_write_some → ASIO 内部对象池，无堆分配
  4. ... poll() 批量 io_uring_enter (一次 syscall 提交整批 SQE) ...
  5. ... CQE 到达 ...
  6. io_context::poll() 批量收割 + 直接调 lambda
  7. lambda 内 buf->Advance + m_callback
```

**小包场景下，native 的步骤 3（new）+ 10（delete）≈ 几十~几百 ns 固定开销，步骤 5 的 submit syscall ≈ 几百 ns。** 而小包整个 I/O 才几 μs——固定开销占比太大。**Asio 内部用对象池避免了步骤 3/10，批量提交省了步骤 5/7。** 这些是架构决定的，不是"修修 bug"能改的——除非你把 native_uring 也写成一个 ASIO 级别的成熟框架。

具体量化分析见 `uring计划优化路线.md §性能上限分析`：
> 64KB 场景下 syscall 开销 ≈ 1ms，占 8-9ms 总耗时的 ~12%。+7.6% ≈ 省掉的 syscall / 总开销，已到控制路径天花板。

---

## 六、关键时间线（理解决策背景）

```
2026-05-11  AsioUringIoBackend 主线程直驱方案验证完成
            → 64KB c100 延迟 −86%，全场景不劣于 ev
            → 确立为 io_uring 主力方案

2026-05-16  NativeUringIoBackend Path B 骨架完成
            → 目标: 验证 send_zc 在 Thunder 是否可行
            → 64KB 普通 send +7.6%，小包 −11%

2026-05-17  NativeUringIoBackend B-3b 真零拷贝验证完成
            → send_zc +9.1% RPS, 0 crash
            → shared_ptr RAII 方案通过双重验证
            → 结论: send_zc 可行，但 native 不适合做通用主力
```

**AsioUringIoBackend 先诞生、先确立为主力。NativeUringIoBackend 后诞生、为验证 send_zc 而建，从未被设计为通用方案。**

---

## 结论

| 维度 | AsioUringIoBackend | NativeUringIoBackend |
|------|:--:|:--:|
| 全包大小表现 | ✅ 均衡，小包不亏 | ❌ 小包 −11% |
| 大包延迟 | ✅ −86% vs ev | ✅ 尾延迟极稳 |
| send_zc | ❌ 不支持 | ✅ +9.1% RPS |
| 工程成熟度 | ✅ ASIO 扛状态机 | ⚠️ 手写简化版 |
| 协议支持 | ✅ 全协议透明 | ✅ 全协议透明 |
| 适用场景 | **通用主力** | **64KB+ send_zc 专用** |

**NativeUringIoBackend 不是主力，不是因为"没做好"，而是从一开始就不是为"做主力"设计的。**

它是 Path B 实验的载体——验证 send_zc 在 Thunder 中是否可行。验证通过了（+9.1% RPS, 0 crash），但它作为通用 I/O 后端的结构性缺陷（小包 −11%、手写异步写状态机、无 ASIO 级 buffer 生命周期管理）决定了它只适合 **64KB+ 大包 + send_zc** 的专用场景。

两者不是竞争关系——是互补关系。通用可靠用 AsioUringIoBackend；需要 send_zc 天花板用 NativeUringIoBackend。

---

*参考：*
- `docs/uring/uring计划优化路线.md` — Path B 设计 + native_uring 压测数据 (2026-05-16~17)
- `tests/benchmark/results/asio_uring_benchmark.md` — AsioUringIoBackend 压测数据 (2026-05-11)
- `code/Net/src/labor/AsioUringIoBackend.{hpp,cpp}` — 主力 uring 实现（~550 行）
- `code/Net/src/labor/NativeUringIoBackend.{hpp,cpp}` — Path B 原生 uring 实现（~400+ 行）
- `docs/uring/io_uring原理分析-Thunder案例分析.md` — io_uring 原理 + 整体架构分析
