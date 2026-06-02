# IoBackend 协议覆盖、场景适配与后续发展方向

> 2026-05-22 | 综合代码分析与性能数据

---

## 核心结论

> 如果你只读一段，读这里。

1. **协议透明** — IoBackend 没有 codec 过滤条件，`AddIoReadEvent`/`AddIoWriteEvent` 纯粹按 fd 排除 4 个特殊 fd（listen/Manager 通信）。因此 **asio_uring / native_uring 对 HTTP、HTTPS、WebSocket (JSON/PB)、内部 PB、APP 全部协议都生效**。

2. **HTTPS 例外仅限握手** — TLS 握手阶段有同步 `WriteFD` 绕过（`SSL_do_handshake` 需要同步双向 I/O）。握手完成后，数据 I/O 回归 IoBackend。短连接 HTTPS 不受 IoBackend 加速影响，长连接 HTTPS（WSS/HTTP2）充分受益。

3. **内部协议走 IoBackend** — Manager→Worker 通过 `send_fd_with_attr`/`recv_fd_with_attr` 传递的连接 fd，其数据 I/O 全部走 IoBackend。Manager 自身的 S2S 连接同样走 IoBackend。仅 `iManagerDataFd`/`iManagerControlFd` 通道自身使用 legacy ev_io。

4. **场景推荐有据可依**：

| 场景 | 推荐 | 三后端对比 (小包/大包) | 理由 |
|------|------|------------------------|------|
| API 网关（小包密集） | **ev** | ev 139k / asio 140k / native 125k | ev 和 asio 持平（+0.6%），native 亏 9.8% |
| 反向代理（大包+ZC） | **native_uring+zc** | ev 5.3k / asio 5.3k / native+zc 5.8k | native 大包 +9.3%，send_zc 独占 |
| 聊天服务（长连接混合） | **asio_uring** | ev 139k/5.3k / asio 140k/5.3k / native 125k/5.8k | 全场景不亏 + idle 友好（见 §3.3） |
| 通用混合流量 | **asio_uring** | 小包持平 ev，大包持平 ev | 唯一不亏的 io_uring 后端 |
| K8s 生产 | **ev** 或 **asio_uring** | — | ev 最稳，asio 需 seccomp profile |
| 零拷贝大文件 | **native_uring+ZC** | 独占 | send_zc 能力 asio 架构不支持 |

5. **两个 io_uring 后端是互补，不是竞争** — `AsioUringIoBackend` 是全场景 QPS 不亏的默认主力（ASIO 对象池消除 per-op 堆分配），`NativeUringIoBackend` 是 send_zc 专用加速器（per-op 堆分配导致小包亏 8%，但大包+ZC 独占）。消除 native 的 per-op 堆分配（P1 方向）后，两者可合并。

---

## 一、协议→IoBackend：全覆盖，三例外

### 1.1 架构分层

```
客户端请求
    ↓
Worker: AddIoReadEvent / AddIoWriteEvent
    ↓  判断 fd 是否排除 (仅 3 类特殊 fd)
    ↓  ┌─ 否 → IoBackend::SubmitRead / SubmitWrite  (asio_uring / native_uring / ev)
    ↓  └─ 是 → legacy ev_io  (libev ev_io watcher)
    ↓
CBuffer ReadFD / WriteFD
    ↓
mapCodec[eCodecType] → Decode / Encode  (协议层)
```

**核心结论：IoBackend 工作在 socket I/O 层，协议 codec 在上层。所有协议共享同一 SubmitRead/SubmitWrite 路径，无 codec 差异。**

### 1.2 仅 3+1 个特殊 fd 排除在 IoBackend 之外

| 排除 fd | 位置 | 原因 |
|---------|------|------|
| `m_iC2SListenFd` | Worker | 客户端监听 socket — SO_REUSEPORT 多进程 accept 分发 |
| `iManagerDataFd` | Worker | Manager→Worker 传递连接 fd（传入 `recv_fd_with_attr`） |
| `iManagerControlFd` | Worker | Manager→Worker 控制通道 |
| `m_iS2SListenFd` | Manager | 服务间监听 socket |

排除逻辑在 `Worker::AddIoReadEvent`（行 5034-5037）和 `Manager::AddIoReadEvent`（行 1756-1757）：
```cpp
if (m_pIoBackend
    && pConn->iFd != m_iC2SListenFd     // Worker
    && pConn->iFd != iManagerDataFd      // Worker
    && pConn->iFd != iManagerControlFd)  // Worker
// Manager 仅排除 m_iS2SListenFd
{
    return m_pIoBackend->SubmitRead(...);  // ← io_uring 异步 I/O
}
// else → legacy ev_io
```

### 1.3 9 种协议 codec × 3 种 IoBackend = 全覆盖

Worker 注册的 9 种 codec（`Worker::Init` 行 2316-2329）：

| codec 类型 | 编号 | 用途 | ev | asio_uring | native_uring |
|-----------|------|------|:--:|:--:|:--:|
| CODEC_PB_INTERNAL | 2 | 内部服务间通信（S2S） | ✅ | ✅ | ✅ |
| CODEC_HTTP | 3 | HTTP/1.1 客户端请求 | ✅ | ✅ | ✅ |
| CODEC_PRIVATE | 4 | 私有二进制协议 | ✅ | ✅ | ✅ |
| CODEC_WEBSOCKET_EX_JS | 5 | WebSocket JSON 帧 | ✅ | ✅ | ✅ |
| CODEC_WEBSOCKET_EX_PB | 6 | WebSocket Protobuf 帧 | ✅ | ✅ | ✅ |
| CODEC_TEST | 8 | 自定义测试协议 | ✅ | ✅ | ✅ |
| CODEC_APP | 9 | APP 客户端协议 | ✅ | ✅ | ✅ |
| CODEC_WEBSOCKET_EX_PB_APP | 10 | APP WebSocket PB | ✅ | ✅ | ✅ |
| CODEC_HTTPS | 11 | HTTPS (TLS) | ✅ | ✅ | ✅¹ |

> ¹ **HTTPS 注意**：TLS 握手阶段有同步 WriteFD 绕过（见 §1.4），握手完成后数据 I/O 回归 IoBackend。

### 1.4 HTTPS TLS 握手：唯一的同步 Write 绕过

```
HTTPS 连接建立流程：
┌─ TLS 握手 ─────────────────────────────────────────────────────┐
│  HttpsCodec::EncodeToConnection                                │
│    → SSL_do_handshake() → OpenSSL 产出响应字节到 pSendBuff       │
│    → Worker::RecvDataAndDispose (行 595-598)                   │
│    → 检测 CODEC_HTTPS && pSendBuff->ReadableBytes() > 0        │
│    → pSendBuff->WriteFD(fd, ...)  ← 同步写，绕过 IoBackend      │
│    → 原因：SSL_do_handshake 需要同步双向 I/O                    │
├─ 握手完成后 ────────────────────────────────────────────────────┤
│  EncryptPlain() → SSL_write() → 密文写入 pSendBuff              │
│  → 正常路径：AddIoWriteEvent → SubmitWrite → io_uring 异步写    │
└────────────────────────────────────────────────────────────────┘
```

**影响**：TLS 握手期间的同步写不受 IoBackend 加速。对于短连接 HTTPS（频繁握手），无论用 ev/asio/native 后端，握手性能一致。对于长连接 HTTPS（WebSocket over TLS），握手后数据 I/O 享受 IoBackend 加速。

---

## 二、内部协议是否走 IoBackend

**答案：是的，走 IoBackend。**

### 2.1 Worker 端（CODEC_PB_INTERNAL）

Worker 的 `iManagerDataFd` 接收 Manager 传来的连接 fd（带 codec 类型），此后该连接的数据 I/O 全部走 IoBackend：

```
Manager (send_fd_with_attr) → Worker (recv_fd_with_attr)
    → CreateAcceptFdAttr(fd, seq, codec)
    → AddIoReadEvent(pConn) → SubmitRead(fd, ...)
    → RecvDataAndDispose → mapCodec[CODEC_PB_INTERNAL]::Decode
    → ... 业务处理 ...
    → SendToClient → AddIoWriteEvent → SubmitWrite(fd, ...)
```

### 2.2 Manager 端（S2S 连接）

Manager 的 `AcceptServerConn` 接收其他服务节点的连接，同样走 IoBackend：

```
Manager::AcceptServerConn(m_iS2SListenFd)
    → CreateFdAttr(fd, seq)
    → AddIoReadEvent → SubmitRead(fd, ...)
    → RecvDataAndDispose → 直接 protobuf 解析
```

Manager 不使用 codec map，直接解析 protobuf 消息头+消息体（`MsgHead::ParseFromArray`）。所有 S2S 通信使用内部 protobuf 协议。

### 2.3 例外说明

仅以下 fd **不走** IoBackend：
- `iManagerDataFd` 自身（用于传递连接 fd 的控制通道）
- `iManagerControlFd` 自身（控制通道）

这两个 fd 用于 Worker 与 Manager 之间的控制消息和 fd 传递，使用 legacy ev_io。但通过它们**传递过来的业务连接 fd**，后续数据 I/O 全部走 IoBackend。

---

## 三、场景适配分析

基于 2026-05-22 同机测试数据（`why-asio-uring-is-primary.md` §一）：

```
┌─────────────────┬──────────────┬─────────────────┬──────────────────────┐
│      场景        │  最佳后端     │   大包性能       │   小包性能            │
│                  │              │   (64KB c50)     │   (~60B c100)        │
├─────────────────┼──────────────┼─────────────────┼──────────────────────┤
│ ev               │ ev           │   5,330 RPS      │   138,746 RPS         │
│ asio_uring       │ asio_uring   │   5,295 RPS      │   139,520 RPS         │
│ native_uring+zc  │ native_uring │   5,828 RPS      │   125,213 RPS (−9.8%) │
└─────────────────┴──────────────┴─────────────────┴──────────────────────┘
```

### 3.1 API 网关

| 特征 | 说明 |
|------|------|
| 包大小 | 请求头 ~200-800B，响应体 ~几百B-几KB |
| 连接模式 | 大量短连接或 keep-alive 长连接，c1M 问题突出 |
| 性能瓶颈 | 小包 I/O 密集 — 每个请求产生 1-2 次 read + 1-2 次 write |
| QoS 要求 | 低延迟（P99 < 10ms），高可用 |

**推荐：ev（小包最强：139k RPS）**

- ev 的 epoll + 同步 I/O 对小包场景确定性最高，无异步框架的额外开销
- asio_uring 小包持平 ev（+0.6%），若需要统一后端且能接受 ASIO 的依赖，可用 asio
- **native_uring 不推荐**：小包亏 8-10%，网关场景这是致命的

### 3.2 反向代理 / S2S 代理

| 特征 | 说明 |
|------|------|
| 包大小 | 混合：上行请求 ~几百B-几KB，下行响应 ~几KB-几MB（文件/流媒体） |
| 连接模式 | 长连接池（连接复用），连接数数十到数千 |
| 性能瓶颈 | 大包 I/O 吞吐 — 64KB+ 响应是常态 |
| 额外需求 | 可能需要零拷贝转发（代理不需要解析 body → sendfile/splice/zc） |

**推荐：native_uring（大包 +9.3%，且支持 send_zc）**

- 64KB 响应场景 native+zc 比 ev 提升 9.3%
- send_zc 对代理场景价值独特：代理转发时不需要 memcpy 数据到用户态再写回，直接 splice/send_zc 从上游 fd 到下游 fd
- 若代理还有大量小包健康检查、控制流，可混合部署：小包走 ev/asio 路径

### 3.3 聊天服务（WebSocket）

| 特征 | 说明 |
|------|------|
| 包大小 | 心跳 ~几十B；业务消息 ~几百B-几KB（文本）或 ~几KB-几百KB（图片/文件） |
| 连接模式 | 海量长连接（十万到百万级），90%+ 时间 Idle |
| 性能瓶颈 | Idle 连接管理（epoll/io_uring 的空闲开销）+ 突发消息的延迟 |
| 特殊需求 | WebSocket 帧编解码在上层 codec，I/O 层用标准 TCP |

**推荐：asio_uring（全场景均衡）**

- 聊天场景包大小分布极广，没有单一"最差"包大小 → asio 全场景不亏是关键
- **idle 友好**：聊天连接 90%+ 时间处于空闲等待状态。epoll 模型下，每个 idle 连接都注册在 epoll 实例中，idle 时无事件、无开销，但当有事件到达时需遍历就绪列表逐个处理。io_uring 模型更优：**所有连接共享一个 ring_fd**，无需 per-fd 的 epoll 事件注册——提交一次 `read` SQE 后，连接"注册"在 io_uring 的 SQ 中，内核有数据时直接填入 CQE，不存在 epoll 的 per-fd 注册/注销开销。连接数越多（十万→百万），这个差距越显著。
- **突发场景**（大量消息同时到达所有连接）：io_uring 的 SQ 批量提交（一次 `io_uring_enter` 提交一批 SQE）+ CQ 批量收割（一次 `poll` 收割一批 CQE）把 N 次系统调用压成 1 次；epoll 需要在事件循环中逐个 `read`/`write`，每个都是系统调用
- WebSocket 帧编解码在 codec 层完成，对 IoBackend 透明

### 3.4 asio_uring 为什么全场景不亏：三项结构优势

> 以下解释为什么 asio_uring 能做到小包持平 ev、大包不输 native，成为唯一全场景不亏的 io_uring 后端。
> 完整架构剖析（三路驱动、FdState 生命周期、WeakPtr 回收、Fixed Buffer 双路径）及与 nginx/envoy 横向对比见**独立文档** `docs/uring/asio-uring-internals-and-horizontal-comparison.md`。

**优势 1：零 per-op 堆分配 — ASIO 对象池**

```
native_uring 每次 I/O：new PendingOp() → 使用 → delete PendingOp()
asio_uring 每次 I/O：复用 ASIO 内部预分配的对象池 (recycling_allocator)
```

堆分配（`malloc`/`free`）每次耗费几十到几百纳秒。小包（~60B）整个 I/O 耗时仅 ~700ns，堆分配占比可达 10%+。ASIO 的对象池将 per-op 堆分配降为零——这是 asio 小包持平 ev、native 小包亏 8% 的根本原因。

**优势 2：批量系统调用 — SQ 攒批 + CQ 批量收割**

```
ev 模型：   每个连接有数据 → read() syscall（每次 1 个 syscall）
            每个连接写数据 → write() syscall（每次 1 个 syscall）

native_uring：每个操作 → io_uring_submit()（每次 1 个 syscall）

asio_uring： OnPrepare() 批量 flush   → 一次 io_uring_enter(提交 N 个 SQE)
             ev_io(ring_fd) 唤醒      → poll() 批量收割 N 个 CQE
```

asio_uring 不需要每操作一次 `io_uring_enter`——SQE 先写入共享内存的 SQ，在 `ev_prepare` 回调中一次提交。CQE 也一次收割。当连接数很大、并发操作密集时，批量提交/收割将每操作系统调用数从 1 降到 1/N。

**优势 3：单 ring_fd 等待 — 无 per-fd 事件注册**

```
epoll 模型：  epoll_ctl(ADD, fd1) → epoll_ctl(ADD, fd2) → ... → epoll_wait
io_uring 模型：sqe→sqe→sqe→... → 等 ring_fd 一个 fd → 收割 CQE
```

epoll 需要为每个连接 `epoll_ctl(ADD/MOD/DEL)` 管理事件。io_uring 只需向 ring 提交 SQE，内核有结果时写入 CQE，用户态等 `ring_fd` 一个文件描述符即可。连接越多，per-fd 注册开销的累积差异越大——这就是 **idle 友好** 的涵义：十万 idle 连接在 io_uring 下没有 per-fd epoll 管理开销，只需等待一个 ring_fd。

**与 native_uring 的关键差异**：

| 维度 | asio_uring | native_uring |
|------|-----------|-------------|
| per-op 堆分配 | ❌ 无（对象池） | ✅ `new PendingOp()` |
| 系统调用模式 | 批量 `io_uring_enter` | 每操作 `io_uring_submit()` |
| 收割路径 | ASIO `io_context::poll()` 直接调 lambda | hash map 查两次 + fd/seq 校验 |
| send_zc | ❌ 架构不支持 | ✅ 独占 |

### 3.5 场景推荐总结

```
场景                          推荐后端              理由
──────────────────────────────────────────────────────────────────
API 网关（小包密集）           ev 或 asio_uring      小包 ev 139k / asio 140k（持平），native 亏 9.8%
反向代理 / S2S（大包+ZC）     native_uring+zc       大包 +9.3% vs ev，ZC 独占
聊天服务（长连接+混合包）     asio_uring            全场景不亏 + idle 友好（ring_fd 单路等待）
混合通用流量                   asio_uring            默认安全选择（小包+0.6%, 大包−0.7% vs ev）
K8s 生产环境                   ev 或 asio_uring      ev 最稳，asio_uring 需 seccomp profile
零拷贝大文件传输               native_uring+ZC       唯一支持 send_zc
```

---

## 四、后续发展方向

### 方向 1：消除 native_uring 的 per-op 堆分配（短期，高收益）

**问题**：native_uring 的 `PendingOp` 每次 I/O 都 `new`/`delete`，小包场景固定开销占比 8%。

**方案**：引入对象池（类似 ASIO 的 `recycling_allocator`），预分配 `PendingOp` 池，用完归还。预期小包 QPS 提升 5-10%。

**工作量**：~100 行代码改动，低风险。

### 方向 2：asio_uring 支持 send_zc（中期，高收益但高难度）

**问题**：ASIO 的 `async_write_some` 使用 `IORING_OP_WRITE`，架构上不支持 `MSG_ZEROCOPY` 的双 CQE 模型。

**方案**：
- 路径 A：在 ASIO 的 `io_uring_service` 层扩展，新增 `async_write_some_zc` 操作类型，返回一个带 NOTIF CQE 的 completion handler
- 路径 B：在 `AsioUringIoBackend` 中绕过 ASIO 的 write，对大包直接用 `io_uring_prep_send_zc`，手写 CQE 收割（类似 native_uring 的 write 路径），但复用 ASIO 的 ring 管理

**挑战**：ASIO 的 completion 模型是单 CQE（`op → lambda`），ZC 需要双 CQE（结果 + NOTIF）。路径 B 更可行但会部分破坏 ASIO 抽象。

**收益**：asio 作为默认后端 + send_zc → 覆盖所有场景。

### 方向 3：固定缓冲注册（fixed buffers）推广（中期）

**当前状态**：asio_uring 已支持 `THUNDER_ASIO_URING_FIXEDBUF=1` 环境变量启用固定缓冲注册。native_uring 未支持。

**方案**：
- native_uring 增加 `IORING_REGISTER_BUFFERS` 支持，避免 per-op 的 `get_user_pages` 开销
- 系统化验证：固定缓冲 + send_zc 的组合收益（预期 64KB 场景额外提升 5-10%）

### 方向 4：io_uring 生产就绪（中长期，运维向）

**当前问题**：
- io_uring 需要 `CAP_SYS_NICE`（SQPOLL）或更高权限
- 某些内核版本有已知 bug（如 5.10 的 CQE 乱序问题）
- 缺乏生产环境灰度数据

**方案**：
1. **Seccomp profile**：为 asio_uring/native_uring 编写最小化 seccomp 规则，仅开放 `io_uring_enter`/`io_uring_register`/`io_uring_setup` 系统调用
2. **内核版本检查**：启动时检查 `/proc/version`，对已知有 bug 的内核版本（<5.15）强制回退到 ev
3. **灰度部署**：先在内部测试环境开 asio_uring，收集 QPS/延迟/错误率数据，确认无回归后再推生产

### 方向 5：多协议压测补齐（短期，验证向）

**当前覆盖面**：仅 HTTP Echo 场景（CODEC_HTTP）有完整的三后端对比数据。

**需要补齐**：
- WebSocket (CODEC_WEBSOCKET_EX_JS/PB)：长连接 + 心跳场景，验证 idle 连接开销
- PB Internal (CODEC_PB_INTERNAL)：内部 S2S 场景，验证 64KB+ protobuf 消息
- HTTPS (CODEC_HTTPS)：验证 TLS 握手阶段的 WriteFD 绕过是否成为瓶颈

### 方向 6：DPDK 后端补齐（长期，高门槛）

**当前状态**：`IoBackend.hpp` 接口声明支持 DPDK（`Name()` 可返回 `"dpdk"`），但实现尚未完成。

**场景价值**：裸金属高性能网关（~1M+ QPS per core），绕过内核网络栈。

**工作量**：大（~1000+ 行代码），需要 DPDK 环境。

---

### 优先级排序

```
优先级  方向                                   影响面        工作量
────────────────────────────────────────────────────────────
 P0     方向 5：多协议压测补齐                   验证全场景    小（~2天）
 P1     方向 1：消除 native per-op 堆分配        小包 +5-10%   小（~1天）
 P2     方向 4：io_uring 生产就绪                可上线         中（~1周）
 P3     方向 3：固定缓冲注册 (native)            大包额外提升   中（~3天）
 P4     方向 2：asio_uring 支持 send_zc          统一后端+ZC    大（~2周+）
 P5     方向 6：DPDK 后端                        十倍性能提升   大（~月级）
```

---

## 五、核心结论

### 结论 1：协议透明 — 所有 codec 走同一 IoBackend 路径

`AddIoReadEvent`/`AddIoWriteEvent` 中不检查 `eCodecType`，不区分协议。IoBackend 工作在 **socket I/O 层**，codec 编解码在上层。因此 **asio_uring/native_uring 对所有协议都生效**——HTTP、HTTPS、WebSocket (JSON/PB)、内部 PB、APP 协议无一例外。

```
IoBackend::SubmitRead(fd, buf, seq) → CBuffer 收到原始字节
    → mapCodec[eCodecType]::Decode → 协议解析
                        ↓
                  业务逻辑
                        ↓
    → mapCodec[eCodecType]::Encode → CBuffer 产出响应
IoBackend::SubmitWrite(fd, buf, seq) ← 原始字节写回 socket
```

### 结论 2：仅 4 个特殊 fd 不走 IoBackend

| fd | 位置 | 原因 |
|----|------|------|
| `m_iC2SListenFd` | Worker | 客户端监听，多进程 SO_REUSEPORT accept |
| `iManagerDataFd` | Worker | Manager→Worker 传递连接 fd 的控制通道 |
| `iManagerControlFd` | Worker | Manager→Worker 控制消息通道 |
| `m_iS2SListenFd` | Manager | 服务间监听 |

这 4 个 fd 使用的都是低频率控制操作，不构成性能瓶颈。其余所有业务连接 fd（无论何种协议）**100% 走 IoBackend**。

### 结论 3：HTTPS TLS 握手是唯一的同步 Write 绕过

TLS 握手要求同步双向 I/O（`SSL_do_handshake` 产出的响应必须立即发出，否则会中断握手状态机），因此在 `RecvDataAndDispose` 中检测 `CODEC_HTTPS && pSendBuff 有待发数据` 时使用 `WriteFD` 同步写。

**影响边界：仅握手阶段。** 握手完成后，`EncryptPlain → BIO_read(pWriteBio) → pSendBuff` 产出的密文数据通过 `AddIoWriteEvent → SubmitWrite` 走 IoBackend 正常异步写路径。

**实际影响**：短连接 HTTPS（每次请求一次握手）握手性能不由 IoBackend 决定。长连接 HTTPS（WebSocket over TLS、HTTP/2 连接复用）握手后数据 I/O 充分受益于 IoBackend 加速。

### 结论 4：内部协议走 IoBackend — Manager→Worker 传递的连接同样受益

```
客户端请求 → Manager accept (access_port, access_codec=CODEC_HTTP)
    → Manager: send_fd_with_attr(iDataFd, client_fd, ..., CODEC_HTTP)
        → Worker: recv_fd_with_attr(iManagerDataFd, ..., &iCodec)
            → CreateAcceptFdAttr(fd, seq, CODEC_HTTP)
            → AddIoReadEvent → SubmitRead(fd, ...)   ← IoBackend
            → AddIoWriteEvent → SubmitWrite(fd, ...)  ← IoBackend
```

Manager 自身的 S2S 连接同样：
```
服务节点 A → Manager accept (inner_port, CODEC_PB_INTERNAL)
    → CreateFdAttr → AddIoReadEvent → SubmitRead  ← IoBackend
    → AddIoWriteEvent → SubmitWrite               ← IoBackend
```

**注意**：`iManagerDataFd`/`iManagerControlFd` 自身不走 IoBackend，但通过它们传递的**业务连接 fd**，后续数据 I/O 全部走 IoBackend。

### 结论 5：场景推荐有据可依

基于同机三后端性能数据（`why-asio-uring-is-primary.md` §一）：

```
场景                     推荐后端             关键指标
─────────────────────────────────────────────────────────────────
API 网关（小包密集）       ev 或 asio_uring     小包 ev 139k / asio 140k（持平），native 亏 9.8%
反向代理（大包+ZC）       native_uring+zc      大包 +9.3% vs ev，ZC 独占
聊天服务（长连接混合）    asio_uring           全场景不亏 + idle 友好（ring_fd 单路等待，见 §3.3）
混合通用流量               asio_uring           默认安全选择（小包+0.6%, 大包−0.7% vs ev）
K8s 生产                   ev 或 asio_uring     ev 最稳，asio_uring 需 seccomp
零拷贝大文件传输           native_uring+ZC      唯一支持 send_zc，独占能力
```

**关键判断逻辑：**

- **小包性能是否最重要**（网关类）→ ev > asio_uring >> native_uring
- **大包吞吐是否最重要**（代理/文件类）→ native_uring+zc > native_uring ≈ asio_uring > ev
- **包大小不可预测**（通用/聊天类）→ asio_uring（唯一全场景不亏的 io_uring 后端）
- **需要零拷贝**（sendfile 替代）→ 仅 native_uring

### 结论 6：两个 io_uring 后端的定位不是竞争，是互补

```
AsioUringIoBackend  →  默认主力（全场景 QPS 不亏）
                         └─ ASIO 对象池 + 批量提交 = 零 per-op 堆分配
                         └─ 不支持 send_zc（ASIO 架构限制）

NativeUringIoBackend →  send_zc 专用加速器
                         └─ per-op 堆分配导致小包亏 8%，不适合默认
                         └─ send_zc 独占能力，大包 +9.3%
                         └─ P1 方向：消除 per-op 堆分配后，可挑战默认地位
```

---

*数据与代码引用：*
- `code/Net/src/labor/Worker.cpp` — `AddIoReadEvent`(行 5030), `AddIoWriteEvent`(行 5075), `RecvDataAndDispose`(行 551), `InitClientListener`(行 2407)
- `code/Net/src/labor/Manager.cpp` — `AddIoReadEvent`(行 1752), `AcceptServerConn`(行 266), `RecvDataAndDispose`(行 285)
- `code/Net/src/codec/HttpsCodec.cpp` — `EncodeToConnection`(行 98), `EncryptPlain`(行 372)
- `code/Util/src/util/StreamCodec.hpp` — `E_CODEC_TYPE` enum(行 16-28)
- `docs/uring/why-asio-uring-is-primary.md` — 同机三后端性能对比数据
