# Thunder io_uring 优化路线与后端选型

## 结论先行

**能不能做？** 能。5 项核心优化（Fixed Buffers / SQPOLL / Provided Buffers / MSG_ZEROCOPY send / native accept-connect）已分析过，内核（7.0）与 liburing（2.14）层面全部支持，技术上都可落地。

**要做哪些？** 按收益和风险排序，建议做：

1. **K8s seccomp 就绪**（部署前置，必做）
2. **Fixed Buffers**（数据路径根本优化，改动不大，零选型风险）
3. SQPOLL、native accept/connect（独立项，按需）
4. send_zc（依赖 Fixed Buffers）
5. Provided Buffers（架构岔路，最后评估）

**用 Asio 还是原生 liburing？** 当"更快的 epoll"用，Asio 更省心；要榨干 io_uring 高端能力，原生结构上更优——因为 SQPOLL/Provided Buffers/send_zc 这 3 项恰是 Asio 架构性锁死的。建议先做零选型风险的项，用实测数据再定。

---

## 5 项优化一览

| 优化 | 能否做 | 改动量 | 收益 | 一句话方案 |
|------|--------|--------|------|-----------|
| **Fixed Buffers** | ✅ | 中，不改 Asio | **中-高**：省内核 pin/unpin，大包尤明显，是 send_zc 前置 | Asio 原生支持；Thunder 把 per-conn 动态缓冲改成固定注册池 |
| **MSG_ZEROCOPY send** | ✅ | 中，需 patch Asio | **高（仅大包写）**：直击 64KB 写 memcpy 瓶颈；小包反而负收益 | Asio 无 send_zc，patch 加零拷贝发送路径；依赖 Fixed Buffers |
| **SQPOLL** | ✅ | 小，需 patch Asio | **中**：消提交 syscall，小包高频明显；Thunder 已批量合并，边际打折 | Asio 硬编码 flags=0，patch 一处加 SQPOLL 标志；内核 7.0 免特权 |
| **Provided Buffers** | ⚠️ | 大，架构岔路 | **中-高**：recv 省一次拷贝、多连接内存效率高；但实现代价大 | Asio 完全无支持，需深度 patch 或 recv 侧绕开 Asio |
| **native accept/connect** | ✅ | 中，不改 Asio | **低**：非吞吐热路径，仅高频建连场景受益 | Asio 原生支持；建连从裸 syscall 搬进 Asio |

> Asio 是我们掌控的 pinned submodule，patch 可控；但 ②③⑤ 三项都要动它，是选型的关键信号。

---

## Asio 后端 vs 原生 liburing

**不是"原生更方便"——原生更难写对。** Thunder 早期手写过 `UringIoBackend`，因异步写与 codec 状态机交互复杂，写操作被迫退化为同步，已删除。Asio 的价值就是把 buffer 生命周期、取消、CQE 收割、async write 全部抽象掉了。

**但目标决定选型：**

- 只要"更快的 epoll"（Fixed Buffers + native accept/connect）→ **留 Asio**：少代码、少 bug、async write 免费。
- 要高端天花板（SQPOLL + send_zc + Provided Buffers）→ **原生结构上更优**：Asio 锁死这 3 项，继续用就是 patch 子模块 + fork，不如写专用原生后端"5 项生而有之"。
- 走原生的代价：要重新解决当年沉掉第一版的 async write / 取消 / buffer 生命周期。

---

## 部署前置（K8s，必做，与选型无关）

当前 io_uring 能在容器跑，靠的是 compose 里 `seccomp:unconfined`。**K8s 默认 seccomp 会封禁 io_uring 系统调用**，届时 Thunder 会静默降级到 ev（仅一行日志，无告警，生产无人会发现）。

必做两件：

- **G1**：提供自定义 seccomp profile 放行 io_uring 三个系统调用，K8s 用 `securityContext.seccompProfile: Localhost` 引用（替代 unconfined，多数 K8s 集群拒绝 unconfined）。
- **G2**：把静默降级改成显著告警（启动横幅 + 指标 + 健康检查可见）。

待定：K8s 下 io_uring 是强制（失败拒绝启动）还是尽力而为（降级 + 告警）。

---

## 推荐落地顺序

```
P0  既有《使用与原理分析》bridge→host 纠错（独立，零代码）
        │
Gate    K8s seccomp 就绪 + 降级可观测  ── 一切 io_uring 优化的前提
        │
①  Fixed Buffers（数据路径根本，零选型风险）──► send_zc（依赖①）
②  SQPOLL          ③ native accept/connect（独立，按需）
        │
        └──► Provided Buffers（①完成、实测后再决策走 Asio 还是原生）
```

**建议**：先做 Gate + Fixed Buffers，拿实测数据，再决定是否为 SQPOLL/send_zc/Provided Buffers 走「留 Asio 增量 patch」还是「重写原生后端」。

---

*v2.0 — 2026-05-16｜配套《Thunder_io_uring使用与原理分析.md》（原理）*
*仓库：https://github.com/chenjiayi0603/thunder*
