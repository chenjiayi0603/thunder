# io_uring + ZeroCopy 在 Thunder 项目的适用性分析

> 日期: 2026-05-13 | 内核: Linux 7.0 | io_uring: 支持 SEND_ZC + REGISTER_BUFFERS

---

## 一、什么是 io_uring ZeroCopy

### 1.1 传统 send 路径

```
应用层             内核态              硬件
  │                  │                  │
  ├─ send(buf, len)  │                  │
  │  ─────────────→  │                  │
  │                  ├─ 拷贝 buf → skb  │
  │                  ├─ 协议栈处理       │
  │                  │  ──────────────→ │
  │                  │                  ├─ DMA → NIC
  │  ← 返回 len ──── │                  │
  │                  │                  │
  问题: buf 内容在内核态又被拷贝了一次 (64KB → 64KB memcpy)
```

### 1.2 io_uring ZeroCopy send 路径

```
应用层                     内核态              硬件
  │                          │                  │
  ├─ io_uring_register_buffers(buf)   (一次性)
  │                          │                  │
  ├─ IORING_OP_SEND_ZC      │                  │
  │  (传 buffer index+len)   │                  │
  │  ─────────────────────→  │                  │
  │                          ├─ 锁定用户页 (pin) │
  │                          ├─ 协议栈直接引用   │
  │                          │  ──────────────→ │
  │                          │                  ├─ DMA → NIC
  │  ← CQE (完成通知) ────── │                  │
  │                          │                  │
  优点: 零拷贝 — 内核直接 DMA 用户态内存到网卡
```

### 1.3 两个关键机制

| 机制 | 作用 | 内核版本要求 |
|------|------|------------|
| `IORING_REGISTER_BUFFERS` | 预注册一组固定内存缓冲区，后续 SEND_ZC/RECV 引用 buffer index 而非指针 | 5.1+ |
| `IORING_OP_SEND_ZC` | 异步零拷贝发送，内核 pin 用户页后直接 DMA | 6.0+ (TCP) |

---

## 二、Thunder 当前 I/O 数据路径

### 2.1 当前写路径

```
Worker::RemoveIoWriteEvent / Coroutine send
  │
  └─ 编解码器 Encode() → CBuffer (应用层缓冲区)
       │
       └─ IoBackend::SubmitWrite(fd, CBuffer* buf)
            │
            ├─ EvIoBackend:    buf->WriteFD(fd) → ::send(fd, ptr, len, MSG_NOSIGNAL)
            │                   ↑ 内核拷贝 buf → skb
            │
            ├─ UringIoBackend: buf->WriteFD(fd) → ::send()   (同步回退)
            │                   ↑ 同上
            │
            └─ AsioUringIoBackend: sock.async_write_some(asio::buffer(ptr, len))
                                  ↑ ASIO 内部调用 io_uring send, 仍有内核拷贝
```

### 2.2 数据拷贝分析

| 数据大小 | 应用场景 | 当前拷贝次数 | 拷贝耗时 (估计) |
|---------|---------|------------|---------------|
| 20B | Echo 响应 | 1 次 (send 内部) | ~0.01 μs（可忽略） |
| 4KB | 典型业务响应 | 1 次 | ~0.5 μs |
| 64KB | 大包 / 文件传输 | 1 次 | ~8 μs |
| 1MB | 批量传输 | 1 次 | ~130 μs |

> 对于 HTTP 服务，单次 send 的拷贝延迟相比业务逻辑（编解码、DB 查询）通常占比 <1%。

### 2.3 CBuffer 内存模型

```cpp
class CBuffer {
    char*  m_buffer;      // raw buffer (malloc 分配)
    size_t m_buffer_len;  // 总容量
    size_t m_write_idx;   // 写位置
    size_t m_read_idx;    // 读位置
    // ...
    bool EnsureWritableBytes(size_t min);  // 可能 realloc → 地址变化
};
```

**关键问题**: CBuffer 是动态缓冲区，`EnsureWritableBytes`/`Compact` 可能触发 `realloc`，导致内存地址变化。而 `IORING_REGISTER_BUFFERS` 要求缓冲区地址固定不变。

---

## 三、逐后端可行性分析

### 3.1 EvIoBackend — ❌ 不适用

`EvIoBackend` 基于 epoll + `::send()`/`::writev()`，不经过 io_uring，无法使用 io_uring 的 zerocopy 机制。

可选的替代方案是 `MSG_ZEROCOPY` socket 选项：
```c
int val = 1;
setsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &val, sizeof(val));
send(fd, buf, len, MSG_ZEROCOPY);
// 然后通过 recvmsg(fd, ..., MSG_ERRQUEUE) 获取完成通知
```
但这需要改造 `EvIoBackend` 的完成通知模型（从同步 send 返回 → 异步错误队列通知），改动量与直接迁移到 io_uring 相当。

**建议**: 不单独对 ev 做 zerocopy，直接升级到 asio_uring。

### 3.2 UringIoBackend — ⚠️ 可行但建议跳过

`UringIoBackend` 是手写的 io_uring 后端，技术上可以实现 zerocopy：
1. 注册 CBuffer 的 `m_buffer` 为 registered buffer
2. SubmitWrite 用 `io_uring_prep_send_zc()` 替代当前同步 `::send()`
3. CQE 收割时处理 `IORING_CQE_F_MORE` 完成通知

但 `UringIoBackend` 已在计划中标记为「待 asio_uring 验证后移除」，不应再投入改造。

**建议**: 跳过，直接聚焦 asio_uring。

### 3.3 AsioUringIoBackend — ✅ 最适合

`AsioUringIoBackend` 使用 standalone ASIO，而 ASIO 1.24+ 已支持 `registered_buffer`：

```cpp
// ASIO 已内置 registered_buffer 支持 (code/3party/asio)
#include "asio/registered_buffer.hpp"

// 注册缓冲区（一次性）
std::vector<asio::mutable_registered_buffer> registered;
registered.push_back(asio::register_buffers(ctx, asio::buffer(buf, size)));

// 零拷贝发送
sock.async_write_some(registered[0], handler);  // ASIO 内部用 IORING_OP_SEND_ZC
```

**ASIO 源码证据**: `/home/tommychen/thunder/code/3party/asio/src/tests/unit/registered_buffer.cpp` 已包含 `mutable_registered_buffer` / `registered_buffer_id` 的完整编译测试，说明当前 ASIO 版本原生支持。

---

## 四、收益量化

### 4.1 不同场景的收益

基于 Thunder 实测数据和 Linux 内核 `memcpy` 性能估算：

| 场景 | 当前延迟 | ZeroCopy 后 | 节省 | 是否值得 |
|------|---------|------------|------|---------|
| 小包 Echo (20B) | 0.43 ms | 0.43 ms | ~0% | ❌ |
| 大包响应 (4KB) | ~1.5 ms | ~1.49 ms | ~0.5% | ❌ |
| 64KB 传输 | ~17 ms | ~16.9 ms | ~0.6% | ⚠️ |
| 1MB 文件下发 | ~130 ms | ~128 ms | ~1.5% | ⚠️ |
| 10Gbps 满速 (64KB) | 16.78 ms | 16.70 ms | ~0.5% | ❌ |

**关键发现**: 对于 Thunder 的典型业务场景（HTTP API, WebSocket 消息, RPC），业务逻辑延迟（编解码、DB、协程调度）占比 >99%，网络拷贝仅占 <1%。ZeroCopy 的收益在这种情况下**不显著**。

### 4.2 真正有收益的场景：Proxy 模式 与 大包流式传输

ZeroCopy 的核心价值不在常规 API 场景，而在以下两种**数据面密集型**模式。

#### (1) Proxy / 反向代理模式

Thunder 当前是「服务端模式」— 接收请求 → **解包** → 业务逻辑 → **打包** → 返回。body 必须完整解析。

Proxy 模式则是「透传模式」— body **不解包、不处理**，原样从一个 socket 转发到另一个 socket：

```
当前 Thunder (服务端模式):
  Client ──→ [JSON 解析] ──→ 业务逻辑 ──→ [JSON 构造] ──→ Client
               ↑ memcpy 一次                  ↑ memcpy 一次
               (开销 < 1% 总延迟)

Proxy 模式 (透传):
  Client ──→ [不解包] ──→ 后端服务 ──→ [不解包] ──→ Client
               ↑ 透传 body                     ↑ 透传 body
               (body 越大，拷贝占比越高)
```

**为什么 Proxy 模式需要 ZeroCopy**：

```
假设一个 100MB 的文件通过 Thunder 反向代理转发：

当前做法 (每块 64KB):
  100MB ÷ 64KB = 1600 次 send
  每次 send: memcpy(buf → skb) ≈ 8μs
  纯拷贝开销: 1600 × 8μs ≈ 12.8ms
  这 12.8ms 是纯 CPU 浪费 — 数据本身不需要任何处理

ZeroCopy 做法:
  注册 100MB 固定 buffer
  1 次 IORING_OP_SEND_ZC → 内核直接 DMA 到网卡
  纯拷贝开销: 0
```

典型代理层产品（NGINX, Envoy, HAProxy）的核心优化之一就是尽可能减少 body 拷贝。

#### (2) 大包流式传输

类似文件下载、视频流推送、日志批量导出等场景。当前 Thunder 的 Echo 只有 20B body，`CBuffer` 一次性装下没问题。但如果服务端需要下发 100MB 的文件：

```
当前做法:
  CBuffer 装不下 100MB
  → 分块 read → 每块 memcpy 到 CBuffer → send (再 memcpy 到 skb)
  → 100MB 文件 = 至少 2 次全量 memcpy = 200MB 内存搬运

ZeroCopy 做法:
  注册 100MB 固定 buffer
  → splice/sendfile 从磁盘直接到 buffer
  → IORING_OP_SEND_ZC 从 buffer 直接到网卡
  → 零 CPU 拷贝
```

#### 两种模式 vs 当前 Thunder

| 模式 | 当前 Thunder? | body 是否解包 | ZeroCopy 收益 |
|------|:---:|:---:|---|
| 服务端模式 (解包→处理→打包) | ✅ 现在是 | 是 | <1%（业务逻辑占主导） |
| **Proxy/反向代理** (不解包透传) | ❌ 没有 | **否** | **10-30%**（body 不走 CPU） |
| **大包流式传输** (文件/视频下发) | ❌ 没有 | **否** | **5-10%**（省去大数据拷贝） |

**一句话**: 如果 Thunder 未来要当反向代理用（类似 NGINX 那样透传流量），或者内置文件服务下发大文件，ZeroCopy 就很值得。当前纯业务逻辑的 JSON API 场景下，收益太小不值得改。

Thunder 当前缺失这些场景的**关键支撑**：没有 proxy/relay 透传模式，没有大文件流式传输模块 — 这些是实现 ZeroCopy 价值的前提。

---

## 五、实现复杂度

### 5.1 需要改动的模块

```
改动清单:
  ├── code/Util/src/util/CBuffer.hpp/cpp    (需要新增固定缓冲区模式)
  ├── code/Net/src/labor/AsioUringIoBackend.cpp  (SubmitWrite 改用 registered_buffer)
  ├── code/Net/src/labor/AsioUringIoBackend.hpp  (新增 buffer 注册管理)
  └── code/Net/src/labor/Labor.cpp               (初始化时预注册 buffer pool)
```

### 5.2 CBuffer 改造难点

```
当前 CBuffer:
  m_buffer = malloc(N)    ← 地址任意
  EnsureWritableBytes → 可能 realloc  ← 地址变化！
  Compact → 可能 free + malloc      ← 地址变化！

ZeroCopy 要求:
  m_buffer 必须固定                ← 不能 realloc
  必须在 io_uring 中注册 buffer_id  ← 初始化时注册
  SubmitWrite 传 buffer_id + offset  ← 不是传指针

方案 A: 新增 FixedCBuffer 子类（预分配固定大小，禁用 realloc）
方案 B: CBuffer 增加 Registered 模式（仅在 asio_uring 后端使用）
方案 C: 新增独立 BufferPool，连接从池中获取固定 buffer
```

**推荐方案 B**: 改动量最小，CBuffer 新增 `RegisterForZeroCopy()` / `GetRegisteredBufferId()` 接口，仅在 `asio_uring` 后端启用。

### 5.3 生命周期管理风险

```
风险场景:
  1. SubmitWrite(fd, buf)
  2. io_uring 持有 buf 引用 (内核 pin 了页面)
  3. 业务代码提前释放 buf / realloc buf
  4. → 内核 DMA 到已释放的内存 → 数据损坏 / kernel panic

需要的保护:
  - buf 引用计数 (io_uring 在用 +1, CQE 到达 -1)
  - 或: buf 生命周期绑定到连接 (connection-scoped)
```

Thunder 中 `CBuffer` 绑定在 `tagConnectionAttr` 上（`pRecvBuff`/`pSendBuff`），连接关闭前不会释放。这自然避免了提前释放问题，但需要确保 `Compact`/`EnsureWritableBytes` 不会在 ZeroCopy 发送期间被调用。

---

## 六、对比：MSG_ZEROCOPY vs io_uring SEND_ZC

| 维度 | MSG_ZEROCOPY (socket opt) | io_uring SEND_ZC |
|------|--------------------------|-----------------|
| 内核版本 | 5.0+ | 6.0+ (TCP) |
| 完成通知 | `recvmsg(MSG_ERRQUEUE)` | io_uring CQE |
| 与事件循环集成 | 需额外监听 error queue fd | 天然集成 (ring_fd) |
| 缓冲区管理 | 不需预注册 | 需 register buffers |
| 实现复杂度 | 中 | 中（但 ASIO 已封装） |
| 适合 Thunder? | ❌ (需要 epoll 额外监听) | ✅ (ASIO 原生支持) |

---

## 七、决策建议

### 7.1 当前建议：暂不实施

| 理由 | 说明 |
|------|------|
| **收益不显著** | 当前服务端模式下，网络拷贝占延迟 <1%；需 Proxy 透传或大包流式才有 10-30% 收益 |
| **CBuffer 改动大** | 需要新增固定缓冲区模式，至少改动 3 个模块 |
| **优先做更有价值的事** | io_uring 大包场景已通过 asio_uring 获得 86% 延迟降低 |
| **内核要求** | SEND_ZC 需要 6.0+，当前环境满足但需确认生产环境 |

### 7.2 实施前提

当 Thunder 出现以下**任意 2 个**信号时，可启动（详见 4.2 节）：

| # | 信号 | 现状 |
|---|------|------|
| 1 | 出现 **Proxy/反向代理** 模式 (body 不解包，透传转发) | ❌ 无 |
| 2 | 出现 **>1MB 大包流式传输** (文件下发、视频流、日志导出) | ❌ 无 |
| 3 | 单连接吞吐 >1Gbps (拷贝成为瓶颈) | ❌ 当前 ~3.2 MB/s |
| 4 | 已有 registered buffers 基础代码 | ⚠️ ASIO 3party 中有测试代码，Thunder 自身无 |

### 7.3 推荐路线

```
P0 (当前):   asio_uring 主线程直驱          ✅ 已实现
P1 (近期):   多业务节点 io_uring 覆盖测试    建议
P2 (中期):   registered buffers 预研 + POC  本文档
P3 (远期):   SEND_ZC 正式实施              等待业务需求触发
```

#### 各阶段目的概述

```
P1 → 建立信心: 当前 asio_uring 只在 HelloHttp 一个节点跑通过。
     全节点覆盖 E2E 测试，确认没有隐藏的边界 bug。
     跑通了才能放心用，跑出 bug 就修。不涉及新功能开发。

P2 → 降低风险: CBuffer 动态 realloc 与 registered buffer 固定地址天然冲突。
     在独立分支上做 POC 验证方案可行性，结论写文档，不合入主分支。
     将来真要上 SEND_ZC 时，方案现成、风险已知，不用从零摸索。

P3 → 消除拷贝: 终局目标。前提是 P1(全节点稳定) + P2(方案已验证) + 业务需求。
     数据从用户态直接 DMA 到网卡，内核零拷贝。
     三个条件缺一个都不做。
```

---

#### P0 — asio_uring 主线程直驱 ✅ 已实现

当前已完成。IoBackend 三档可运行时切换（ev / uring / asio_uring），`AsioUringIoBackend` 通过 ev_prepare + ev_io(ring_fd) + ev_check 三路驱动，零锁零线程跳，64KB 大包延迟相对 ev 降低 86%。

**已完成的工作**:
- `IoBackend` 抽象接口 (`code/Net/include/labor/IoBackend.hpp`)
- `AsioUringIoBackend` 主线程直驱实现
- `Labor.cpp` 中三档后端运行时选择 (`io_backend` 配置项)
- `RemoveIoWriteEvent` 的 CancelFd + SubmitRead 修复
- wrk 三档横向压测 + 性能文档

**当前局限**: 仅在 HelloHttp 节点验证过。其他节点（Interface, Logic, HelloWs, HelloHttps）理论上共享同一套 `Labor` 初始化逻辑，但未逐节点回归测试。

---

#### P1 — 多业务节点 io_uring 覆盖测试

**目标**: 确保 asio_uring 在**所有节点类型**下正常工作，而不是只在 HelloHttp 一种场景下跑通。

**为什么需要**:
- 不同节点的网络 IO 模式不同：
  - `HelloHttp` — 纯 HTTP C2S（客户端→服务端）
  - `Interface` — C2S HTTP + 协程 S2S（服务端→Center→Logic）
  - `Logic` — 纯 S2S（接收 Interface/Center 的内部协议）
  - `HelloWs` — WebSocket 长连接（IO 模式与短连接 HTTP 不同）
  - `HelloHttps` — TLS 加密流量（OpenSSL BIO 与 io_uring 的交互）
- io_uring 的 `SubmitWrite` 在 `UringIoBackend` 中用的是同步回退（`::send()`），说明写路径在 io_uring 下有特殊处理逻辑。AsioUringIoBackend 虽然用 `async_write_some` 实现了异步写，但不同节点对写的依赖模式（短 burst vs 长流）需要逐一验证。
- S2S 连接（Manager→Center, Worker→Center）的生命周期管理（频繁创建/销毁连接）对 io_uring fd 注册/注销是压力测试。

**具体要做的事**:

```
1. 逐节点切换 io_backend = "asio_uring"
    ├── HelloHttp     ✅ 已验证
    ├── HelloHttps    ⚠️ 待测 (TLS + io_uring)
    ├── HelloWs       ⚠️ 待测 (WebSocket 长连接)
    ├── Interface     ⚠️ 待测 (协程 S2S + HTTP C2S)
    └── Logic         ⚠️ 待测 (纯 S2S 内部协议)

2. 每个节点运行 E2E 测试套件
    ./tests/run_all.sh e2e    # 25 cases

3. 记录各节点 asio_uring vs ev 的性能差异
    - 写个脚本统一采集 RPS/延迟

4. 修复发现的问题
    - 预期: 可能有 fd 泄漏、CQE 未收割等边界 bug
```

**预计工作量**: 1-2 天，主要是跑测试 + 修边界 bug，不需要大量新代码。

---

#### P2 — registered buffers 预研 + POC

**目标**: 不改生产代码，在独立分支上用 ASIO 的 `registered_buffer` 接口做一个最小验证，确认方案在 Thunder 的 CBuffer 模型下可行。

**为什么是「预研」而不是「实施」**:
- CBuffer 是动态缓冲区（`EnsureWritableBytes` 会 `realloc`），与 registered buffers 的「固定地址」要求冲突
- 需要找到最小改动方案，让 CBuffer 在 asio_uring 后端下切换到固定模式
- 需要验证 `io_uring_register_buffers` 的性能开销（注册本身是一次性的，但每个连接的 buffer 大小不同）

**POC 要验证的**:

```
1. 单个连接的 CBuffer 固定化改造成本
   方案 A: CBuffer 新增 FixedMode (禁用 realloc)
   方案 B: 新增独立的 FixedBufferPool (连接从池中取)

2. ASIO registered_buffer 在 Thunder 的集成路径
   asio::register_buffers(ctx, buffer) → 获取 buffer_id
   sock.async_write_some(registered_buffer, handler)

3. 性能对比 (wrk)
   当前 asio_uring vs asio_uring + registered_buffers (无 SEND_ZC)
   → 验证注册本身是否有额外开销

4. 生命周期安全验证
   buffer 在 SubmitWrite 期间被 Compact/释放 → 是否 crash
```

**不做的**:
- 不改 CBuffer 的 public 接口（只在内部新增可选模式）
- 不开启 SEND_ZC（那是 P3 的事）
- 不合入 dev 分支（只做分支验证）

**预计工作量**: 2-3 天，产出 POC 分支 + 验证报告。

---

#### P3 — SEND_ZC 正式实施

**目标**: 在 P1 覆盖测试通过 + P2 POC 验证可行 + 业务需求触发的前提下，正式实现 io_uring 零拷贝发送。

**触发条件**（三选二）:
1. 出现 Proxy/反向代理透传模式
2. 出现 >1MB 大包流式传输需求
3. 单连接吞吐 >1Gbps 成为瓶颈

**此时 P2 的 POC 成果直接应用**:
- CBuffer 固定模式方案已验证 → 直接合入
- registered buffers 集成路径已跑通 → 改 SubmitWrite 即可
- 生命周期安全问题已澄清 → 有现成的防护策略

**正式实施改动**:
```cpp
// AsioUringIoBackend::SubmitWrite 改动示意
bool AsioUringIoBackend::SubmitWrite(int fd, util::CBuffer* buf, uint32_t seq)
{
    // ... 现有检查 ...

    if (m_useZeroCopy && buf->IsRegistered()) {
        // P3 新增: 零拷贝路径
        auto reg_buf = buf->GetRegisteredBuffer();
        sp->sock.async_write_some(reg_buf, [...] {
            buf->AdvanceReadIndex(n);  // 同上
            m_callback(fd, seq, IoOp::Write, n, m_userData);
        });
    } else {
        // 现有路径: asio::buffer(ptr, len)
        sp->sock.async_write_some(asio::buffer(src, readable), [...]{...});
    }
}
```

**预计工作量**: 1-2 天（基于 POC 的成果，改动量很小）。

---

### 7.4 路线图总览

```
现在 ──→ P1(近期) ──→ P2(中期) ──→ P3(远期,有条件)
 │           │            │              │
 │  asio_uring│  全节点覆盖 │  POC 验证    │  正式实施
 │  主线程直驱│  回归测试  │  Buffer固定  │  SEND_ZC
 │  ✅ 已实现 │  1-2天     │  2-3天       │  1-2天
 │           │            │              │
 └─ 零风险 ──┘ 低风险 ────┘ 无风险 ──────┘ 业务触发
```

**关键原则**: 每步独立验证，不阻塞主分支。P2 POC 不合并，P3 等业务需求触发才执行。

---

## 八、总结

| 问题 | 答案 |
|------|------|
| io_uring zero-copy 适合 Thunder 吗？ | **当前不适合，未来有条件适合** |
| 什么时候适合？ | 出现 >1MB 大包传输或 proxy 模式 + 连接复用 buffer |
| 技术上可行吗？ | **是** — ASIO 已内置支持，CBuffer 改造约 500 行 |
| 收益有多大？ | 典型场景 <1%，极端大包场景 ~5-10% |
| 推荐立即做吗？ | **不推荐** — 业务逻辑延迟占主导，优先聚焦 asio_uring 稳定性和覆盖率 |
| 最务实的下一步？ | **P1**: asio_uring 覆盖全部节点 (Interface/Logic/Ws/Https)，积累生产数据 |
| 多久能上 ZeroCopy？ | P1(1-2d) → P2(2-3d) → P3(1-2d)，业务触发后总工期约 1 周 |
| 最大风险？ | CBuffer realloc 与 registered buffer 固定地址的冲突（P2 POC 重点验证） |

---

*分析基于 Thunder dev 分支 commit `7176f75`，内核 Linux 7.0.0-15，ASIO 版本含 `registered_buffer` 支持。*
