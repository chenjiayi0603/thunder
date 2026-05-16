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
| **native accept/connect** | ✅ | 中，不改 Asio | **低**：非吞吐热路径，仅高频建连场景受益 | Asio 原生支持；建连从裸 syscall 搬进 Asio |
| **Provided Buffers** | ⚠️ | 大，架构岔路 | **中-高**：recv 省一次拷贝、多连接内存效率高；但实现代价大 | Asio 完全无支持，需深度 patch 或 recv 侧绕开 Asio |

> Asio 是我们掌控的 pinned submodule，patch 可控；但 ②③⑤ 三项都要动它，是选型的关键信号。

### 关于 send_zc 的「仅大包写」与按阈值分流

零拷贝发送省的是那次 memcpy，代价是一组**与包大小无关的固定开销**：

```
普通 send：用户 buffer ─[memcpy]→ 内核 socket buffer → 网卡
           发完即可复用，一个 CQE

send_zc：  内核 pin 住用户页 → 网卡直接 DMA 用户内存
           发完前 buffer 不可动 → 两个 CQE（提交 + NOTIF 才可复用）
```

| 固定开销 | 说明 |
|---------|------|
| 页 pin/unpin | 内核 `get_user_pages` 锁住用户页（防止 DMA 期间被换出/释放），发完再解锁 |
| 双 CQE | 普通 send 一个完成事件就够（"发完了，buffer 可复用"）。send_zc 把"内核已接收请求"和"网卡真正发完、用户页已释放"拆成**两个 CQE**：第一个表示已提交，第二个 NOTIF 才代表 buffer 可安全复用。应用必须等到第二个才能动 buffer → 每次发送要处理两个完成事件，收割逻辑翻倍 |
| 零拷贝记账 | 普通 send 把数据拷进内核，skb 自己持有数据，发完即丢。send_zc 不拷，内核要给每个 skb 挂一个指回用户页的通知对象、做引用计数，发完后顺着它触发 NOTIF 并解锁页 → 这套 per-skb 状态机和引用计数是普通 send 没有的额外开销 |

小包（如 37B）省下的 memcpy 几乎免费（cache 内、个位数纳秒），却要付上面三笔固定开销 → **净亏**。Linux 内核文档明确 `MSG_ZEROCOPY` "不是免费午餐"，经验阈值约 **~10KB 以上才划算**。

**实现要求**：send_zc **必须按 buffer 大小分流**——超过阈值（建议 ~16KB，可配）走 `prep_send_zc`，小包走普通 `prep_send`。不可无脑全用，否则小包路径净劣化。这也是表中标「仅大包写」的原因。

### 关于接收侧零拷贝（zcrx）为何不做

接收侧也有零拷贝（zcrx，对应 send_zc 的反方向）：

```
普通 recv：NIC → 内核 skb → [copy_to_user] → 你的 buffer   ← 这次拷贝是成本
zcrx：     NIC 把包头给内核走协议栈，包体直接 DMA 进你预注册的用户内存
           CQE 告诉你数据落在区域哪个位置，无 copy_to_user
           用完通过 refill ring 把 buffer 还给内核
```

不做的三个理由（任一即足够）：

**1. send/recv 所有权不对称 → recv 侧耦合极重**

| | send_zc | zcrx（recv） |
|--|---------|-------------|
| buffer 谁拥有 | 你（pSendBuff） | 内核/NIC（它挑哪块放数据） |
| 生命周期谁控 | 你：NOTIF 前不复用 | 内核：用完 refill 还回 ring |
| 数据布局 | 你定，codec 原样产出 | 内核选，可能每次不同块、跨块不连续 |
| codec 影响 | **零** | **必须重构**：从内核选的非连续池缓冲解析 + 跨块重组 |

send 你产数据你说了算；recv 数据何时来、落哪块，内核说了算。recv 侧零拷贝必然把"内核选的非连续池缓冲"灌进 codec，是比 send 更狠的数据面重构。

**2. 容器不友好（已用本机事实坐实）**

zcrx 要内核 ≥6.11 + 网卡支持 header/data split + 特定驱动。容器只锁用户态镜像，锁不住宿主机内核与网卡。本机实测：内核 7.0 ✅、liburing 2.14 ✅，但有线网卡 `e1000e`、WiFi `mt7921e` **均不支持 zcrx**；且本机是 K8s 节点（`flannel.1`/`cni0`/`veth*`），容器流量全走虚拟接口，根本不经物理网卡 → zcrx 连应用机会都没有。K8s 跨节点内核/网卡异构，更不可靠。

**3. 客户端面向路径瓶颈在发送（但 S2S 路径接收可能重）**

| 路径 | 接收 | 发送 | 实测/性质 |
|------|------|------|----------|
| 客户端面向（HelloHttp 等） | 小请求（37B~几KB JSON option），纳秒级可忽略 | 大响应（64KB），≈640MB/s memcpy = 那道墙 | 实测 64KB RPS 墙在 send（§5.6） |
| S2S 节点间（Interface↔Logic、转发/聚合/同步） | **可能大**（收大结果集/大转发体），架构内在、非假设 | **可能大**（发大包给对端） | **无压测数据**，recv 变重合理 |

请求/响应服务天然"问得小、答得大"，客户端路数据面成本压在发送侧；但 Thunder 是分布式集群框架，**S2S 是一等公民路径，其接收侧完全可能大**——此前结论"接收可忽略"只对客户端路成立，已修正。

> **关键**：是否做 recv 侧零拷贝，**不取决于 recv 大不大**。三个否定理由里只有「优化非瓶颈」会被 S2S 重场景削弱；另两个（容器不友好、Asio 零支持 Provided Buffers = 架构岔路 + codec 重构）**路径无关，照样成立**。即修正理由：不是"recv 不重要所以不做"，而是**"即便 S2S 让 recv 变重，recv 侧零拷贝的可行性硬伤依然在，代价仍不划算"**。
>
> 补：S2S 大包是双向的，**send_zc 在 S2S 发送方向同样吃得到**，故 send_zc 在 S2S 重场景价值更大、仍是性价比最高项。

**结论**：recv 侧零拷贝（⑤ Provided Buffers / zcrx）= 重耦合 + 容器不友好（路径无关）+ Asio 零支持。即便 S2S 让接收变重也够不着，落地仍是被挂起的架构岔路。真要管须先**实测 S2S 链路**确认 recv 拷贝是瓶颈（当前无数据），再评估。**现阶段只做 send_zc，接收侧维持现状。**

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

## 决策已定：走 Path B（原生 NativeUringIoBackend）

经分析定论，**不再走「留 Asio 增量 patch」**，原因已坐实：

- Asio 架构性锁死 SQPOLL/send_zc/Provided Buffers；patch 子模块还撞上 **asio 子模块未初始化**，补丁不进版本控制、`submodule update` 即失（Task 2 的 SQPOLL patch 实证）。
- A1（Asio 注册池 + 边界拷贝）= 把拷贝从内核挪到用户态，**净收益≈0**，非真零拷贝。
- 真零拷贝只有 send_zc；其前置不是 A1，是「buffer 持有到 NOTIF」的生命周期——Thunder **已有** `pSendBuff`/`pWaitForSendBuff` 双缓冲可承接。

### 拱心石结论（已读码坐实）

socket IO 完成**不经 StepCo20/Awaitable 协程**，是纯 C 回调：`backend → Worker::OnIoComplete(fd,seq,IoOp,result,ud)`（`IoBackend.hpp:39` 函数指针，`Worker.cpp:1060`）→ `HandleIoRead/WriteComplete`。Step 协程只在 Read 完成**下游**(codec decode 后)被驱动。

→ **send_zc 的双 CQE 不需要协程、不需要 awaiter、不碰 codec/ev**：只是同一条 C 回调桥上**多一个事件类型** + 拆一个写完成函数。风险等级远低于"重写状态机"。

### Path B 主线（旧 Asio 任务 A1/SQPOLL-patch 作废，思路保留）

```
1. IoOp::WriteNotif + 契约/Worker 写完成分发拆分
     IoOp { Read, Write, WriteNotif }；OnIoComplete 加分支；
     HandleIoWriteComplete 拆「字节记账(Write)」+「回收 pSendBuff/倒 pWaitForSendBuff/重提(WriteNotif)」；
     per-op 标志区分 普通 send / zc send；ev 与普通 send 行为不变。
     —— 小、安全、可独立编译验证，先做。

2. NativeUringIoBackend 骨架
     自管 SQ/CQ；libev 单线程驱动收割（接 ev_prepare/ev_check/ev_io(ring_fd)，
     绝不 thread-per-op，io_engine_uring.cpp 的反面教材）；SQPOLL flag 内建；
     ev 始终保留作回退（io_backend 配置三档：ev / asio_uring / native_uring）。

3. send_zc
     prep_send_zc 直发 pSendBuff；收割循环按 IORING_CQE_F_MORE/F_NOTIF 分流：
     F_MORE → (fd,seq,Write,bytes)；F_NOTIF → (fd,seq,WriteNotif,0)；
     NOTIF 经 OnIoComplete→WriteNotif 门控 pSendBuff 回收；按大小阈值(~16KB)
     分流，小包走普通 send。

4. recv：普通 recv（真 recv 零拷贝 zcrx 受网卡硬件天花板，本部署不可行，已定论）。

5. 编译（-j1，强制）+ 单元 + E2E。

6. 压测对比 ev —— Path B 验收：原生+send_zc 必须对 64KB 发送有实测收益，
   否则按 Path B 自身逻辑回退默认 ev。
```

部署前置（Gate G1/G2，K8s seccomp + 降级可观测）与选型无关，仍必做，并入主线。

---

*v3.0 — 2026-05-16｜决策落定：走 Path B 原生 NativeUringIoBackend，记录拱心石结论(IO 完成非协程，send_zc=回调多一事件类型)与 6 步主线；旧 Asio 增量 patch 路线作废｜配套《Thunder_io_uring使用与原理分析.md》（原理）*
*仓库：https://github.com/chenjiayi0603/thunder*
