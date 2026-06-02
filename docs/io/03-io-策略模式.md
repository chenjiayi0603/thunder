# Thunder IoBackend 策略模式重构设计

> **版本**: v1.1
> **日期**: 2026-05-18
> **状态**: 设计阶段 (明确角色命名 + 取消中间基类)
> **目标**: 一套代码支持 ev / io_uring / DPDK+mTCP 三种底层 I/O，运行时配置切换，Worker 零分支

---

## 1. 问题诊断

### 1.1 当前架构：IoBackend 只抽象了一半

`IoBackend` 是策略接口，但只覆盖了 **I/O 提交**（`SubmitRead/SubmitWrite`），**连接生命周期**（创建/accept/关闭/属性设置）全是 Worker 里的直接内核调用。

```
当前 IoBackend 接口 (7 个方法):
  ✅ Init / Destroy         — 生命周期
  ✅ SubmitRead / SubmitWrite — I/O 提交
  ✅ CancelFd / HasPending  — 连接清理
  ❌ 缺失:
     CreateListenSocket()   — 创建监听 socket
     Accept()               — 接受新连接
     CloseFd()              — 关闭连接
     SetSocketOpt()         — 设置 socket 属性
     GetPeerName()          — 获取对端地址
```

### 1.2 当前 Worker 的分支判断（杂乱）

```cpp
// Worker.cpp 现状: 到处是 if (m_pIoBackend) 分支

// 位置 A: 写完成回调
if (m_pIoBackend)
    m_pIoBackend->SubmitWrite(pConn->iFd, pConn->pSendBuff, pConn->ulSeq);
else
    AddIoWriteEvent(pConn);   // legacy ev_io

// 位置 B: DestroyConnect 关闭
if (m_pIoBackend)
    m_pIoBackend->CancelFd(iFd);
close(iFd);                   // ← 永远执行，但不该对 mTCP sockid 执行

// 位置 C: listen socket 创建 → 全部是直接内核调用，不经过 IoBackend
socket() → setsockopt() → bind() → listen()

// 位置 D: accept → 直接内核调用
accept(iFd, ...)

// 位置 E: 读重新提交
if (m_pIoBackend && !m_pIoBackend->HasPending(iFd))
    m_pIoBackend->SubmitRead(iFd, pConn->pRecvBuff, ulSeq);
// else: ev_io watcher 自动重新触发
```

**问题**：每加一个新后端（DPDK），这些分支点全部要改，且各后端有不同的分支逻辑，极易出 bug。

---

## 2. 方案：完整策略模式

### 2.1 核心思想

```
Worker 代码 = 纯业务逻辑（编解码/路由/Session/Step）
           ↓ 只调用 IoBackend 接口，不碰任何 socket API
IoBackend  = 完整的连接生命周期策略接口
           ↓
    ┌──────┼──────┬──────────────┐
    │      │      │              │
  EvBackend  │  UringBackend  DpdkBackend
 (内核epoll)│ (io_uring)     (DPDK+mTCP)
```

Worker 代码里**零个 `if (m_pIoBackend)` 分支**，所有 I/O 操作都通过策略接口委托。

### 2.2 扩展后的 IoBackend 接口

```cpp
// code/Net/include/labor/IoBackend.hpp (扩展版)

namespace net {

struct PeerAddr {
    char ip[64];
    uint16_t port;
};

class IoBackend
{
public:
    virtual ~IoBackend() = default;

    // ========== 生命周期 ==========
    virtual bool Init(struct ev_loop* loop, IoCompletionCallback cb, void* userData) = 0;
    virtual void Destroy() = 0;

    // ========== 监听 socket ==========
    // 创建监听 socket。返回 fd (内核 fd 或 mTCP sockid)。
    // boReusePort: 是否 SO_REUSEPORT（mTCP 不支持时忽略）
    virtual int  CreateListenSocket(const char* ip, uint16_t port,
                                    bool boReusePort, int backlog) = 0;

    // ========== Accept ==========
    // 从监听 fd accept 新连接。返回 client fd，同时填充对端地址。
    // 返回 < 0 表示失败或 EAGAIN。
    virtual int  Accept(int listenFd, PeerAddr& outPeerAddr) = 0;

    // ========== I/O 提交 ==========
    virtual bool SubmitRead(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq) = 0;
    virtual bool SubmitWrite(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq) = 0;

    // ========== 连接关闭 ==========
    virtual void CancelFd(int fd) = 0;          // 取消待处理 I/O
    virtual void CloseFd(int fd) = 0;           // 实际关闭（内核 close 或 mtcp_close）
    virtual bool HasPending(int fd) const = 0;

    // ========== Socket 属性 ==========
    // 设置连接属性（TCP_NODELAY 等）。不支持的选项静默忽略（不报错）。
    virtual void SetSocketOpt(int fd) = 0;

    // ========== 对端地址 ==========
    // 获取对端地址。调用方应优先使用 Accept 时缓存的地址；
    // 此方法仅作为兜底（ev/uring 用 getpeername，dpdk 返回缓存的地址）。
    virtual bool GetPeerName(int fd, PeerAddr& outAddr) = 0;

    // ========== 标识 ==========
    virtual const char* Name() const = 0;
};

} // namespace net
```

### 2.3 三个具体策略实现（无中间基类，允许少量代码重复）

```
EvIoBackend (扩展, 文件: EvIoBackend.cpp):
  CreateListenSocket() → socket() + setsockopt(SO_REUSEPORT) + bind() + listen()
  Accept()             → accept(fd, ...) + SetSocketOpt()
  CloseFd()            → ::close(fd)
  SetSocketOpt()       → setsockopt(TCP_NODELAY, SO_KEEPALIVE)
  GetPeerName()        → getpeername()
  SubmitRead/Write     → 注册 ev_io watcher

NativeUringIoBackend (扩展):
  CreateListenSocket() → 同 EvIoBackend (内核 socket, 不是 io_uring 的活)
  Accept()/CloseFd()   → 同 EvIoBackend
  SetSocketOpt()       → 同 EvIoBackend
  GetPeerName()        → getpeername()
  SubmitRead/Write     → io_uring SQE

AsioUringIoBackend (扩展):
  同上 NativeUringIoBackend, SubmitRead/Write 用 ASIO 封装

DpdkIoBackend (新增):
  CreateListenSocket() → mtcp_socket() + mtcp_bind() + mtcp_listen()
  Accept()             → mtcp_accept(mctx, sockid, ...)
  CloseFd()            → mtcp_close(mctx, sockid)
  SetSocketOpt()       → mtcp_setsockopt (支持的子集)
  GetPeerName()        → 从 Accept 时缓存的 PeerAddr 返回
  SubmitRead/Write     → mtcp_epoll + mtcp_read/write
```

> 不抽 `KernelIoBackend` 基类。ev/uring 共用 ~60 行内核 API 包装，重复的成本远低于增加一层继承带来的调试心智负担。

---

## 3. Worker 重构

### 3.1 重构前（当前）

```cpp
// InitClientListener — 当前
bool Worker::InitClientListener()
{
    int iFd = socket(...);                    // ❌ 直接内核
    setsockopt(iFd, SO_REUSEPORT, ...);       // ❌ 直接内核
    bind(iFd, ...);                           // ❌ 直接内核
    listen(iFd, ...);                         // ❌ 直接内核
    m_iC2SListenFd = iFd;
    AddIoReadEvent(pListenConn);             // ❌ 直接 libev
}

// AcceptClientConn — 当前
bool Worker::AcceptClientConn(int iFd) {
    int iAcceptFd = accept(iFd, ...);         // ❌ 直接内核
    SetSocketAttr(iAcceptFd);                 // ❌ 直接内核
    CreateAcceptFdAttr(iAcceptFd, ...);
}

// DestroyConnect — 当前
bool Worker::DestroyConnect(...) {
    if (m_pIoBackend)
        m_pIoBackend->CancelFd(iFd);          // ✅ IoBackend
    close(iFd);                               // ❌ 直接内核
}

// HandleIoReadComplete — 当前
    if (m_pIoBackend && !m_pIoBackend->HasPending(iFd))
        m_pIoBackend->SubmitRead(...);        // ✅ IoBackend (但有 if)
    // else: ev_io watcher 自动重新触发
```

### 3.2 重构后

```cpp
// InitClientListener — 重构后
bool Worker::InitClientListener()
{
    int iFd = m_pIoBackend->CreateListenSocket(
        m_strHostForClient.c_str(), m_iPortForClient,
        /*boReusePort=*/true, m_iClientSocketBackLog);
    if (iFd < 0) return false;

    m_iC2SListenFd = iFd;

    // 统一: SubmitRead 在监听 fd 上 — IoBackend 内部决定
    // 是注册 ev_io watcher 还是 mtcp_epoll 还是 io_uring
    auto pListenBuf = std::make_shared<util::CBuffer>(0); // 虚 buffer
    m_pIoBackend->SubmitRead(iFd, pListenBuf, GetFdSequence());
}

// AcceptClientConn — 重构后
bool Worker::AcceptClientConn(int iFd)
{
    IoBackend::PeerAddr peerAddr;
    int iAcceptFd = m_pIoBackend->Accept(iFd, peerAddr);
    if (iAcceptFd < 0) return false;

    m_pIoBackend->SetSocketOpt(iAcceptFd);      // 统一

    tagConnectionAttr* pConnAttr = CreateAcceptFdAttr(
        iAcceptFd, GetFdSequence(), m_eAccessCodec);
    snprintf(pConnAttr->szRemoteAddr, sizeof(pConnAttr->szRemoteAddr),
             "%s", peerAddr.ip);                // 用 Accept 返回的地址

    return true;
}

// DestroyConnect — 重构后
bool Worker::DestroyConnect(...) {
    m_pIoBackend->CancelFd(iFd);                // 取消异步 I/O
    m_pIoBackend->CloseFd(iFd);                 // 关闭连接（内核或 mtcp）
    // 不再有 ::close(fd)
}

// HandleIoReadComplete — 重构后
    // 统一: 总是重新提交读（IoBackend 内部处理重复提交）
    // ev 后端: SubmitRead 注册 ev_io watcher
    // uring 后端: SubmitRead 提交 io_uring SQE
    // dpdk 后端: SubmitRead 注册 mtcp_epoll
    m_pIoBackend->SubmitRead(iFd, pConn->pRecvBuff, ulSeq);
```

### 3.3 消除所有 `if (m_pIoBackend)` 分支

| 重构前 Worker 中的分支 | 重构后 |
|----------------------|--------|
| `if (m_pIoBackend) SubmitWrite(...) else AddIoWriteEvent(...)` | → `m_pIoBackend->SubmitWrite(...)` 统一调用 |
| `if (m_pIoBackend) CancelFd(...)` + 后面的 `close(fd)` | → `m_pIoBackend->CancelFd(...)` + `m_pIoBackend->CloseFd(...)` |
| `if (m_pIoBackend && !HasPending) SubmitRead(...)` | → 总是调用 `m_pIoBackend->SubmitRead(...)` |
| `socket()/bind()/listen()` | → `m_pIoBackend->CreateListenSocket(...)` |
| `accept()` | → `m_pIoBackend->Accept(...)` |
| `setsockopt(...)` | → `m_pIoBackend->SetSocketOpt(...)` |

### 3.4 Legacy ev_io 路径的处理

`EvIoBackend` 内部用 `ev_io` watcher 实现 `SubmitRead/SubmitWrite`。Worker 不再直接操作 `ev_io` watcher 或调用 `AddIoReadEvent/AddIoWriteEvent`。

```cpp
// EvIoBackend::SubmitRead 内部
bool EvIoBackend::SubmitRead(int fd, shared_ptr<CBuffer> buf, uint32_t seq)
{
    // 注册/修改 ev_io watcher (已有实现)
    // 这一步原本在 Worker 里通过 AddIoReadEvent 完成
    // 现在封装在 EvIoBackend 内部
}
```

---

## 4. 工厂方法与配置

### 4.1 后端创建工厂

```cpp
// Labor::InitIoBackend — 扩展
bool Labor::InitIoBackend(const util::CJsonObject& oJsonConf,
                          IoCompletionCallback callback)
{
    // 清理旧后端
    if (m_pIoBackend) {
        m_pIoBackend->Destroy();
        delete m_pIoBackend;
        m_pIoBackend = nullptr;
    }

    std::string strBackend;
    oJsonConf.Get("io_backend", strBackend);

    // 策略工厂
    static const std::unordered_map<std::string,
        std::function<IoBackend*()>> kFactory = {
        {"ev",           []() -> IoBackend* { return new EvIoBackend(); }},
        {"native_uring", []() -> IoBackend* { return new NativeUringIoBackend(); }},
        {"asio_uring",   []() -> IoBackend* { return new AsioUringIoBackend(); }},
        {"dpdk",         []() -> IoBackend* { return new DpdkIoBackend(); }},
    };

    auto it = kFactory.find(strBackend);
    if (it == kFactory.end()) {
        LOG4_WARN("unknown io_backend '%s', using ev", strBackend.c_str());
        it = kFactory.find("ev");
    }

    IoBackend* pBackend = it->second();
    if (pBackend && pBackend->Init(m_loop, callback, static_cast<void*>(this))) {
        m_pIoBackend = pBackend;
        LOG4_INFO("IoBackend: %s initialized", pBackend->Name());
        return true;
    }

    // 失败回退
    delete pBackend;
    if (strBackend != "ev") {
        LOG4_WARN("IoBackend '%s' init failed, fallback to ev", strBackend.c_str());
        return InitIoBackend(/*force=*/"ev", oJsonConf, callback);
    }
    LOG4_ERROR("IoBackend: all backends failed");
    return false;
}
```

### 4.2 配置示例（游戏网关，DPDK 后端）

```json
{
    "node_type": "GATEWAY",
    "io_backend": "dpdk",

    "dpdk": {
        "eal_args": "-l 0-3 -n 4",
        "port_id": 0,
        "mbuf_pool_size": 65536,
        "lcore_id": 2
    },
    "mtcp": {
        "max_concurrency": 500000,
        "rcv_buf_size": 8192,
        "snd_buf_size": 8192
    }
}
```

**切换回内核**: 改一行 `"io_backend": "ev"` → Worker 代码零改动。

---

## 5. 兼容性保证

### 5.1 向后兼容

| 场景 | 保证 |
|------|------|
| 不配 `io_backend` | 默认 ev，行为不变 |
| 现有 `HelloHttp/HelloWs` 等 | ev/uring 后端不变 |
| 现有的 `AddIoReadEvent/AddIoWriteEvent` | EvIoBackend 内部调用，对外接口不变 |
| 现有 `IoRead/IoWrite` 回调 | 移到 EvIoBackend 内部，Worker 只通过 `OnIoComplete` 回调感知 |
| `tagConnectionAttr` | `iFd` 字段语义：ev/uring 后端 = 内核 fd，dpdk 后端 = mTCP sockid+偏移。Worker 不区分 |

### 5.2 旧代码迁移策略

**不一次性全改**。分两步：

```
Step 1: 扩展 IoBackend 接口，EvIoBackend 实现所有新方法
        Worker 代码逐步用新接口替代直接内核调用
        此时行为完全不变（EvIoBackend 内部还是内核调用）

Step 2: DpdkIoBackend 实现同样的接口
        Worker 代码无需再改
        切换 io_backend=dpdk 即可
```

---

## 6. 实施计划

| 阶段 | 内容 | 改动文件 | 工时 |
|------|------|---------|------|
| **P1: 接口扩展** | IoBackend +5 新方法声明。EvIoBackend/UringBackend 各实现 5 个包装方法 | `IoBackend.hpp` +30, `EvIoBackend.cpp` +63, `NativeUringIoBackend.cpp` +30, `AsioUringIoBackend.cpp` +30 | 2d |
| **P2: Worker 重构** | 消除所有 `if (m_pIoBackend)` 分支，InitClientListener/Accept/DestroyConnect 统一走接口 | `Worker.cpp` (~15 处改动, ~90 行涉及) | 2d |
| **P3: DPDK 后端** | DpdkContext + DpdkIoBackend 实现完整 12 方法接口 | 新增 `DpdkContext.cpp/hpp` (~150), `DpdkIoBackend.cpp/hpp` (~400) | 3d |
| **P4: 工厂方法** | InitIoBackend 改为 map 驱动工厂 | `Labor.cpp` (~35 行涉及) | 0.5d |
| **P5: 回归测试** | ev/uring 后端全量测试，确保重构无回归 | `tests/e2e/` | 1d |
| **P6: DPDK 端到端** | dpdk 后端 + GameGateway 集成 | `deploy/GameGateway/` | 3d |
| **合计** | | | **~11.5d** |

> 不引入中间基类，ev/uring 后端各写一份内核 API 包装（每个方法 3-8 行），架构扁平、排查方便。

---

## 7. 设计收益

| 收益 | 说明 |
|------|------|
| **一套代码三后端** | 游戏网关 + REST API 网关 + 内部 RPC 共用同一套 Worker/Codec/Session 代码 |
| **配置切换** | 改一行 JSON `io_backend` 即可，无需重新编译 |
| **零 Worker 分支** | Worker.cpp 不再有 `if (m_pIoBackend)` — 无论哪个后端，Worker 代码完全一致 |
| **易测试** | 可 mock IoBackend 进行单元测试 |
| **可扩展** | 加新后端（如 AF_XDP）只需实现 IoBackend，Worker 不动 |
| **回退安全** | 后端初始化失败自动降级 ev，不 crash |

---

## 8. 类图与角色说明

### 8.1 四层角色

```
┌──────────────────────────────────────────────────────────────┐
│ Worker (业务层)                                               │
│ 纯业务逻辑：编解码、路由、Session、Step                       │
│ 只调用 IoBackend 接口，不碰任何 socket API                    │
│ 零个 if(m_pIoBackend) 分支                                    │
└──────────────────────────┬───────────────────────────────────┘
                           │ 依赖
                    ┌──────┴──────┐
                    │  IoBackend  │  ← 策略接口 (接口层)
                    │  (抽象类)    │    定义 12 个纯虚方法
                    └──────┬──────┘
           ┌───────────────┼───────────────┐
           │               │               │
  ┌────────┴────────┐ ┌────┴─────┐ ┌───────┴────────┐
  │  EvBackend      │ │UringBack │ │ DpdkBackend    │ ← 具体策略
  │  libev + epoll  │ │ io_uring │ │ DPDK + mTCP    │   (实现层)
  └────────┬────────┘ └────┬─────┘ └───────┬────────┘
           │               │               │
  ┌────────┴────────┐ ┌────┴─────┐ ┌───────┴────────┐
  │ 内核 socket API │ │内核socket│ │ mTCP 用户态 API │ ← 底层
  │ socket/accept/  │ │ io_uring │ │ mtcp_socket/    │   (系统层)
  │ close/setsockopt│ │ SQE/CQE  │ │ accept/close    │
  └─────────────────┘ └──────────┘ └─────────────────┘
```

### 8.2 每个文件是什么

| 文件 | 中文名 | 角色 | 干什么的 |
|------|--------|------|---------|
| `IoBackend.hpp` | **I/O 后端抽象接口** | 策略接口 | 定义 12 个纯虚方法。Worker 只认这个接口。新增后端只需加一个实现类 |
| `EvIoBackend.cpp/.hpp` | **libev 内核后端** | 具体策略 | 用内核 socket + libev epoll 实现接口。是默认后端，零外部依赖，永不作废 |
| `NativeUringIoBackend.cpp/.hpp` | **原生 io_uring 后端** | 具体策略 | 用内核 socket + io_uring 实现接口。listen/accept/close 跟 Ev 一样走内核 |
| `AsioUringIoBackend.cpp/.hpp` | **ASIO io_uring 后端** | 具体策略 | 用 ASIO 封装的 io_uring。同上，listen/accept/close 都是内核调用 |
| `DpdkIoBackend.cpp/.hpp` | **DPDK 用户态后端** | 具体策略 (新增) | 用 DPDK PMD + mTCP 用户态 TCP 实现全部 12 个方法。listen/accept/close 全部走 mTCP API |

### 8.3 为什么不需要中间基类

ev、native_uring、asio_uring 三个后端在 listen/accept/close/setsockopt 上的实现完全相同（都是内核系统调用包装）。理论上可以抽一个基类避免重复。

**但实际不需要**，因为每个方法只有 3-8 行：

```cpp
// EvIoBackend::CloseFd  — 就一行
void EvIoBackend::CloseFd(int fd) { ::close(fd); }

// NativeUringIoBackend::CloseFd — 同样一行
void NativeUringIoBackend::CloseFd(int fd) { ::close(fd); }
```

抽基类省 ~60 行代码但多一层继承关系，排查问题时多跳一次虚函数表，得不偿失。**允许少量重复，换取架构扁平**。

### 8.4 接口定义（IoBackend.hpp 完整版）
