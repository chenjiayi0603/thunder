# Thunder io_uring 优化路线与后端选型

## 摘要

Thunder 当前用 standalone Asio 的 io_uring 后端（`AsioUringIoBackend`）。本文回答三个问题：

1. io_uring 的 5 项核心优化（Fixed Buffers / SQPOLL / Provided Buffers / MSG_ZEROCOPY send / native accept-connect）**能否实现、怎么实现**；
2. 继续用 **Asio 后端** vs 改用 **原生 liburing**，哪个更好；
3. 从 **Docker(host 网络) / 将来 K8s** 部署现实出发，推荐的落地顺序。

一句话定调：**io_uring 当"更快的 epoll"用，Asio 更省心；要榨干 io_uring 高端天花板，原生结构上更优**——因为 5 项里有 3 项恰是 Asio 架构性锁死的。

---

## 第一部分：平台与部署事实（核对结论的前提）

### 1.1 平台能力

| 项 | 值 | 含义 |
|----|----|----|
| 内核 | `7.0.0` | send_zc(≥6.0)、provided buffer ring(≥5.19)、SQPOLL 免 CAP(≥5.13) 全可用 |
| liburing | `2.14` | 最新，全特性支持 |
| `/proc/sys/kernel/io_uring_disabled` | `0` | io_uring 全允许，内核层不是任何一项的阻塞点 |

**结论**：5 项优化在内核层全部可用，瓶颈只在「Asio 代码覆盖」与「Thunder 集成」与「容器 seccomp 策略」。

### 1.2 部署现实（`deploy/docker/docker-compose.yml`）

| 配置 | 行 | 影响 |
|------|----|----|
| `network_mode: host` | :32 | **无 Docker bridge**，网络路径等价裸机。既有《使用与原理分析》§5.1「bridge 网络」措辞是事实错误，需单列纠错 |
| `security_opt: [seccomp:unconfined]` | :34 | io_uring 当前**靠关闭 seccomp 才能跑**。K8s 默认 seccomp 会封禁，是最大移植阻塞点 |
| `ulimits.memlock: 64MB` | :44 | Fixed Buffers 注册池是 pinned 内存，受此 + 各服务 cgroup memory 双重约束 |
| `deploy.resources.limits.cpus` | 各 service | SQPOLL 内核轮询线程自旋会吃 cgroup CPU 配额，容器下危害比裸机大 |

### 1.3 失败行为（`Labor.cpp:459-487`）

`asio_uring` Init 失败 → `LOG4_WARN("asio_uring init failed, falling back to ev")`（`Labor.cpp:470`）→ 起 `EvIoBackend`，不崩。

**真实风险**：K8s 默认 seccomp 下，Thunder 静默跑在 ev 上，所有 io_uring 收益归零，唯一证据是一行 WARN 日志，无指标无告警，生产无人会发现。

---

## 第二部分：Asio 后端 vs 原生 liburing

### 2.1 本质区别

| 维度 | Asio io_uring 后端 | 原生 liburing |
|------|-------------------|--------------|
| 代码量 | `AsioUringIoBackend` ~420 行，委托 SQE/CQE/seq/cancel/buffer 生命周期给 Asio | 全部手管，代码多、易错 |
| async write | Asio 已解决（`async_write_some` 内部处理完成与生命周期） | 须重新正确实现 |
| 与 libev 集成 | `io_context.poll()` 经 ev_prepare/check/ring_fd；有 NOP-SQE/`interrupt()` 副作用（曾致 ring_fd busy loop，即 GitHub issue #2 / fix #2） | 可写更可预测的 CQE 收割循环，无 NOP 副作用 |
| 依赖 | header-only，已 vendored（pinned submodule `bd500f0`，asio-1.38.0） | 仅 liburing 2.14（已具备） |

### 2.2 历史实证："原生不更方便"

Thunder 曾有手写 `UringIoBackend`，因**异步写与 codec 状态机交互复杂**，写操作被迫退化为同步 `send()`，后整体删除。这证明：原生 liburing **不是"更方便"，而是更难写对**。Asio 的真实价值在于把 buffer 生命周期 / 取消 / CQE 收割 / seq 管理 / async write 全部抽象掉了。

---

## 第三部分：5 项核心优化 — 能否实现 + 怎么实现

平台层全可行（见 1.1）。差异在 Asio 是否挡路：

| # | 优化 | 能否实现 | 怎么实现 | Asio 后端 | 原生 |
|---|------|---------|---------|----------|------|
| ① | **Fixed Buffers** | ✅ 不改 Asio | Asio 公开 `register_buffers()`（`io_uring_service.hpp:114`）；改 Thunder：`Manager.cpp:1933-1934` 的 `make_unique<CBuffer>()` per-conn 动态缓冲 → 固定注册池借/还 + 大包分片回退；池 ≤ memlock | ✅ 受 `is_single_buffer && is_registered_buffer`（`io_uring_socket_recv_op.hpp:69`）约束 | ✅ 无约束 |
| ② | **SQPOLL** | ✅ 需 patch Asio | patch `io_uring_service.ipp:532` `io_uring_queue_init(...,0)` → `io_uring_queue_init_params` + `IORING_SETUP_SQPOLL` + `sq_thread_idle`，config 门控；内核 7.0 免 CAP；`sq_thread_idle` 设小（如 100ms）防吃 cgroup CPU 配额 | ❌ flags 硬编码 0 | ✅ 一个 flag |
| ③ | **MSG_ZEROCOPY send** | ✅ 需 patch Asio | Asio 零 `send_zc`。patch send op 增 `io_uring_prep_send_zc` 路径 + 处理零拷贝**双 CQE**（completion + `IORING_CQE_F_NOTIF`，notif 前 buffer 不可复用）；依赖 ① 的稳定 buffer | ❌ 零支持 | ✅ 直接 `prep_send_zc` |
| ④ | **native accept/connect** | ✅ 不改 Asio | Asio 原生 `io_uring_prep_accept`（`io_uring_socket_accept_op.hpp:69`）/ `io_uring_prep_connect`（`io_uring_socket_connect_op.hpp:57`）；改 Thunder：`Manager.cpp:271` 裸 `accept()`、`Manager.cpp:1086` 裸 `connect()`（非阻塞+EINPROGRESS+libev）→ 搬进 `asio::ip::tcp::acceptor`/`socket` | ✅ 支持 | ✅ 直接 |
| ⑤ | **Provided Buffers** | ⚠️ 架构岔路 | Asio 全代码零 `IOSQE_BUFFER_SELECT`。两条路：(a) 深度 patch Asio recv op 加 buffer ring；(b) recv 侧自建 liburing 快路径绕开 Asio（write 仍 Asio） | ❌ 零支持 | ✅ 直接 buffer ring |

**小结**：①④ Asio 原生可达，只需改 Thunder；②③ 须 patch 我们掌控的 pinned submodule；⑤ 是真正的架构岔路。

---

## 第四部分：部署前置（K8s Gate，与选型无关，必做）

无论留 Asio 还是改原生，K8s 下都必须先解决 io_uring 可用性：

- **G1 — seccomp profile**：`deploy/` 提供放行 `io_uring_setup` / `io_uring_enter` / `io_uring_register` 的自定义 seccomp json；K8s 用 `securityContext.seccompProfile: type=Localhost` 引用，替代 compose 的 `seccomp:unconfined`（K8s `RuntimeDefault`/PodSecurity 多数集群拒 unconfined）。
- **G2 — 降级可观测**：`Labor.cpp:470` 静默 WARN → 升级为启动横幅 + 指标计数 + 健康检查可暴露，杜绝 K8s 下静默跑 ev 无人知。
- **待用户拍板**：K8s 下 io_uring 是**强制**（Init 失败拒绝启动）还是**尽力而为**（降级 ev + 响亮告警）？影响 G2 实现。

---

## 第五部分：选型结论与推荐路线

### 5.1 选型结论

| 目标定位 | 推荐后端 | 理由 |
|---------|---------|------|
| io_uring 作"更快 epoll"（仅 ①④）| **Asio** | 少代码、少 bug、async write 免费 |
| 榨干高端天花板（要 ②③⑤）| **原生 liburing** | Asio 架构锁死这 3 项；继续用 = ②③ patch 子模块 + ⑤ fork，不如专用原生后端"5 项生而有之" |

**决定性代价**：走原生须重新正确实现 async write / cancel / buffer 生命周期——正是当年沉掉第一版 `UringIoBackend` 的地方，也是当初选 Asio 的原因。

### 5.2 推荐路线与排序

```
P0  既有文档 bridge→host 纠错（独立，零代码，单列待办）
Gate G1+G2  seccomp 就绪 + 降级可观测  ── K8s 下一切 io_uring 优化的前提
        │
        ▼
① Fixed Buffers（Asio 原生，数据路径根本优化，零选型风险）──┬──► ③ send_zc（依赖 ①）
② SQPOLL（独立，提交侧）   ④ native accept/connect（独立）   │
        │                                                    ▼
        └────────────────────────────► ⑤ Provided Buffers（①③ 后用实测决策 A/B）
```

| 阶段 | 工作量 | 需 patch Asio |
|------|--------|--------------|
| Gate | 小-中 | 否（改 Labor 日志/指标 + deploy 配置）|
| ① Fixed Buffers | 中-大 | 否（公开 API）|
| ② SQPOLL | 中 | 是（小范围）|
| ③ send_zc | 中-大 | 是 |
| ④ accept/connect | 中 | 否（Asio 原生）|
| ⑤ Provided Buffers | 大 | 是（深度）/ 或绕开 Asio |

**建议**：先做 Gate + ①（Asio 原生支持、数据路径根本优化、零选型风险），用实测数据判断 64KB 数据路径是否仍是瓶颈，再决定是否为 ②③⑤ 走选型 **A（留 Asio 增量 patch）** 还是 **B（重写专用原生后端）**。

---

## 附录：关键源码索引

| 引用 | 位置 | 含义 |
|------|------|------|
| Asio register_buffers | `code/3party/asio/include/asio/detail/io_uring_service.hpp:114` | Fixed Buffers 公开 API |
| Asio fixed-buf 条件 | `code/3party/asio/include/asio/detail/io_uring_socket_recv_op.hpp:69` | `is_single_buffer && is_registered_buffer` 才走 `prep_read_fixed` |
| Asio queue_init flags=0 | `code/3party/asio/include/asio/detail/impl/io_uring_service.ipp:532` | SQPOLL 硬编码阻塞点 |
| Asio prep_accept | `code/3party/asio/include/asio/detail/io_uring_socket_accept_op.hpp:69` | native accept 原生支持 |
| Asio prep_connect | `code/3party/asio/include/asio/detail/io_uring_socket_connect_op.hpp:57` | native connect 原生支持 |
| Thunder 缓冲分配 | `code/Net/src/labor/Manager.cpp:1933-1934` | per-conn 动态 CBuffer（Fixed Buffers 改造点）|
| Thunder 裸 accept | `code/Net/src/labor/Manager.cpp:271` | Asio 外裸 accept() |
| Thunder 裸 connect | `code/Net/src/labor/Manager.cpp:1086` | Asio 外裸 connect()（非阻塞+EINPROGRESS）|
| 后端降级 | `code/Net/src/labor/Labor.cpp:459-487` | Init 失败静默降级 ev（G2 改造点）|

---

*文档版本：v1.0*
*创建：2026-05-16*
*配套：《Thunder_io_uring使用与原理分析.md》（原理）、本文（优化路线与选型）*
*项目仓库：https://github.com/chenjiayi0603/thunder*
