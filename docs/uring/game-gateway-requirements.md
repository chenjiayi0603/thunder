# Thunder 游戏 TCP 网关 — 需求文档

> **版本**: v1.0
> **日期**: 2026-05-18
> **状态**: 需求分析
> **依赖**: DPDK+mTCP 设计文档 v1.2（`docs/uring/DPDK+mTCP设计文档.md`）

---

## 1. 背景

### 1.1 场景描述

Thunder 作为游戏 TCP 接入网关，部署在自建机房/裸金属物理机上，负责：

```
GameClient ←TCP→ Thunder(GameGateway) ←→ GameServer(Logic)
  自定义二进制协议    |                         |
  高频小包(< 100B)    | DPDK + mTCP 用户态 TCP   |
  C10K-C100K 长连接   | 低延迟 (< 1ms P99)        |
```

### 1.2 为什么 Thunder 适合做游戏网关

| Thunder 已有能力 | 游戏网关需求 | 匹配度 |
|-----------------|-------------|--------|
| Interface 网关节点 | 客户端接入层 | ✅ 已有 |
| CODEC_PRIVATE (CodecCustom) | 自定义私有二进制协议 | ✅ 已有 codec 类型 |
| IoBackend 抽象 | 可替换为 DPDK+mTCP | ✅ 已有接口 |
| Session/Step 模式 | 登录/鉴权/异步请求-响应 | ✅ 已有 |
| ConnectionAttr 连接管理 | 连接状态/心跳/超时 | ✅ 已有 |
| 插件系统 (.so) | 游戏逻辑热加载 | ✅ 已有 |
| C++20 协程 | 协程化业务代码 | ✅ 已有 |

### 1.3 现有能力缺口

| 缺失能力 | 优先级 |
|---------|--------|
| DPDK + mTCP I/O 后端实现 | P0 |
| 适配 DPDK 的 listen/accept/close 控制面 | P0 |
| 游戏二进制协议的高效帧编解码器 | P0 |
| 低延迟数据路径优化（避免内存分配） | P1 |
| 连接级别的优先级调度 | P1 |
| 游戏专用 metrics（帧率/延迟分位值） | P2 |
| 断线重连 + session 恢复 | P2 |

---

## 2. 功能需求

### FR1: DPDK + mTCP I/O 后端

**描述**：实现 `io_backend=dpdk` 模式，使用 DPDK PMD + mTCP 用户态 TCP 栈替代内核 TCP。

**验收标准**：
- `config.json` 中 `io_backend: "dpdk"` 可启动 Worker
- DPDK EAL 初始化成功（物理网卡绑 DPDK PMD、hugepages 分配）
- mTCP per-core context 创建成功
- `mtcp_socket/bind/listen/accept/read/write/close` 全链路可用
- DPDK 初始化失败时自动回退到 ev 后端

**依赖**：
- `code/Net/src/labor/DpdkContext.cpp`（新增）
- `code/Net/src/labor/DpdkIoBackend.cpp`（新增）
- `code/Net/src/labor/Labor.cpp` `InitIoBackend()` 新增 `dpdk` 分支

### FR2: 游戏二进制协议编解码器

**描述**：实现 `CODEC_GAME_BINARY` 类型编解码器，支持游戏常见的 TLV/固定头+可变体 二进制帧格式。

**帧格式设计**（参考 `CodecCustom` 扩展）：

```cpp
#pragma pack(1)
struct GameFrameHead
{
    uint8_t  magic;          // 魔数 (0xAB)
    uint8_t  version;        // 协议版本
    uint16_t cmd;            // 命令字 (区分消息类型)
    uint32_t seq;            // 序列号 (请求-响应对应)
    uint32_t body_len;       // body 长度 (不含头)
    uint32_t timestamp_ms;   // 客户端时间戳 (用于 RTT 计算)
};
#pragma pack()

struct GameFrame
{
    GameFrameHead head;      // 固定 14 字节头
    uint8_t       body[0];   // 可变长 body
};
```

**验收标准**：
- `E_CODEC_TYPE` 新增 `CODEC_GAME_BINARY = 12`
- `CodecGameBinary` 类实现 `ThunderCodec::Encode/Decode`
- 支持 `CODEC_STATUS_PAUSE`（半包等待）
- 支持 `CODEC_STATUS_ERR`（非法帧 → 断开连接）
- 单帧 decode/encode < 1μs（不含 protobuf，纯二进制拷贝）
- 通过 `mapCodec` 注册，与现有 HTTP/WebSocket 协议共存

**为什么不用 protobuf**：游戏高频小包要求固定头+可变体，protobuf 的 varint 编码和反射开销不适合 <100B 的包。`CodecCustom` 已有 `clientMsgHead` 作为自定义二进制头，`CodecGameBinary` 在此基础上扩展。

### FR3: 低延迟数据路径

**描述**：构建从 DPDK 收包 → 解码 → 业务处理 → 编码 → 发包的最小延迟路径。

**验收标准**：
- 单次 request → response 完整延迟（不含业务逻辑）< 50μs
- 零额外内存分配：CBuffer 复用 pRecvBuff/pSendBuff，不新建 string/protobuf
- 数据路径热缓存友好：帧头 + body 都在连续内存中
- IO 批量处理：`ProcessMtcpEvents()` 一次收割多个就绪事件

**关键优化点**：

| 优化 | 说明 |
|------|------|
| **CBuffer 栈复用** | `pRecvBuff` 写满后不 realloc，compact 后复用 |
| **帧头零拷贝解析** | `GameFrameHead*` 直接指向 CBuffer 内存，不 memcpy |
| **mTCP 批量 I/O** | `mtcp_epoll_wait` 一次收割 N 个就绪 fd，逐个处理 |
| **无中间格式转换** | binary codec 直接产出 `MsgHead/MsgBody`，不过 JSON |
| **无 protobuf** | 游戏协议不走 protobuf 序列化，直接用 struct + memcpy |

### FR4: DPDK 连接生命周期适配

**描述**：改造 Worker 中涉及内核 socket API 的 6 处代码，按 `io_backend` 分流。

**必须改的 6 处**（详见设计文档 7.4 节）：

| # | 位置 | 内核调用 | DPDK 替代 | 改动量 |
|---|------|---------|----------|--------|
| 1 | `InitClientListener` | `socket/bind/listen` | `mtcp_socket/bind/listen` | ~10 行 |
| 2 | `AcceptClientConn` | `accept()` | `mtcp_accept()` | ~15 行 |
| 3 | `DestroyConnect` | `close()` | `mtcp_close()` | ~5 行 |
| 4 | `SetSocketAttr` | `setsockopt()` | `mtcp_setsockopt()` | ~10 行 |
| 5 | `Fd2Address` | `getpeername()` | accept 时缓存 | ~5 行 |
| 6 | `mapFdAttr` key | 内核 fd | mTCP sockid (+偏移) | ~10 行 |

**验收标准**：
- 内核 socket 和 mTCP sockid 可在同一进程共存
- Manager IPC fd 继续走内核路径
- `DestroyConnect` 调用 `mtcp_close()` 而非 `::close()`，无资源泄漏
- fd 命名空间不冲突（mTCP sockid 偏移 10,000,000）

### FR5: 事件循环桥接

**描述**：libev 管理 timer/signal/admin，mTCP epoll 管理业务连接。通过 `ev_check` + `ev_idle` 桥接。

**实现**（与现有 `AsioUringIoBackend` 同模式）：

```cpp
void Worker::SetupDpdkBridge()
{
    ev_check_init(&m_checkWatcher, OnCheckDpdk);
    ev_check_start(m_loop, &m_checkWatcher);

    ev_idle_init(&m_idleWatcher, OnIdleDpdk);
    ev_idle_start(m_loop, &m_idleWatcher);
}

// ev_check: 非阻塞收割 (每次 epoll_wait 后)
void OnCheckDpdk() {
    dpdkBackend->ProcessMtcpEvents(/*timeout=*/0);
}

// ev_idle: 阻塞等待 + 驱动 mTCP 内部定时器 (空闲时)
void OnIdleDpdk() {
    dpdkBackend->ProcessMtcpEvents(/*timeout=*/1);  // 1ms
}
```

**验收标准**：
- libev timer 正常触发（业务超时、心跳检测）
- mTCP epoll 事件正常处理（读/写/错误）
- 两套事件循环不相互阻塞

### FR6: 游戏 session 管理

**描述**：扩展 `ConnectionAttr` 支持游戏 session 状态。

```cpp
// 扩展 tagConnectionAttr
struct tagConnectionAttr {
    // ... 现有字段 ...

    // 游戏 session 扩展
    uint64_t    ullPlayerId;       // 玩家 ID (登录后分配)
    uint32_t    uiLoginState;      // 0=未登录 1=登录中 2=已登录
    time_t      tLoginTime;        // 登录时间
    uint32_t    uiLastSeq;         // 上次处理的 seq (防重放)
    uint64_t    ullRoomId;         // 当前所在房间/场景
    uint32_t    uiLatencyMs;       // 最近 RTT (ms)
};
```

**验收标准**：
- 登录流程：client 发 LOGIN_REQ → gateway 验证 → 返回 LOGIN_RSP + 分配 playerId
- 断线检测：心跳超时 → `DestroyConnect` → 通知 Logic 玩家离线
- 重连流程：client 发 RECONNECT_REQ (带 token) → 恢复原 session

### FR7: 心跳与健康检测

**描述**：游戏连接需要双向心跳保活。

```
Client → Gateway: HEARTBEAT_REQ (每 5s)
Gateway → Client: HEARTBEAT_RSP (echo client seq)
Gateway 侧: 30s 无心跳 → 主动断开
```

**验收标准**：
- `IO_TIMER_CHECK` 复用现有超时检测机制
- 心跳超时断开连接，不阻塞其他连接处理
- 心跳包不计入业务 metrics

### FR8: 监控

**描述**：游戏网关专属 metrics。

| metric | 说明 | 采集方式 |
|--------|------|---------|
| `game_conn_active` | 活跃连接数 | mapFdAttr.size() |
| `game_conn_total` | 累计连接数 (含已断开) | counter |
| `game_msg_rx/tx` | 每秒收/发消息数 | counter |
| `game_msg_rx/tx_bytes` | 每秒收/发字节数 | counter |
| `game_latency_p50/p99` | 消息 RTT 分位值 | histogram |
| `game_frame_err` | 非法帧/解码失败计数 | counter |
| `game_login_succ/fail` | 登录成功/失败 | counter |
| `dpdk_rx/tx_burst` | DPDK 收发包批次 | DPDK xstats |
| `mtcp_conn_pool` | mTCP 连接池使用率 | mtcp stats |

---

## 3. 非功能需求

### NFR1: 性能

| 指标 | 目标 | 测量方法 |
|------|------|---------|
| PPS (小包 64B) | > 5M pps/核 | wrk + custom Lua |
| P99 延迟 | < 1ms (不含业务逻辑) | frame seq RTT |
| 最大并发连接 | > 500K (16GB RAM) | 连接数压测 |
| CPU 使用率 | 100% (轮询模式) | DPDK xstats |
| 新建连接速率 | > 50K CPS | TCP SYN 压测 |
| 零丢包 | 网卡 rx_missed_errors = 0 | DPDK ethtool stats |

### NFR2: 可靠性

| 指标 | 目标 |
|------|------|
| 崩溃恢复 | EAL 初始化失败 → 自动回退 ev (不 crash) |
| 内存泄漏 | 24h 压测后 RSS 增长 < 5% |
| 连接泄漏 | DestroyConnect 后 mTCP socket 完全回收 |

### NFR3: 运维

| 指标 | 目标 |
|------|------|
| 部署方式 | `config.json` 中 `io_backend: "dpdk"` 一键切换 |
| 日志 | 保持现有 log4cplus，新增 DPDK/mTCP 相关日志 |
| 排障 | 提供 `show game_conns` admin 命令查看当前连接列表 |
| 灰度 | `io_backend=dpdk` 和 `io_backend=ev` 可在不同 Worker 共存 |

---

## 4. 限制与风险

### 4.1 硬限制

| 限制 | 对策 |
|------|------|
| 必须独占物理网卡 | 至少 2 块网卡：管理口（ssh）+ 业务口（DPDK） |
| 必须 hugepages | 系统启动前配置，不可动态变更 |
| 必须 root/sudo | DPDK EAL 初始化需要 `CAP_SYS_ADMIN` |
| 不可容器化 | 只能用物理机/裸金属部署 |
| tcpdump/ss 不可用 | 用 DPDK pdump + 自建 admin 命令 |

### 4.2 技术风险

| 风险 | 影响 | 缓解 |
|------|------|------|
| mTCP 功能子集不全 | 可能缺某些 sockopt | 兼容降级：不支持的 option 忽略或 warn |
| mTCP 社区维护放缓 | bug 修不了 | 考虑 F-Stack (腾讯维护, FreeBSD TCP 更完整) 作为 fallback |
| DPDK 版本绑定 | 升级内核/网卡驱动后需回归 | 锁定 DPDK LTS 版本 |
| fd 碰撞 | mTCP sockid 与内核 fd 冲突 | sockid 偏移方案已纳入设计 |

### 4.3 不做的

| 项目 | 原因 |
|------|------|
| UDP/KCP 支持 | mTCP 仅支持 TCP；UDP 需额外方案（如 DPDK + 自研可靠 UDP） |
| 客户端 SDK | 客户端协议实现由游戏团队负责，网关只提供帧格式文档 |
| 广播/组播 | 游戏广播由 Logic 层实现，网关只负责单播转发 |
| protobuf 游戏协议 | 高频小包场景 protobuf 开销过大，统一用二进制 struct |

---

## 5. 实施计划

| 阶段 | 内容 | 依赖 | 工时 |
|------|------|------|------|
| **P1: DPDK 后端** | DpdkContext + DpdkIoBackend 实现 | DPDK/mTCP 库编译通过 | 3d |
| **P2: 控制面适配** | Worker 6 处 socket API 分流 | P1 | 2d |
| **P3: 事件循环桥接** | ev_check + ev_idle 钩子 | P2 | 1d |
| **P4: 游戏 codec** | CodecGameBinary + CODEC_GAME_BINARY | — (独立) | 2d |
| **P5: Session 扩展** | ConnectionAttr 游戏字段 + 登录流程 | P4 | 2d |
| **P6: 集成联调** | GameGateway 节点端到端 | P1-P5 | 3d |
| **P7: 性能调优** | PPS/延迟压测 + 火焰图分析 | P6 | 2d |
| **P8: 监控接入** | Metrics + admin 命令 | P6 | 1d |
| **合计** | | | **16d** |

> DPDK+mTCP 设计文档中 Phase 1-3（5d）对应上表 P1-P3；上表额外包含游戏协议和业务逻辑层（P4-P7, ~10d）。

---

## 6. 配置示例

```json
// deploy/GameGateway/conf/GameGateway.json
{
    "node_type": "GATEWAY",
    "io_backend": "dpdk",

    "dpdk": {
        "eal_args": "-l 0-3 -n 4 --proc-type=primary",
        "port_id": 0,
        "nb_rx_queues": 1,
        "nb_tx_queues": 1,
        "mbuf_pool_size": 65536,
        "lcore_id": 2
    },

    "mtcp": {
        "max_concurrency": 500000,
        "max_num_buffers": 500000,
        "rcv_buf_size": 8192,
        "snd_buf_size": 8192,
        "tcp_timeout": 30
    },

    "game": {
        "codec_type": "CODEC_GAME_BINARY",
        "heartbeat_interval_s": 5,
        "heartbeat_timeout_s": 30,
        "max_packet_size": 65536,
        "login_timeout_s": 10
    },

    "listen": {
        "ip": "0.0.0.0",
        "port": 9001
    }
}
```

---

## 7. 参考资料

- `docs/uring/DPDK+mTCP设计文档.md` — DPDK+mTCP 总体设计
- `docs/architecture/dpdk_analysis.md` — DPDK 需求论证
- `code/Util/src/util/StreamCodec.hpp` — E_CODEC_TYPE 枚举定义
- `code/Net/src/codec/CodecCustom.hpp` — 自定义二进制 codec 参考实现
- `code/Net/src/labor/IoBackend.hpp` — I/O 后端抽象接口
- `code/Net/src/labor/Worker.cpp` — Worker 连接生命周期
- mTCP: https://github.com/mtcp-stack/mtcp
- F-Stack: https://github.com/F-Stack/f-stack
