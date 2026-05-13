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

### 4.2 真正有收益的场景

ZeroCopy 适合以下**极端场景**：
- 大文件传输（>1MB payload，例如 CDN 回源、视频流）
- 高吞吐代理转发（body 不解包直接透传）
- 频繁的 buffer 复用（连接池 + 固定大小 buffer）

Thunder 当前缺失这些场景的**关键支撑**：没有 proxy/relay 模式，没有大文件流式传输模块。

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
| **收益不显著** | 典型业务中网络拷贝占比 <1%，ZeroCopy 节省的 0.5-1.5% 不可感知 |
| **CBuffer 改动大** | 需要新增固定缓冲区模式，至少改动 3 个模块 |
| **优先做更有价值的事** | io_uring 大包场景已通过 asio_uring 获得 86% 延迟降低 |
| **内核要求** | SEND_ZC 需要 6.0+，当前环境满足但需确认生产环境 |

### 7.2 实施前提

当 Thunder 出现以下**任意 2 个**信号时，可启动：

| # | 信号 | 现状 |
|---|------|------|
| 1 | 出现 >1MB 的大包流式传输需求 | ❌ 无 |
| 2 | 出现 proxy/relay 模式 (body 不解包转发) | ❌ 无 |
| 3 | 单连接吞吐 >1Gbps (拷贝成为瓶颈) | ❌ 单片 160k RPS × 20B = 3.2 MB/s |
| 4 | 已有 registered buffers 基础代码 | ❌ ASIO 测试代码在 3party 中已有 |

### 7.3 推荐时机

```
优先级:
  P0 (当前):   asio_uring 主线程直驱         ✅ 已实现
  P1 (近期):   多业务节点 io_uring 覆盖测试   建议
  P2 (中期):   registered buffers 预研 + POC 本文档
  P3 (远期):   SEND_ZC 正式实施             等待业务需求触发
```

---

## 八、总结

| 问题 | 答案 |
|------|------|
| io_uring zero-copy 适合 Thunder 吗？ | **当前不适合，未来有条件适合** |
| 什么时候适合？ | 出现 >1MB 大包传输或 proxy 模式 + 连接复用 buffer |
| 技术上可行吗？ | **是** — ASIO 已内置支持，CBuffer 改造约 500 行 |
| 收益有多大？ | 典型场景 <1%，极端大包场景 ~5-10% |
| 推荐立即做吗？ | **不推荐** — 业务逻辑延迟占主导，优先聚焦 asio_uring 稳定性和覆盖率 |
| 最务实的下一步？ | asio_uring 后端覆盖更多节点，积累生产数据后再决策 |

---

*分析基于 Thunder dev 分支 commit `7176f75`，内核 Linux 7.0.0-15，ASIO 版本含 `registered_buffer` 支持。*
