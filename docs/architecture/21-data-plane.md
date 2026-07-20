# Thunder 数据面 — 网络 I/O · Raft 共识 · 共享内存路由

> 原 [`01-architecture-design.md`](01-architecture-design.md) 第 6-8 章独立拆分。适合理解请求如何从网卡流入、经编解码管道、通过 Raft 协调、写入共享内存分发给 Worker。

---

> ⚠️ **注意**: 第 7 章 Raft 共识描述的是旧 Center 集群模式。当前生产部署已迁移到 etcd 注册中心。Raft 章节保留作为架构演进参考，实际服务发现/配置推送见 [02-etcd-designed.md](02-etcd-designed.md)。

---

## 6. 网络 I/O 与编解码管道

### 6.1 读数据流程

```
epoll_wait → EV_READ
  │
  ▼
IoCallback → IoRead → RecvDataAndDispose
  │
  ├─ pRecvBuff->Compact(8192)      ← 释放已读空间
  ├─ pRecvBuff->ReadFD(fd, err)    ← 非阻塞 read
  │
  ▼
  while (ReadableBytes >= MsgHeadSize):
    ├─ codec->Decode(pConn, oMsgHead, oMsgBody)
    │   ├─ CODEC_STATUS_OK:
    │   │   ├─ Protocol message (cmd > 0) → Dispose(Step/Cmd)
    │   │   └─ HTTP/WS message → Dispose(HttpMsg)
    │   │       └─ Encode response → pSendBuff->WriteFD → Compact(8192)
    │   ├─ CODEC_STATUS_PAUSE: break  ← 数据不完整，等下次 EV_READ
    │   └─ CODEC_STATUS_ERR: DestroyConnect
    │
    ▼
  return true;

错误处理:
  read == 0        → 对端关闭 → DestroyConnect
  errno == EAGAIN  → 正常（非阻塞读空）
  errno == EINTR   → goto read_again（被信号中断，重试）
  errno == other   → DestroyConnect
```

### 6.2 编解码器体系

```
ThunderCodec (抽象基类)
  ├─ Encode(MsgHead, MsgBody) → CBuffer
  ├─ Decode(CBuffer) → MsgHead, MsgBody
  ├─ Decode(ConnectionAttr) → MsgHead, MsgBody  // 连接感知解码
  │
  ├─ ProtoCodec          ← S2S / 内部通信（MsgHead+MsgBody 二进制）
  ├─ HttpCodec           ← HTTP 请求/响应解析
  ├─ HttpsCodec          ← HTTPS（OpenSSL 握手 + HTTP）
  ├─ CodecWebSocketJson  ← WebSocket JSON 帧
  ├─ CodecWebSocketPb    ← WebSocket Protobuf 帧
  ├─ CodecWebSocketPbApp ← WebSocket Protobuf App 帧（带用户会话）
  ├─ ClientMsgCodec      ← 客户端私有协议
  ├─ AppMsgCodec         ← 应用层协议（带 auth verify）
  └─ CodecCustom         ← 自定义编解码器扩展
```

### 6.3 发送数据流程

```
SendTo(msgShell, MsgHead, MsgBody):
  1. 查找连接 fd
  2. pSendBuff->Write(MsgHead.SerializeAsString())
  3. pSendBuff->Write(MsgBody.SerializeAsString())
  4. pSendBuff->WriteFD(fd, err)     ← 立即尝试 write
  5. pSendBuff->Compact(8192)        ← 压缩缓冲区
  6. if EAGAIN: RefreshEvent(EV_WRITE)  ← 注册写事件
     if error: DestroyConnect
```

**关键优化**: 先尝试直接 write，失败才注册 EV_WRITE 事件。这种 "try-write-first" 模式避免了不必要的 epoll_ctl 系统调用。

### 6.4 压缩/加密管道

```
ThunderCodec 支持透明的压缩和加密：

编码路径:
  MsgBody.body  → [可选: Zip/Gzip 压缩] → [可选: RC5/AES 加密] → 组包 → CBuffer

解码路径:
  CBuffer → 拆包 → [可选: AES/RC5 解密] → [可选: Gunzip/Unzip 解压] → MsgBody.body

控制位（MsgHead.cmd）:
  gc_uiGzipBit (0x10000000)  ← gzip 压缩
  gc_uiZipBit  (0x20000000)  ← zip 压缩
  gc_uiRc5Bit  (0x01000000)  ← RC5 加密
  gc_uiAesBit  (0x02000000)  ← AES-128 加密
```

### 6.5 CBuffer 缓冲区设计

```
       +-------------------+------------------+------------------+
       | readed bytes      |  readable bytes  |  writable bytes  |
       +-------------------+------------------+------------------+
       |                   |                  |                  |
       0      <=      readerIndex   <=   writerIndex    <=    capacity
```

| 操作 | 说明 |
|------|------|
| `Compact(8192)` | 当可写空间不足时，释放已读空间。若空闲仍不足，malloc 新 buffer |
| 扩容策略 | 容量不足时 ×2 扩容（`newCapacity <<= 1`） |
| `BUFFER_MAX_READ` | 8192 字节，单次最多读取量 |
| `DEFAULT_BUFFER_SIZE` | 32 字节初始容量 |

---

## 7. Raft 共识与分布式协调

### 7.1 Raft 状态机

```
                    timeout, start election
     ┌──────────┐ ──────────────────────────► ┌───────────┐
     │ Follower │                             │ Candidate │
     │          │ ◄────────────────────────── │           │
     └────┬─────┘  discover current leader    └─────┬─────┘
          │         or higher term                  │
          │                                         │ receives votes from
          │ AppendEntries from leader               │ majority of servers
          ▼                                         ▼
     ┌──────────┐                             ┌──────────┐
     │ Follower │ ◄───────────────────────────│  Leader  │
     │ (normal) │    AppendEntries heartbeat  │          │
     └──────────┘                             └──────────┘
```

### 7.2 关键时间参数（SessionRaftCluster.cpp）

```
Follower Lease:
  base = center_beat × mult  (mult 从配置读取，默认值取决于同数据中心/跨数据中心)
  extra = 1.0 + U(0,1) × 0.5
  lease = base + extra

Candidate 选举重试:
  retry = 0.08 + U(0,1) × 0.12  (80ms~200ms random jitter)

冷启动随机延迟:
  delay = 0.20 + U(0,1) × 0.30  (200ms~500ms，避免同时启动多个选举)

心跳间隔:
  center_beat (配置项，通常 1~3 秒)
```

### 7.3 Raft 消息流

```
                              Leader                           Follower
                                │                                 │
  RaftTick (periodic timer)     │                                 │
    Leader: skip (no action)    │                                 │
    Follower: check lease       │                                 │
      if expired:               │                                 │
        RaftStartElection()     │  ── RequestVote ──────────────► │
        term++, votedFor=self   │  ◄── RequestVoteRsp ─────────── │
                                │                                 │
  RaftSendAppendEntriesToAll()  │  ── AppendEntries ────────────► │
    (Leader periodic)           │     {term, leaderId,            │
    node_id cursor merge        │      node_id_alloc_cursor,      │
    online node snapshot        │      prevLogIndex, prevLogTerm, │
                                │      entries[], leaderCommit}   │
                                │                                 │
                                │  ◄── AppendEntriesRsp ─────────  │
                                │     {term, success,             │
                                │      matchIndex}                │
```

### 7.4 Node ID 分配（环形合并算法）

```
Node ID 范围: 1 ~ 254 (255 个节点，0 为保留值)

分布式分配算法 (MergeNodeIdAllocRing):
  ┌───────────────────────────────────────────┐
  │         Node ID Ring (mod 255)            │
  │                                           │
  │    cursor_A ──► [ assigned_A ] ──►        │
  │    cursor_B ──► [ assigned_B ] ──►        │
  │                                           │
  │  Merge 规则:                               │
  │    每个节点维护自己的 cursor 和已分配集合    │
  │    Leader 收集所有 node 的 cursor          │
  │    取 max(cursor, 看到的最远已分配位置)      │
  │    通过 AppendEntries 同步给所有 Follower   │
  └───────────────────────────────────────────┘
```

### 7.5 业务节点与 Center 交互

```
业务节点 Manager → Center:
  ┌─────────────────────────────────────────────────┐
  │ 1. ReportToCenter (periodic, NODE_BEAT 秒)       │
  │    ├─ 有 Raft Leader 缓存 → 只发给 Leader          │
  │    └─ 无 Leader 缓存 → fan-out 所有 Center        │
  │                                                   │
  │ 2. NodeReportRsp 处理 (DisposeDataFromCenter):    │
  │    ├─ err=2 (no stable leader) → 清空 leader 缓存 │
  │    ├─ err=0 + leader_identify → 缓存 leader       │
  │    ├─ err=0 + subscribed_route_snapshot:          │
  │    │   ├─ 与旧快照比较 (SerializeAsString 全量)    │
  │    │   ├─ 有变化 → GetRouteNoticeVersionData()    │
  │    │   │          .SetNodeNotice(oSnapshot)       │
  │    │   │          → 写入共享内存                   │
  │    │   └─ 无变化 → skip (避免无效写入)             │
  │    └─ node_id 变化 → CMD_REQ_REFRESH_NODE_ID      │
  │                      → SendToWorker               │
  └─────────────────────────────────────────────────┘
```

### 7.6 Leader 缓存与故障切换

```
业务节点 Manager:
  ┌────────────────────────────────────────┐
  │ m_strRaftLeaderCenterKey               │
  │                                        │
  │ if (!m_strRaftLeaderCenterKey.empty()) │
  │    SendTo(leader_conn, report)         │  ← 精准发送
  │ else                                   │
  │    for each center_conn:               │  ← fan-out
  │        SendTo(center_conn, report)     │
  └────────────────────────────────────────┘

Leader 缓存更新时机:
  1. NodeReportRsp 回调 (CMD_RSP_NODE_REGISTER / CMD_RSP_NODE_STATUS_REPORT)
  2. err=2 (no stable leader) → 清空缓存
  3. leader_identify 不在配置的 Center 列表中 → 清空缓存
```

---

## 8. 共享内存路由/配置同步

### 8.1 三块共享内存

```
Manager 进程创建 (MAP_SHARED | MAP_ANON):

┌──────────────────────────────────────────────────────────────────┐
│  RouteNoticeVersionMM       路由镜像共享内存          (≈164 KB)  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ seq_node_notice (atomic<uint64>)  ← 业务版本号              │ │
│  │ seq_snapshot    (atomic<uint64>)  ← 奇:写中, 偶:稳定        │ │
│  │ node_id         (uint32)                                       │ │
│  │ node_notice_len (uint32)                                       │ │
│  │ node_notice_crc32 (uint32)                                     │ │
│  │ node_notice_blob[160*1024]  ← NodeNotice protobuf binary      │ │
│  └────────────────────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────────────────────┤
│  CustomConfigVersionMM       自定义配置共享内存        (≈164 KB) │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ seq_custom     (atomic<uint64>)  ← 业务版本号               │ │
│  │ seq_snapshot   (atomic<uint64>)                             │ │
│  │ custom_len     (uint32)                                     │ │
│  │ custom_crc32   (uint32)                                     │ │
│  │ custom_blob[160*1024]  ← JSON custom config                │ │
│  └────────────────────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────────────────────┤
│  LoaderConfigVersionMM      配置文件共享内存         (≈16.1 KB)  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ seq_config     (atomic<uint64>)  ← 配置版本号               │ │
│  │ server_config_name[64]                                      │ │
│  │ server_config_body[16*1024]  ← JSON config content         │ │
│  └────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
```

### 8.2 写入协议（Write Order: blob → len → version++）

```
Manager 写路由镜像 (SetNodeNotice):
  1. seq_snapshot.fetch_add(1)  ← 变奇数，标记"正在写"
  2. memcpy(node_notice_blob, data, data.size())
  3. node_notice_len = data.size()
  4. node_notice_crc32 = Crc32(blob, len)
  5. seq_node_notice.fetch_add(1)  ← 递增业务版本号
  6. seq_snapshot.fetch_add(1)  ← 变偶数，标记"稳定"

Worker 读路由镜像 (GetNodeNotice):
  Double-Snapshot Read (最多重试 3 次):
  1. snapA = seq_snapshot.load(acquire)
  2. if snapA & 1: continue  ← 写入进行中，重试
  3. len = node_notice_len, crc = node_notice_crc32
  4. memcpy(data, blob, len)
  5. snapB = seq_snapshot.load(acquire)
  6. if snapA != snapB || (snapB & 1): continue  ← 读写并发，重试
  7. if Crc32(data) != crc: return false  ← 数据损坏
  8. ParseFromArray(data) → 成功
```

### 8.3 Worker 定时轮询（CheckShareMem）

```
Worker::ShortPeriodicTaskCallback (每 1 秒):

CheckShareMem():
  ├─ [Loader Config]
  │   ├─ IsConfigVersionChange()? → UpdateLoaderConfigVersion()
  │   ├─ GetServerConfigFile() → 解析 JSON
  │   ├─ 比较 custom、log_level、so、module 字段
  │   └─ 有变化 → 重载对应组件
  │
  ├─ [Route Notice]
  │   ├─ IsNodeNoticeVersionChange()? → GetNodeNotice()
  │   ├─ 应用路由镜像:
  │   │   ├─ 构建 expected={node_type → {identify set}}
  │   │   ├─ 删除不在 expected 中的旧路由
  │   │   └─ 添加 expected 中的新路由
  │   └─ UpdateNodeNoticeVersion()
  │
  ├─ [Node ID]
  │   └─ node_id 变化 → SetNodeId()
  │
  └─ [Custom Config]
      ├─ IsCustomVersionChange()? → GetCustomConfig()
      ├─ 解析 customContent JSON
      └─ SetCustomConf() + UpdateCustomVersion()
```

### 8.4 版本通知路径

```
                              ┌─────────────────────────┐
                              │        Center ★          │
                              │  (Raft Leader)           │
                              └───────────┬─────────────┘
                                          │ NodeReportRsp
                                          │ + subscribed_route_snapshot
                              ┌───────────▼─────────────┐
                              │      Manager Process     │
                              │                          │
                              │  1. Compare(旧,新)       │
                              │  2. SetNodeNotice(shm)   │
                              │  3. if node_id changed:  │
                              │     SendToWorker(CMD_)   │
                              └───────────┬─────────────┘
                                          │ Shared Memory
                              ┌───────────▼─────────────┐
                              │      Worker Process      │
                              │                          │
                              │  ShortPeriodicTask (1s): │
                              │    CheckShareMem()       │
                              │    → 版本号变化即更新     │
                              │    → node_id主动通知也处理│
                              └──────────────────────────┘
```

---


---

> 📖 上一篇: [01 核心架构](01-architecture-design.md)  
> 📖 下一篇: [22 运维内幕 — 连接 · 插件 · 性能 · 优化](22-operations-internals.md)

---

## 附录：性能基准数据

> 原独立文档 `architecture/21-data-plane.md#附录性能基准数据`，合并于此方便查阅。

> 日期: 2026-06-29 | 环境: Ubuntu 24.04, Intel N100, lo 网卡 | 工具: Python raw socket

## 目的

验证"单机连接建立 5,000/秒"这一性能目标是否合理。

## 方法

8 线程 × 10 秒，每次新建 TCP 连接，发完请求等响应后关闭，统计 QPS。

| 测试 | 目标 | 方法 |
|------|------|------|
| 裸 TCP | Thunder HelloHttp :27006 | connect → close（不走任何协议） |
| HTTP 短连接 | Thunder HelloHttp :27006 | `POST /hello/hello`, Connection: close |
| WS 握手 | Thunder hello_ws :27010 | HTTP Upgrade → 等 101 响应 → close |

## 结果

### 裸 TCP

| 端点 | QPS | 说明 |
|------|:---:|------|
| `127.0.0.1:27006` | **13,422** | Thunder accept() 上空，无协议解析 |

### HTTP 短连接

| 端点 | QPS | vs 裸 TCP |
|------|:---:|:---:|
| `http://127.0.0.1:27006/hello/hello` | **11,070** | -18%（picohttpparser + JSON + protobuf） |

### WS 握手

| 端点 | QPS | vs 裸 TCP |
|------|:---:|:---:|
| `ws://127.0.0.1:27010/hello/shake` | **3,240** | -76%（HTTP Upgrade + SHA1 + Session） |

### 延迟推算

| 场景 | 单连接延迟 | 推算方式 |
|------|:---:|------|
| 裸 TCP | ~0.07ms | 1/13422 |
| HTTP 短连接 | ~0.09ms | 1/11070 |
| WS 握手 | ~0.31ms | 1/3240 |

### 开销分解

```
裸 TCP:     13,422 ─────────────────────────── accept() 上限
HTTP:       11,070 ─── -2352 (-18%) ───────── +picohttpparser + CJsonObject + protobuf
WS:          3,240 ──── -7830 (-71%) ───────── +HTTP Upgrade + SHA1 + Base64 + Session
```

Thunder 事件循环的 accept() 在 lo 网卡上限约 13K，每加一层协议开销递减。WS 建连最重，但实际业务中连接建立是低频操作，高频路径在 keep-alive 复用。

WS 比 HTTP 低 71%，差在 HTTP Upgrade 头解析 + SHA1 + Base64（计算 `Sec-WebSocket-Accept`）。

---

## Keep-Alive 吞吐量（长连接复用）

> wrk -t4 -c100 -d10s, POST + keep-alive, 本机实测。

短连接测**建连速度**（每次新 TCP），keep-alive 测**持续吞吐**（复用连接）。两个维度合起来才是 Thunder 完整的连接能力。

| 维度 | 指标 | 数值 | 说明 |
|------|------|:---:|------|
| 建连速度 | 裸 TCP | 13,422 conn/s | Thunder accept()，无协议 |
| 建连速度 | HTTP 短连接 | 11,070 conn/s | +HTTP 解析 |
| 建连速度 | WS 握手 | 3,240 conn/s | +Upgrade+SHA1+Session |
| **吞吐量** | **HTTP keep-alive** | **71,417 req/s** | wrk POST, 本机实测 |
| 延迟 P50 | keep-alive | 0.76ms | wrk 报告 |
| 延迟 P99 | keep-alive | 26.1ms | wrk 报告 |

```
           短连接 (建连速度)               Keep-Alive (持续吞吐)
           ────────────────────           ────────────────────
裸 TCP      13,422 conn/s                    —
HTTP        11,070 conn/s                    71,417 req/s
WS           3,240 conn/s                    —
```

## 5,000/秒 可行性

| 条件 | QPS | 达到 5K? |
|------|:---:|:---:|
| 笔记本 lo + 裸 TCP | 13,422 | ✅ |
| 笔记本 lo + HTTP 短连接 | 11,070 | ✅ |
| 笔记本 lo + WS 握手 | 3,240 | ❌ 单 Worker |
| 生产 16 核 + 4 Worker | 估算 13,000+ | ✅ |

> 笔记本单 Worker + lo 虚拟网卡，Thunder HTTP 短连接已跑到 11,070（超过 5K），瓶颈不在 Thunder。
> WS 握手单 Worker 3,240，生产 4 Worker 线性扩展即可到 13K。
> lo 虚拟网卡不经过硬件 offload，物理网卡可进一步提升。

## 代码

| 文件 | 用途 |
|------|------|
| `/tmp/thunder_bench.py` | Thunder HTTP + WS 综合压测脚本 |

## 参考

- [10-vs-nginx-benchmark-20260610.md](10-vs-nginx-benchmark-20260610.md) — wrk keep-alive 吞吐量对比
- [11-io-backend-comparison.md](11-io-backend-comparison.md) — IO 后端性能对比

