# Thunder DPDK + mTCP 适配设计文档

> **版本**: v1.2
> **日期**: 2026-05-18
> **状态**: 设计阶段（新增第 10 章 Thunder 场景矩阵 + 第 11 章大厂参考）
> **目标**: 为 Thunder 增加 DPDK + mTCP 传输层选项，通过 `io_backend=dpdk` 配置开启，与现有 ev/io_uring 后端共存。

---

## 1. 背景与动机

### 1.1 当前架构

```
Thunder 当前 I/O 路径:
  Worker → IoBackend → libev epoll / io_uring → 内核 TCP 栈 → 网卡驱动 → NIC

  配置: io_backend = ev | native_uring | asio_uring
  特点: 依赖内核 TCP 栈，单核约 C100K 连接
```

### 1.2 为什么需要 DPDK + mTCP

| 场景 | 当前瓶颈 | DPDK+mTCP 收益 |
|------|---------|----------------|
| 极致 PPS (>5M pps/核) | 内核协议栈每包 syscall + 中断 | 用户态 TCP + PMD 轮询，10-20M pps/核 |
| C10M 长连接 | 内核 TCP 内存/CPU 开销 | 用户态轻量 TCP 栈，C10M 可行 |
| 微秒级尾部延迟 | 中断调度抖动 | 轮询模式延迟稳定 |
| 定制拥塞控制 | 内核固定算法 | 用户态可替换算法 |

### 1.3 设计原则

1. **配置化开关**：`io_backend=dpdk` 启用，不改代码
2. **接口兼容**：复用现有 `IoBackend` 抽象接口，Worker 改动最小化
3. **可回退**：DPDK 初始化失败 → 自动回退 ev 后端
4. **可共存**：同一进程内可同时有 DPDK 和 ev/io_uring 实例（不同 Worker）

---

## 2. 架构设计

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          Thunder Process                                 │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                    Worker (主线程)                                  │  │
│  │                                                                     │  │
│  │  ┌─────────┐    ┌───────────────┐    ┌────────────────────────┐  │  │
│  │  │ Manager │───→│  DpdkIoBackend│───→│      mTCP 用户态 TCP   │  │  │
│  │  │         │    │  (IoBackend   │    │  mtcp_socket/accept/    │  │  │
│  │  │         │    │   子类)        │    │  read/write/epoll       │  │  │
│  │  └─────────┘    └───────┬───────┘    └───────────┬────────────┘  │  │
│  │                         │                         │               │  │
│  │                         │          ┌──────────────┴───────────┐  │  │
│  │                         │          │     DPDK EAL + PMD       │  │  │
│  │                         │          │  (rte_eth_rx/tx_burst)   │  │  │
│  │                         │          └──────────────┬───────────┘  │  │
│  └─────────────────────────┼─────────────────────────┼───────────────┘  │
│                            │                         │                  │
│                            │    ★ 用户态              │  ★ 内核态       │
│                            │                         │                  │
│                      ┌─────┴─────┐            ┌──────┴──────┐          │
│                      │  libev     │            │   NIC        │          │
│                      │ (timer等)  │            │ (VFIO/UIO)   │          │
│                      └───────────┘            └─────────────┘          │
└─────────────────────────────────────────────────────────────────────────┘

  ★ DPDK 路径: 全程用户态，不经过内核 TCP/IP 栈
  ★ libev 保留: 用于 timer、signal、admin socket 等非 I/O 事件
```

### 2.2 与现有模式的对比

```
            ev                     native_uring                dpdk (新增)
         ─────────              ────────────────           ───────────────
传输层:   内核 TCP                内核 TCP                   mTCP 用户态 TCP
事件循环: libev epoll             libev epoll               mTCP epoll + libev
I/O 提交: ev_io watch            io_uring SQE              mtcp_read/write
buffer:   CBuffer                 CBuffer                   CBuffer (同)
fd 类型:  内核 fd                 内核 fd                   mTCP sockid (int)
CPU:      <100% (事件驱动)        <100% (事件驱动)          100% (轮询)
网卡:     内核驱动                内核驱动                  DPDK PMD (VFIO)
连接数:   ~C100K                  ~C100K                    ~C10M (理论)
```

---

## 3. 核心组件设计

### 3.1 DpdkContext — DPDK/mTCP 全局初始化

每个 Worker 一个 `DpdkContext`，管理 DPDK EAL 和 mTCP 上下文。

```cpp
// code/Net/src/labor/DpdkContext.hpp

#include <mtcp_api.h>
#include <rte_eal.h>
#include <rte_ethdev.h>

namespace net {

class DpdkContext
{
public:
    struct Config
    {
        // ── DPDK EAL ──
        int    ealArgc;
        char** ealArgv;             // e.g., "-l 0-3 -n 4 --proc-type=primary"
        int    nbPorts;
        int    nbRxQueues;
        int    nbTxQueues;
        int    mbufPoolSize;

        // ── mTCP ──
        int    maxConcurrency;      // 最大并发连接数
        int    maxNumBuffers;       // mTCP 缓冲池大小
        int    rcvBufSize;          // 接收缓冲 (默认 8192)
        int    sndBufSize;          // 发送缓冲 (默认 8192)
        int    tcpTimeout;          // TCP 超时 (秒)

        // ── CPU ──
        int    lcoreId;             // 本 Worker 绑定到哪个 CPU 核
    };

    bool Init(const Config& cfg);
    void Destroy();

    // mTCP 上下文
    mctx_t  GetCtx()    const { return m_mctx; }
    int     GetEpollFd() const { return m_epId; }

    static DpdkContext* Create(const Config& cfg);
    static void         Destroy(DpdkContext* ctx);

private:
    mctx_t m_mctx  = nullptr;      // mTCP per-core context
    int    m_epId  = -1;           // mTCP epoll id

    // 单例 — 整个进程一个 DPDK EAL 实例
    static bool s_ealInitialized;
};

} // namespace net
```

### 3.2 DpdkIoBackend — IoBackend 接口实现

```cpp
// code/Net/src/labor/DpdkIoBackend.hpp

#include "labor/IoBackend.hpp"
#include "DpdkContext.hpp"

namespace net {

class DpdkIoBackend : public IoBackend
{
public:
    DpdkIoBackend(DpdkContext* ctx);
    ~DpdkIoBackend() override;

    // ── IoBackend 接口 ──
    bool Init(struct ev_loop* loop, IoCompletionCallback cb, void* userData) override;
    void Destroy() override;
    bool SubmitRead(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq) override;
    bool SubmitWrite(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq) override;
    void CancelFd(int fd) override;
    const char* Name() const override { return "dpdk"; }
    bool HasPending(int fd) const override;

    // ── mTCP epoll 事件处理 ──
    void ProcessMtcpEvents();

    // ── 监听 socket 管理 ──
    int  CreateListenSocket(const char* ip, uint16_t port);
    void OnAccept(int listenSockid);

private:
    struct FdState
    {
        uint32_t seq          = 0;
        int      readPending  = 0;
        int      writePending = 0;
        bool     cancelled    = false;
    };

    bool RegisterFd(int fd);           // 将 mTCP sockid 注册到 mTCP epoll
    void DeregisterFd(int fd);
    void HandleRead(int fd);
    void HandleWrite(int fd);
    void HandleError(int fd);

    DpdkContext*        m_dpdkCtx    = nullptr;    // 不持有所有权
    struct ev_loop*     m_evLoop     = nullptr;
    IoCompletionCallback m_callback   = nullptr;
    void*               m_userData   = nullptr;

    // buffer 生命周期：Worker 的 pRecvBuff/pSendBuff 是 shared_ptr
    // 后端通过 SubmitRead/SubmitWrite 拿到 shared_ptr 副本
    // 但 mTCP 没有 PendingOp 机制 → 需要额外映射持有引用
    std::unordered_map<int, std::weak_ptr<util::CBuffer>> m_readBufs;
    std::unordered_map<int, std::weak_ptr<util::CBuffer>> m_writeBufs;
    std::unordered_map<int, FdState> m_fds;
};

} // namespace net
```

### 3.3 事件循环整合 — mTCP epoll + libev 共存

**核心问题**：mTCP socket 不是内核 fd，不能注册到 libev。但 Worker 还需要 libev 管理 timer、signal、admin socket 等。

**方案**：使用 libev 的 `ev_prepare`/`ev_check`/`ev_idle` 钩子桥接。

```
libev ev_run() 主循环:

  ┌─ ev_prepare 回调 (在 epoll_wait 之前)
  │   计算 mTCP 下一次 epoll 超时时间
  │
  ├─ epoll_wait(epfd, events, maxevents, timeout)
  │   libev 本身的 timer/signal/admin fd
  │
  ├─ ev_check 回调 (在 epoll_wait 之后)
  │   ★ ProcessMtcpEvents() — 非阻塞收割 mTCP 就绪事件
  │
  └─ ev_idle 回调 (当 libev 无其他就绪事件时)
       ★ mtcp_epoll_wait(…, timeout=1ms) — 阻塞等待 I/O
       mTCP 内部同时驱动 TCP 定时器 (重传/keepalive)
```

**关键**：mTCP 有自己的定时器（TCP 重传、keepalive 等），通过 `mtcp_epoll_wait(timeout)` 驱动。libev 的 timer 用于业务层定时任务。两者通过上述钩子桥接。

### 3.4 监听路径 — accept 流程

```cpp
// DpdkIoBackend 中的监听初始化

int DpdkIoBackend::CreateListenSocket(const char* ip, uint16_t port)
{
    mctx_t ctx = m_dpdkCtx->GetCtx();

    // ① 创建 mTCP socket
    int sockid = mtcp_socket(ctx, AF_INET, SOCK_STREAM, 0);
    if (sockid < 0) return -1;

    // ② 设置非阻塞 + 端口复用
    mtcp_setsock_nonblock(ctx, sockid);
    int on = 1;
    mtcp_setsockopt(ctx, sockid, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    // ③ bind + listen
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    mtcp_bind(ctx, sockid, (struct sockaddr*)&addr, sizeof(addr));
    mtcp_listen(ctx, sockid, 4096);

    // ④ 注册到 mTCP epoll
    int epid = m_dpdkCtx->GetEpollFd();
    struct mtcp_epoll_event ev;
    ev.events      = MTCP_EPOLLIN;
    ev.data.sockid = sockid;
    mtcp_epoll_ctl(ctx, epid, MTCP_EPOLL_CTL_ADD, sockid, &ev);

    return sockid;  // 返回 mTCP sockid 作为 Thunder 的 "fd"
}

void DpdkIoBackend::OnAccept(int listenSockid)
{
    mctx_t ctx = m_dpdkCtx->GetCtx();
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    int clientSockid = mtcp_accept(ctx, listenSockid,
                                   (struct sockaddr*)&addr, &len);
    if (clientSockid < 0) return;

    RegisterFd(clientSockid);  // 注册到 mTCP epoll

    // 直接回调 Worker — 同上层 accept 逻辑
    m_callback(clientSockid, m_fds[clientSockid].seq, IoOp::Read, 0, m_userData);
}
```

### 3.5 I/O 路径 — SubmitRead / SubmitWrite

与 io_uring 不同，mTCP 没有异步提交接口。SubmitRead/SubmitWrite 注册 epoll 事件，数据就绪后在 mTCP epoll 回调中执行实际 I/O。跟 EvIoBackend 模式类似，但 epoll 是 mTCP 用户态的。

```cpp
bool DpdkIoBackend::SubmitRead(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq)
{
    if (!buf || !m_callback) return false;

    auto& st = m_fds[fd];
    if (st.seq != seq) {
        st.seq = seq; st.readPending = st.writePending = 0; st.cancelled = false;
    }
    if (st.readPending > 0) return true;

    buf->EnsureWritableBytes(8192);
    m_readBufs[fd] = buf;  // weak_ptr 持有引用

    // 注册 EPOLLIN 到 mTCP epoll
    struct mtcp_epoll_event ev;
    ev.events      = MTCP_EPOLLIN;
    ev.data.sockid = fd;
    mctx_t ctx = m_dpdkCtx->GetCtx();
    mtcp_epoll_ctl(ctx, m_dpdkCtx->GetEpollFd(),
                   MTCP_EPOLL_CTL_MOD, fd, &ev);

    st.readPending++;
    return true;
}

bool DpdkIoBackend::SubmitWrite(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq)
{
    if (!buf || !m_callback) return false;

    int readable = static_cast<int>(buf->ReadableBytes());
    if (readable <= 0) {
        m_callback(fd, seq, IoOp::Write, 0, m_userData);
        return true;
    }

    auto& st = m_fds[fd];
    if (st.seq != seq) {
        st.seq = seq; st.readPending = st.writePending = 0; st.cancelled = false;
    }
    if (st.writePending > 0) return true;

    mctx_t ctx     = m_dpdkCtx->GetCtx();
    m_writeBufs[fd] = buf;

    // ★ 尝试立即同步写入
    const char* src = buf->GetRawReadBuffer();
    int sent = mtcp_write(ctx, fd, src, readable);

    if (sent > 0) {
        buf->AdvanceReadIndex(sent);
        if (sent == readable) {
            st.writePending = 0;
            m_callback(fd, seq, IoOp::Write, sent, m_userData);
            return true;
        }
        // 部分写完 → 注册 EPOLLOUT 等继续
    } else if (sent == 0) {
        m_callback(fd, seq, IoOp::Write, 0, m_userData);  // EOF
        return true;
    }
    // sent < 0: EAGAIN → 注册 EPOLLOUT

    struct mtcp_epoll_event ev;
    ev.events      = MTCP_EPOLLIN | MTCP_EPOLLOUT;
    ev.data.sockid = fd;
    mtcp_epoll_ctl(ctx, m_dpdkCtx->GetEpollFd(),
                   MTCP_EPOLL_CTL_MOD, fd, &ev);

    st.writePending++;
    return true;
}
```

### 3.6 mTCP epoll 事件处理

```cpp
void DpdkIoBackend::ProcessMtcpEvents()
{
    mctx_t ctx = m_dpdkCtx->GetCtx();
    int epid   = m_dpdkCtx->GetEpollFd();

    struct mtcp_epoll_event events[256];
    int n = mtcp_epoll_wait(ctx, epid, events, 256, 0);  // 非阻塞

    for (int i = 0; i < n; i++) {
        int fd = events[i].data.sockid;
        auto it = m_fds.find(fd);
        if (it == m_fds.end() || it->second.cancelled) continue;

        if (events[i].events & MTCP_EPOLLERR) {
            HandleError(fd); continue;
        }
        if (events[i].events & MTCP_EPOLLIN) {
            HandleRead(fd);
        }
        if (events[i].events & MTCP_EPOLLOUT) {
            HandleWrite(fd);
        }
    }
}

void DpdkIoBackend::HandleRead(int fd)
{
    auto it = m_fds.find(fd);
    if (it == m_fds.end()) return;

    auto bufIt = m_readBufs.find(fd);
    if (bufIt == m_readBufs.end()) return;
    auto buf = bufIt->second.lock();  // weak_ptr → shared_ptr
    if (!buf) return;

    mctx_t ctx = m_dpdkCtx->GetCtx();
    buf->EnsureWritableBytes(8192);
    char* dst  = const_cast<char*>(buf->GetRawWriteBuffer());
    size_t cap = buf->WriteableBytes();

    int n = mtcp_read(ctx, fd, dst, cap);
    if (n > 0) {
        buf->AdvanceWriteIndex(n);
        it->second.readPending--;
        m_callback(fd, it->second.seq, IoOp::Read, n, m_userData);
    } else if (n == 0) {
        m_callback(fd, it->second.seq, IoOp::Read, 0, m_userData);
    }
}
```

### 3.7 CBuffer 与 mTCP 的适配

mTCP 使用**连续内存**读写（`mtcp_read/write`），与 CBuffer 兼容。无需修改 CBuffer。

```
CBuffer 读写索引:
  recv: mtcp_read(fd, buf->GetRawWriteBuffer(), buf->WriteableBytes())
        → AdvanceWriteIndex(n)

  send: mtcp_write(fd, buf->GetRawReadBuffer(), buf->ReadableBytes())
        → AdvanceReadIndex(n)
```

---

## 4. 配置设计

### 4.1 配置方式：JSON 配置文件 + 环境变量

```json
// deploy/HelloHttp/config.json
{
    "io_backend": "dpdk",

    "dpdk": {
        "eal_args": "-l 0-3 -n 4",
        "port_id": 0,
        "nb_rx_queues": 1,
        "nb_tx_queues": 1,
        "mbuf_pool_size": 8192
    },

    "mtcp": {
        "max_concurrency": 1000000,
        "max_num_buffers": 1000000,
        "rcv_buf_size": 8192,
        "snd_buf_size": 8192,
        "tcp_timeout": 30
    }
}
```

```bash
# 环境变量覆盖（可选）
THUNDER_IO_DPDK=1               # 强制启用 DPDK
THUNDER_DPDK_LCORE=2            # Worker 绑定 CPU 2
THUNDER_DPDK_PORT=0             # 使用 port 0
```

### 4.2 InitIoBackend 改动

在 `Labor::InitIoBackend()` 中新增 `dpdk` 分支：

```cpp
// code/Net/src/labor/Labor.cpp: InitIoBackend

bool Labor::InitIoBackend(const util::CJsonObject& oJsonConf,
                          IoCompletionCallback callback)
{
    // ... 清理旧后端 ...

    std::string strBackend;
    oJsonConf.Get("io_backend", strBackend);

    if (strBackend == "dpdk")
    {
#ifdef THUNDER_IO_DPDK
        // ① 解析 DPDK/mTCP 配置
        DpdkContext::Config cfg;
        ParseDpdkConfig(oJsonConf, cfg);

        // ② 创建全局 DPDK 上下文（整个进程共享 EAL）
        DpdkContext* dpdkCtx = DpdkContext::Create(cfg);
        if (!dpdkCtx) {
            LOG4_WARN("DpdkContext init failed, falling back to ev");
            goto fallback_ev;
        }

        // ③ 创建 DpdkIoBackend
        DpdkIoBackend* pBackend = new DpdkIoBackend(dpdkCtx);
        if (pBackend && pBackend->Init(m_loop, callback,
                                       static_cast<void*>(this)))
        {
            m_pIoBackend   = pBackend;
            m_pDpdkContext = dpdkCtx;   // ★ Labor 新增成员
            LOG4_INFO("IoBackend: dpdk initialized successfully");
            return true;
        }
        delete pBackend;
        DpdkContext::Destroy(dpdkCtx);
        LOG4_WARN("DpdkIoBackend init failed, falling back to ev");
#else
        LOG4_WARN("dpdk requested but THUNDER_IO_DPDK not compiled");
#endif
    }

    // ... asio_uring / native_uring / ev ...
}
```

### 4.3 CMake 编译选项

```cmake
# CMakeLists.txt
option(THUNDER_IO_DPDK "启用 DPDK + mTCP I/O 后端" OFF)

if(THUNDER_IO_DPDK)
    add_compile_definitions(THUNDER_IO_DPDK)

    # DPDK (使用 pkg-config)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DPDK REQUIRED libdpdk)

    # mTCP (手动指定路径或子模块)
    set(MTCP_ROOT "${THUNDER_3PARTY}/mtcp" CACHE PATH "mTCP root")
    set(MTCP_INCLUDE "${MTCP_ROOT}/mtcp/src/include")
    set(MTCP_LIB     "${MTCP_ROOT}/mtcp/lib/libmtcp.a")

    # 新增源文件
    add_library(DpdkBackend STATIC
        code/Net/src/labor/DpdkContext.cpp
        code/Net/src/labor/DpdkIoBackend.cpp
    )
    target_include_directories(DpdkBackend PRIVATE
        ${DPDK_INCLUDE_DIRS}
        ${MTCP_INCLUDE}
    )
    target_link_libraries(DpdkBackend
        ${DPDK_LIBRARIES}
        ${MTCP_LIB}
        pthread numa dl
    )

    # 主程序链接
    list(APPEND THUNDER_EXTRA_LIBS DpdkBackend)
endif()
```

---

## 5. Worker 改动

### 5.1 改动点汇总

| 位置 | 现有逻辑 | DPDK 模式改动 |
|------|---------|--------------|
| 事件循环 | `ev_run()` 纯 libev | ★ 桥接 mTCP epoll (`ev_prepare`/`ev_check`/`ev_idle`) |
| 创建监听 | `socket()` + `bind()` + `listen()` | ★ `mtcp_socket()` + `mtcp_bind()` + `mtcp_listen()` |
| Accept | `accept4(fd, …)` | ★ `mtcp_accept(ctx, fd, …)` |
| Read/Write | `SubmitRead` / `SubmitWrite` | 同接口，`DpdkIoBackend` 内部走 mTCP |
| 关闭连接 | `close(fd)` + `CancelFd(fd)` | `mtcp_close(ctx, fd)` + `CancelFd(fd)` |
| Buffer 生命周期 | `shared_ptr<CBuffer>` | 相同，无需改动 |
| Timer/Signal | libev | 不变，libev 保留 |

### 5.2 事件循环桥接骨架

```cpp
// Worker 初始化时设置 libev 钩子
void Worker::SetupDpdkBridge()
{
    if (!m_pDpdkContext) return;

    // ev_check: 每次 epoll_wait 后收割 mTCP 事件
    ev_check_init(&m_checkWatcher, OnCheckDpdk);
    m_checkWatcher.data = this;
    ev_check_start(m_loop, &m_checkWatcher);

    // ev_idle: 当没有 libev 事件时，阻塞在 mTCP epoll
    ev_idle_init(&m_idleWatcher, OnIdleDpdk);
    m_idleWatcher.data = this;
    ev_idle_start(m_loop, &m_idleWatcher);
}

static void OnCheckDpdk(struct ev_loop*, ev_check* w, int)
{
    auto* worker = static_cast<Worker*>(w->data);
    auto* be = static_cast<DpdkIoBackend*>(worker->m_pIoBackend);
    be->ProcessMtcpEvents();  // 非阻塞收割
}

static void OnIdleDpdk(struct ev_loop* loop, ev_idle* w, int)
{
    auto* worker = static_cast<Worker*>(w->data);
    auto* be = static_cast<DpdkIoBackend*>(worker->m_pIoBackend);
    // 阻塞等待 mTCP 事件，超时 1ms 驱动 mTCP 内部定时器
    be->ProcessMtcpEventsWithTimeout(1);
}
```

---

## 6. 数据流总览

### 6.1 收包路径

```
NIC → DPDK PMD (rte_eth_rx_burst)
   → mTCP 用户态 TCP 栈 (处理 ACK/重传/拥塞控制)
   → mtcp_epoll 标记 sockid 为 MTCP_EPOLLIN
   → Worker 事件循环: ev_check → ProcessMtcpEvents()
   → mtcp_read(ctx, sockid, CBuffer.WritePtr, …)
   → AdvanceWriteIndex(n) → m_callback(Read)
   → Worker: OnIoComplete → 业务处理
```

### 6.2 发包路径

```
Worker: DoSend → SubmitWrite(sockid, pSendBuff, seq)
   → DpdkIoBackend::SubmitWrite
      ├─ mtcp_write(ctx, sockid, src, len)   // 立即尝试
      │   └─ 部分成功 → AdvanceReadIndex(sent)
      │   └─ EAGAIN → 注册 MTCP_EPOLLOUT
   → mTCP 用户态 TCP 栈 (拥塞控制/分段/重传)
   → DPDK PMD (rte_eth_tx_burst) → NIC DMA
```

---

## 7. 现有架构冲突与兼容性分析

> 基于 Thunder dev 分支 `IoBackend.hpp`、`Labor.cpp`、`Worker.cpp` 逐代码位置分析。

### 7.1 当前 I/O 路径（关键代码位置）

```
InitClientListener (Worker.cpp:2387-2418)
  → socket() + setsockopt(SO_REUSEPORT) + bind() + listen()    ← 内核调用，不经 IoBackend

AcceptClientConn (Worker.cpp:935-955)
  → accept(iFd, …)                                               ← 内核调用，不经 IoBackend

I/O 读写 (Worker.cpp:1208/1336)
  → m_pIoBackend->SubmitRead(fd, buf, seq)                       ← ✅ IoBackend 接口
  → m_pIoBackend->SubmitWrite(fd, buf, seq)                      ← ✅ IoBackend 接口

连接销毁 — DestroyConnect (Worker.cpp:5330-5384)
  → m_pIoBackend->CancelFd(fd)                                   ← ✅ IoBackend 接口
  → close(fd)                                                    ← ❌ 直接内核调用！
  → DelEvent(pIoWatcher) / DelEvent(pTimeWatcher)                ← libev，不变

连接属性设置 — SetSocketAttr (Labor.cpp)
  → setsockopt(TCP_NODELAY/SO_KEEPALIVE/…)                       ← ❌ 直接内核调用！

对端地址查询 — Fd2Address (Labor.cpp)
  → getpeername(fd, …)                                           ← ❌ 直接内核调用！

Manager IPC 通信 (Worker.cpp)
  → iManagerControlFd / iManagerDataFd (socketpair)              ← ⚠️ 必须保留内核路径
```

**关键发现**：只有 `SubmitRead` / `SubmitWrite` / `CancelFd` 三个方法走了 `IoBackend` 抽象。**listen socket 创建、accept、close、setsockopt、getpeername 全部是直接内核系统调用，不经过 IoBackend。**

### 7.2 硬冲突 — 7 处需改造的代码位置

| # | 代码位置 | 当前做法 | DPDK 下需改为 | 冲突程度 |
|---|---------|---------|-------------|---------|
| 1 | **Listen 创建** `Worker.cpp:2387-2418` | `socket()` → `setsockopt(SO_REUSEPORT)` → `bind()` → `listen()` | `mtcp_socket()` → `mtcp_bind()` → `mtcp_listen()`。mTCP 无 `SO_REUSEPORT` 等价功能，多 Worker 共端口需 RSS 分流 | **硬冲突** |
| 2 | **Accept** `Worker.cpp:939` | `accept(iFd, (sockaddr*)&addr, &len)` | `mtcp_accept(mctx, sockid, …)`。需要 `mctx` 上下文参数，且返回值是 mTCP sockid 而非内核 fd | **硬冲突** |
| 3 | **close()** `Worker.cpp:5384` `DestroyConnect` 末尾 | `::close(fd)` | 若 fd 是 mTCP sockid，内核 close 返回 EBADF 且 mTCP 资源泄漏。须用 `mtcp_close(mctx, sockid)` | **硬冲突** — 静默泄漏 |
| 4 | **SetSocketAttr** `Labor.cpp` TCP_NODELAY/SO_KEEPALIVE | `setsockopt()` | mTCP 有 `mtcp_setsockopt()` 但选项子集不全。`TCP_CORK`、`TCP_QUICKACK` 等不支持 | **硬冲突** — 需包装+降级 |
| 5 | **Fd2Address** `Labor.cpp` | `getpeername()` | mTCP 无等价函数。须在 accept 时缓存 `sockaddr_in` 到 `tagConnectionAttr` | **中冲突** — 可缓存绕过 |
| 6 | **Manager IPC fd** `Worker.cpp` `iManagerControlFd/DataFd` | 内核 `socketpair` | 父子进程 IPC 必须保留内核路径，不能切到 mTCP | **不能动的** — 需双栈共存 |
| 7 | **fd 命名空间** `mapFdAttr` 用 `int fd` 做 key | 内核 fd 全局唯一 | mTCP sockid 也是 `int`，可能和内核 fd 碰撞（如 mTCP sockid=5 碰内核 fd=5） | **中冲突** — 需偏移或加标记位 |

> 最致命的是第 3 点和第 7 点：`DestroyConnect` 对 mTCP sockid 执行内核 `close()` 会静默失败但不报错，mTCP 连接资源永久泄漏。fd 命名空间碰撞导致 `mapFdAttr` 查找逻辑错乱。

### 7.3 兼容的部分 — 不需要改的

| 组件 | 兼容性 | 说明 |
|------|--------|------|
| **IoBackend 接口** | ✅ 完美兼容 | `Init/Destroy/SubmitRead/SubmitWrite/CancelFd/HasPending/Name` 7 个纯虚方法，mTCP 均可对等实现 |
| **CBuffer** | ✅ 无需改动 | mTCP 使用连续内存 `mtcp_read/write`，直接对接 `CBuffer::GetRawReadBuffer/GetRawWriteBuffer` + `AdvanceReadIndex/AdvanceWriteIndex` |
| **OnIoComplete 回调** | ✅ 签名一致 | `void (int fd, uint32_t seq, IoOp op, int result, void* user_data)` — fd 传入 mTCP sockid 即可 |
| **tagConnectionAttr** | ✅ `iFd` 可复用 | struct 的 `int iFd` 存储 mTCP sockid，上层无感知（前提是 fd 命名空间不冲突） |
| **Codec 层** | ✅ 完全无感知 | Codec 只操作 `CBuffer`，不碰 socket fd |
| **libev timer/signal** | ✅ 不变 | `ev_timer`（业务超时/心跳）、`ev_signal`（信号处理）、`ev_async`（跨线程投递）全部保留 |
| **事件循环桥接模式** | ✅ 与 AsioUring 一致 | `ev_check` + `ev_idle` 桥接 mTCP epoll，和现有 `AsioUringIoBackend` 同一模式 |
| **业务逻辑** | ✅ 不受影响 | Session/Step/HttpAttr 等上层抽象不依赖 socket fd |

**结论**：数据面（编解码 → CBuffer → SubmitRead/Write → OnIoComplete）可以无缝切换。**控制面**（listen/accept/close/setsockopt/getpeername）需要按后端分流。

### 7.4 改造方案 — 需要改动的代码

如果要支持 `io_backend=dpdk`，至少需要改 **6 个位置，总改动量约 100 行**（不是上万行）：

```
① InitIoBackend (Labor.cpp:447)
   → 在现有 if/else 链新增 "dpdk" 分支
   → DpdkContext::Create → DpdkIoBackend::Init
   改动: ~30 行

② InitClientListener (Worker.cpp:2380)
   → 分流: if (is_dpdk) → DpdkIoBackend::CreateListenSocket()
            else → 原 socket/bind/listen 内核路径
   改动: ~10 行

③ AcceptClientConn (Worker.cpp:935)
   → 分流: if (is_dpdk) → mtcp_accept(mctx, …)
            else → accept(iFd, …)
   改动: ~15 行

④ DestroyConnect ::close() (Worker.cpp:5384)
   → 分流: if (is_dpdk) → mtcp_close(mctx, sockid)
            else → ::close(fd)
   改动: ~5 行

⑤ SetSocketAttr (Labor.cpp)
   → 分流: if (is_dpdk) → mtcp_setsockopt 子集 (含降级)
            else → setsockopt
   改动: ~10 行

⑥ Fd2Address (Labor.cpp)
   → 分流: if (is_dpdk) → 从 accept 时缓存的 sockaddr_in 取
            else → getpeername(fd, …)
   改动: ~5 行

⑦ fd 命名空间防碰撞
   → 方案 A: mTCP sockid 偏移 10,000,000 (mtcp 分配范围 0-N, 内核 fd 通常 <1000)
   → 方案 B: tagConnectionAttr 加 bool m_bIsMtcp 标记
   改动: ~10 行 (推荐方案 A, 零侵入)
```

### 7.5 依赖清单

**编译依赖：**

| 依赖 | 大小 | 用途 |
|------|------|------|
| `libdpdk.so` | ~50MB | DPDK EAL + PMD 轮询模式驱动 |
| `libmtcp.a` | ~2MB | 用户态 TCP 协议栈 |
| `libnuma` | ~50KB | NUMA 感知内存分配 |
| `libpthread` | 系统自带 | mTCP 内部线程同步 |

**运行时依赖（系统级，不可省略）：**

| 依赖 | 量级 | 说明 |
|------|------|------|
| **hugepages** (2MB/1GB) | 4–8GB | 大页内存预分配，减少 TLB miss |
| **VFIO/UIO 驱动** | — | 网卡从内核解绑，绑定到 DPDK PMD |
| **独占物理网卡** | 1 端口 | DPDK PMD 绑定的 NIC，内核不可再用 |
| **`isolcpus`** (内核启动参数) | 1+ 核 | CPU 隔离，防止内核调度抢占 DPDK 轮询线程 |
| **`CAP_SYS_ADMIN` / root** | — | 大页分配 + 网卡绑定需要特权 |

**不可用的内核工具链：**

| 工具 | 替代方案 |
|------|---------|
| `tcpdump` | DPDK `pdump` |
| `ss -tnp` / `netstat` | mTCP 内置统计接口 |
| `/proc/net/tcp` | 无等价物 |
| `iptables / netfilter` | 需用户态自建 ACL |

### 7.6 功能限制

| 限制 | 影响范围 | 是否有解 |
|------|---------|---------|
| **Manager IPC 必须走内核** | 父子进程通信的 `socketpair` 不能走 mTCP | ✅ 双栈共存：listen/业务走 mTCP，IPC 走内核 |
| **`SO_REUSEPORT` 无等价物** | 多 Worker 共享同一端口需 DPDK RSS 分流 | ⚠️ 需要 DPDK flow director 或 mTCP 层做分发 |
| **TCP socketopt 子集不全** | `TCP_CORK`、`TCP_QUICKACK` 等不支持 | ⚠️ 按需降级实现 |
| **无 `fork()` 支持** | DPDK 初始化后大页+设备绑定锁死进程地址空间 | ❌ 无解，Thunder 本身单进程多 Worker 模式不受影响 |
| **100% CPU 轮询** | 空闲也占满一个核，`top` 无法判断真实负载 | ⚠️ 需用 DPDK 内部 xstats 判断负载 |
| **`getpeername` 不可用** | 对端地址须在 accept 时缓存 | ✅ 可缓存绕过 |
| **容器/云环境不可用** | Docker bridge/overlay 网络走不了 DPDK，需 host 网络或 SR-IOV | ⚠️ 物理机/裸金属部署限定 |

### 7.7 与 AF_XDP 的关系

AF_XDP 和 DPDK+mTCP 不是递进关系，而是**两条平行的内核旁路路线**：

| | AF_XDP | DPDK + mTCP |
|------|--------|-------------|
| **旁路层级** | 仅 L2 数据面（包收发） | L2 + L3 + L4 全部旁路 |
| **TCP 栈** | **内核 TCP** (保留 CUBIC/BBR/拥塞控制/… 全部能力) | 用户态 mTCP（自实现，功能子集） |
| **socket 类型** | 普通内核 socket（AF_XDP 只替换底层包 I/O） | mTCP sockid（非内核 fd） |
| **listen/accept/close** | **不变**，照用内核调用 | **必须全部改为 mTCP API** |
| **容器兼容** | ⚠️ 部分兼容 | ❌ 几乎不可用 |
| **网卡要求** | 需 XDP 驱动支持（`mlx5`/`i40e`/`ice` 等，`e1000e`/`mt7921e` 不支持） | 需 DPDK PMD 支持 |
| **代码冲突** | **极少**（只替换数据面，其他内核调用不变） | **多**（替换整个协议栈生命周期） |
| **当前 Thunder 硬件可行？** | ❌ 当前网卡不支持 zcrx（brcmfmac/mt7921e/e1000e 均不支持） | 取决于目标部署硬件的 DPDK 兼容性 |

> **AF_XDP 的优势**在于保留内核 TCP 栈 → 和 Thunder 现有代码冲突极小（只需改 SubmitRead/SubmitWrite 底下的包 I/O 路径，listen/accept/close/setsockopt 全不用动）。**但当前开发机网卡不支持**，仅作为中期演进方向保留。

### 7.8 性能预期

| 指标 | ev (baseline) | native_uring+SQPOLL | dpdk+mTCP |
|------|------|------|------|
| C10K PPS (64B 小包) | ~1.5M | ~4M | ~10M |
| C10K P99 延迟 | ~50μs | ~15μs | ~5μs |
| CPU (单核) | ~70% | ~55% | 100% ★ |
| 最大连接数 | ~100K | ~100K | ~1M+ |
| 吞吐 (64KB 大包) | ~5Gbps | ~8Gbps | ~10Gbps (线速) |

### 7.9 决策指南

```
✅ 适合 dpdk+mTCP:
  - 需要 C10M 级别连接数
  - 单核 PPS > 5M
  - 有闲置 CPU 核可独占
  - 物理机部署，可绑定网卡
  - 有自己的运维团队管 hugepages/CPU 隔离

❌ 不适合:
  - 容器/云环境（没 VFIO、没 hugepages 控制权）
  - CPU 紧张（DPDK 吃满 100%）
  - 开发阶段/快速迭代（调试困难，tcpdump/ss 不可用）
  - 需要和内核工具配合排障
```

---

## 8. 实施计划

| 阶段 | 内容 | 预计工时 |
|------|------|----------|
| Phase 1 | DpdkContext — DPDK EAL + mTCP 初始化/销毁 | 2d |
| Phase 2 | DpdkIoBackend — 实现 IoBackend 接口 + mTCP epoll 整合 | 3d |
| Phase 3 | Worker 改动 — 事件循环桥接 + accept/listen 路径替换 | 2d |
| Phase 4 | CMake 构建 — 条件编译 + 依赖检测 + 子模块 | 1d |
| Phase 5 | 集成测试 — HelloHttp 在 DPDK 模式下端到端 | 2d |
| Phase 6 | 性能基准 — 与 ev/uring 模式对比 PPS/延迟/CPU | 1d |
| **合计** | | **~11d** |

---

## 9. 用户态 TCP 替代方案对比

### 9.1 有没有改动更小的方案？

**核心结论：不存在"改动小"的用户态 TCP 方案。** 原因在于：一旦把 TCP 栈从内核搬到用户态，`socket fd` 就不再是内核 fd。`socket()/bind()/listen()/accept()/read()/write()/close()/setsockopt()/getsockname()/getpeername()` 这 10 个系统调用**全部**需要替换为对应库的 API。无论选哪个库，改动量相当。

以下是所有主流用户态 TCP 方案的对比：

| 方案 | TCP 栈来源 | 依赖 | 成熟度 | 改动量 | 特点 |
|------|-----------|------|--------|--------|------|
| **mTCP** | 自研用户态 | DPDK | ⭐⭐⭐ 中（学术项目） | 中等 | 简单、轻量，但功能子集不全（被韩国 KAIST 收购后维护放缓） |
| **F-Stack** | FreeBSD TCP 栈 | DPDK | ⭐⭐⭐⭐ 较高（腾讯开源） | 中等 | 完整的 FreeBSD TCP（含 BBR/ECN/TFO），功能最全。posix socket 兼容层 |
| **Seastar** | 自研 native stack | DPDK / 可选 | ⭐⭐⭐⭐⭐ 高（ScyllaDB 生产用） | 大 | 完整 C++ 异步框架，换的不只是 TCP，是整个并发模型 |
| **VPP/TLDK** | 自研 | DPDK | ⭐⭐⭐⭐ 较高（FD.io/LF 项目） | 大 | Cisco VPP 的传输层组件，嵌入式/电信场景 |
| **OpenOnload** | 内核 TCP 移植 | 无 DPDK，直接接管网卡 | ⭐⭐⭐⭐ 高（Solarflare/Xilinx） | 小 | LD_PRELOAD 拦截，源码不变！但只支持 Solarflare 网卡 |
| **DPVS** | 内核 TCP (DPDK 移植) | DPDK | ⭐⭐⭐ 中（爱奇艺开源） | 大 | 基于 DPDK 的 L4 负载均衡器，内置简化 TCP 栈 |
| **io_uring** | 内核 TCP | **无** | ⭐⭐⭐⭐⭐ 极高 | **0 行** | 不是用户态 TCP。批量 syscall，Thunder 已落地 |

### 9.2 唯一"改动小"的方案：OpenOnload（但不通用）

OpenOnload 是**唯一**不需要改源码的用户态 TCP，通过 `LD_PRELOAD` 拦截所有 socket 调用：

```bash
LD_PRELOAD=libonload.so ./thunder_worker  # socket/bind/listen/accept/ 全被接管
```

但它有致命限制：
- **只支持 Solarflare（现 Xilinx/AMD）网卡**，其他厂商不支持
- 不开源核心协议栈（TCP 栈闭源，只有 API 拦截层开源）
- 依赖专用硬件，成本高

对其他网卡（Intel、Mellanox、Broadcom），**不存在 LD_PRELOAD 级别的零改动方案**。

### 9.3 Thunder 的务实路线（改动量从小 → 大）

```
改动量 0 行:   io_uring registered buffers     ← 已分析可行，数据路径优化
改动量 50 行:  io_uring MSG_ZEROCOPY           ← 大包发送零拷贝
改动量 100 行: AF_XDP + 内核 TCP               ← 只旁路包 I/O，保留 TCP 栈
改动量 100 行: DPDK + mTCP / F-Stack           ← 全旁路，但需改 6 处控制面代码
改动量 5000+行: Seastar / VPP                  ← 换整个框架
```

这些方案**不是互斥的** — io_uring → AF_XDP → DPDK 可以逐步演进，每步都有独立的性能收益。

### 9.4 mTCP 的好处（为什么不直接用内核 TCP）

| 收益 | 原理 | 量化 |
|------|------|------|
| **零 syscall** | 用户态收发包不经过内核 | 内核 TCP 每包至少 2 次 syscall (read+write)，mTCP 0 次 |
| **可预测延迟** | 无内核调度 jitter、无中断 | P99 延迟 ~5μs vs 内核 ~50μs |
| **C10M 连接** | 用户态轻量 PCB，无内核 socket 内存开销 | 内核 ~100K/核，mTCP 10M+/核 |
| **定制拥塞控制** | 替换 mTCP 的 CC 模块 | 内核只能选 CUBIC/BBR，mTCP 可自研 |
| **真实零拷贝** | NIC DMA 直写用户态 mbuf，不经内核 skb | 内核路径至少 1 次 memcpy (内核↔用户态) |
| **批量操作** | 轮询模式一次 rte_eth_rx_burst 收割多个包 | 比 epoll 逐个通知高效 |
| **CPU 独占确定性** | 绑核 + 轮询排除一切外界干扰 | 无上下文切换、无 cache 污染 |

### 9.5 为什么对 Thunder 是过度设计

Thunder 的定位是**分布式 RPC 服务框架**（对标 Brpc/Tars/Dubbo），不是网络转发面（NGINX/Envoy/HAProxy）：

```
网络代理的瓶颈:  包转发速率 (pps)、L4 连接数
Thunder 的瓶颈:  JSON 解析、protobuf 序列化、数据库查询、业务逻辑

用 DPDK 把网络延迟从 50μs 压到 5μs，省下 45μs，
但一次 MySQL 查询 500μs → 省下的 45μs 被淹没在 500μs 里，
总延迟几乎不变。
```

io_uring 已经解决了 Thunder 真正的 I/O 瓶颈：**批量 syscall + 异步完成通知**。64KB 大包下 asio_uring 的吞吐只比 ev 高 +7.5%（6,675 vs 6,207 RPS），说明瓶颈已从 I/O 转移到 CPU/内存带宽。DPDK 能再提 10M pps，但 Thunder 的单核业务处理上限根本到不了 10M pps — codec 层才是天花板。

---

## 10. Thunder + DPDK/mTCP 可用场景分析

> **背景**：Thunder 的 Interface 节点类型本身就是"接入网关"——客户端连接、协议解析、消息转发。
> DPDK 不是"能不能用"的问题，而是"什么场景下收益超过代价"的决策。

### 10.1 场景总览

Thunder 作为分布式 RPC 框架，可能的部署角色：

```
┌─────────────────────────────────────────────────────────────────┐
│  Thunder 部署角色                                                 │
│                                                                  │
│  ① API 网关 (Edge Gateway)     ← 对外流量入口                       │
│  ② L7 反向代理 (Reverse Proxy) ← 协议转换 + 路由                    │
│  ③ 微服务 Sidecar             ← 服务网格数据面                     │
│  ④ 内部 RPC 路由 (Interface)   ← 服务间通信转发                     │
│  ⑤ 长连接网关 (WebSocket)      ← 实时消息推送                       │
│  ⑥ 边缘/CDN 节点              ← 内容缓存 + 就近接入                  │
│  ⑦ 游戏/TCP 接入网关           ← 低延迟、长连接、自定义协议           │
│  ⑧ 业务逻辑服务 (Logic)        ← 纯业务处理，不面向客户端流量          │
└─────────────────────────────────────────────────────────────────┘
```

### 10.2 逐场景分析

#### 场景 ① API 网关 (Edge Gateway)

```
Client → Internet → Thunder(Interface) → Logic → ...
```

| 维度 | 分析 |
|------|------|
| **连接数** | C1K–C10K（典型企业 API 网关） |
| **PPS** | < 50K pps/核 |
| **瓶颈** | TLS 握手、JWT 验证、限流、路由匹配 — **CPU 密集而非 I/O 密集** |
| **io_uring 够不够** | ✅ **完全够**。当前 asio_uring 已在 100 并发下到 6,675 RPS (64KB)，C10K 场景轻松覆盖 |
| **DPDK 收益** | 低。API 网关的瓶颈是业务逻辑（认证/限流/路由），不在包转发。C10K 连接数内核 TCP 绰绰有余 |
| **DPDK 代价** | 独占网卡 + hugepages + CPU 绑核 → 运维成本 >> 收益 |
| **推荐** | **io_uring**（当前 asio_uring 即可） |

#### 场景 ② L7 反向代理 / 负载均衡

```
Client → Thunder(L7 Proxy) → Backend1/Backend2/...
         协议: HTTP→gRPC 转换, 请求改写, 灰度路由
```

| 维度 | 分析 |
|------|------|
| **连接数** | C10K–C100K（代理所有后端流量） |
| **PPS** | 50K–200K pps/核 |
| **瓶颈** | 后端连接池管理、请求转发、响应聚合 — I/O 等待为主 |
| **io_uring 够不够** | ⚠️ **临界场景**。C100K 连接下 epoll 扫描开销开始显著（epoll LT 活跃连接全部返回）；io_uring 批量 CQE 可缓解但不是根本解决 |
| **DPDK 收益** | **中等**。C100K+ 连接数下内核 socket 内存开销开始成为瓶颈；mTCP 用户态 PCB 更轻量；批量收发包消除 syscall |
| **DPDK 代价** | 中等 — 物理机部署是硬需求；如果已经在用云 SLB，Thunder 本身不需要再做 L7 代理 |
| **推荐** | **先上 io_uring + registered buffers**，连接数 >50K 再评估 DPDK |

#### 场景 ③ 微服务 Sidecar

```
App ←localhost→ Thunder(Sidecar) → Mesh Network → Backend
```

| 维度 | 分析 |
|------|------|
| **连接数** | C10–C100（单 pod 内） |
| **PPS** | < 5K pps/核 |
| **瓶颈** | CPU（编解码）+ 内存（sidecar 资源受限） |
| **io_uring 够不够** | ✅ **完全够**。sidecar 流量远到不了需要 DPDK 的级别 |
| **DPDK 收益** | **零。** Sidecar 跑在容器里，根本无法绑物理网卡。连基本前提都不满足 |
| **推荐** | **io_uring**（甚至 ev/epoll 都够） |

#### 场景 ④ 内部 RPC 路由 (Interface)

```
Thunder Worker → Center → Thunder(Interface) → Logic → DB/Cache
```

| 维度 | 分析 |
|------|------|
| **连接数** | C10–C100（Server 间长连接复用） |
| **PPS** | < 10K pps/核 |
| **瓶颈** | 业务逻辑 + DB/Cache 查询（每个请求可能多步 Step 链） |
| **io_uring 够不够** | ✅ **完全够**。Server 间通信走长连接池，连接数少 |
| **推荐** | **io_uring** 或 ev |

#### 场景 ⑤ 长连接网关 — WebSocket/SSE

```
Browser/App ←WS→ Thunder(Interface) ←→ Logic ←→ Push/MQ
  连接数: C100K–C1M, 心跳为主, 偶发消息
```

| 维度 | 分析 |
|------|------|
| **连接数** | C100K–C1M（大量空闲连接） |
| **PPS** | < 10K pps/核（大部分时间只有心跳） |
| **瓶颈** | **连接数 > 网络吞吐**。内核 socket 每连接约 3-4KB 内存，100 万连接 ≈ 3-4GB 仅 socket 开销 |
| **io_uring 够不够** | ⚠️ **边界场景**。100 万连接下 epoll 注册/遍历本身成为瓶颈。但 io_uring 不解决 socket 内存问题 — 内核 socket 开销是内核决定的 |
| **DPDK 收益** | **高**（仅针对连接数维度）。mTCP 用户态 PCB 每连接不到 1KB，C1M 连接仅需 ~1GB vs 内核 3-4GB。但注意：Thunder 当前单进程 `mapFdAttr` 也 hold 了连接元数据，C1M 连接本身也吃 1GB+ 应用层内存 |
| **DPDK 代价** | 高 — C1M 连接需要至少 4-8GB hugepages + 独占网卡 + CPU 持续轮询 |
| **推荐** | **io_uring + 内核 socket 优化**（`tcp_tw_reuse`、`tcp_max_orphans`、`net.core.somaxconn`）。真到 C1M 级别不是加 DPDK 就能解决的 — 整个连接管理（mapFdAttr、codec、session）都需要重构 |

#### 场景 ⑥ 边缘/CDN 节点

```
Client → CDN Edge(Thunder) → [命中] 本地缓存直接返回
                           → [未命中] 回源站
```

| 维度 | 分析 |
|------|------|
| **连接数** | C10K–C100K |
| **PPS** | 100K–500K pps/核（小文件/CDN 场景包量极大） |
| **瓶颈** | **包转发 + 磁盘 I/O**。CDN 场景下小对象多、PPS 极高 |
| **io_uring 够不够** | ❌ **不够**。C100K PPS 级别内核 TCP 的 per-packet syscall 开销成为主导瓶颈 |
| **DPDK 收益** | **高**。PPS 从 ~200K 提升到 ~5M+/核。大量小文件返回时包率高，DPDK 批量收发包是关键 |
| **DPDK 代价** | 高 — 但 CDN 场景通常是物理机部署，运维条件满足 |
| **推荐** | **DPDK + mTCP + io_uring（文件 I/O）**。网络 I/O 走 DPDK，磁盘 I/O 走 io_uring，各取所长 |

#### 场景 ⑦ 游戏/TCP 长连接网关

```
GameClient ←TCP→ Thunder(GameGateway) ←→ GameServer
  自定义二进制协议, 高频小包 (< 100B), 低延迟 (< 10ms P99)
```

| 维度 | 分析 |
|------|------|
| **连接数** | C10K–C100K |
| **PPS** | 100K–1M pps/核（玩家操作频繁，高频小包） |
| **瓶颈** | **小包 PPS + 尾延迟**。每个玩家操作产生几十字节的包，内核 per-packet syscall → 中断 → 上下文切换代价极高 |
| **io_uring 够不够** | ❌ **不够**。小包 PPS > 200K 时，即使批量提交，内核 TCP 的 per-packet 处理仍占 30-50% CPU |
| **DPDK 收益** | **非常高。** 零 syscall + 轮询 = P99 延迟 < 10μs（vs 内核 ~50μs）。游戏场景对尾延迟极其敏感 |
| **DPDK 代价** | 高 — 运维团队需要管 hugepages/网卡绑定。但游戏后端通常自建机房/物理机，条件满足 |
| **推荐** | **DPDK + mTCP**。游戏网关是 Thunder + DPDK 的最佳应用场景之一 |

#### 场景 ⑧ 业务逻辑服务 (Logic)

```
Thunder Worker → Logic Node → DB/Cache → Response
  纯业务处理: JSON → DB query → protobuf → response
```

| 维度 | 分析 |
|------|------|
| **连接数** | C1–C100（内部 RPC 协议，长连接池） |
| **PPS** | < 1K pps/核 |
| **瓶颈** | **100% 业务逻辑** — DB 查询 500μs、JSON 解析 50μs、protobuf 打包 20μs。网络 I/O 开销 < 5μs 可忽略 |
| **推荐** | **ev/epoll**。连 io_uring 都未必需要 |

---

### 10.3 决策矩阵

```
场景                        io_uring   DPDK+mTCP   说明
──────────────────────────────────────────────────────────────
① API 网关 (C10K, <50K pps)    ✅          ❌        io_uring 绰绰有余
② L7 反向代理 (C100K, <200K)   ⚠️          ⚠️        C100K 临界线, 先测 io_uring
③ Sidecar (C100, <5K pps)      ✅          ❌        容器内无法 DPDK
④ 内部 RPC (C100, <10K pps)    ✅          ❌        长连接池, 连接数少
⑤ 长连接网关 (C1M, 低 pps)     ⚠️          ✅        连接数为王, DPDK 省内存
⑥ CDN 边缘 (C100K, 高 pps)     ❌          ✅        PPS 为王, DPDK 核心收益
⑦ 游戏网关 (小包, 低延迟)      ❌          ✅★       最佳场景
⑧ 业务逻辑 (低连接, 低 pps)    ✅          ❌        网络不是瓶颈
```

### 10.4 推荐演进路线

```
当前状态 (2026 Q2):
  ev (epoll) + asio_uring
  → 覆盖 ①③④⑧ 四个场景

Step 1 (当前即可):
  io_uring + registered buffers
  → 数据面零拷贝, 覆盖 ①③④⑧, 延伸进入 ②⑤

Step 2 (中期, 需要硬件):
  DPDK + mTCP (配置化开关 io_backend=dpdk)
  → 仅开给 ⑤⑥⑦ 三个高 PPS/高连接场景

Step 3 (远期, 生产验证后):
  自动化决策: 根据负载特征动态切换后端?
  → 仅理论探讨, 实际运维风险高
```

### 10.5 场景限制：什么情况下 DPDK 一定不行

| 限制条件 | 排除场景 | 原因 |
|---------|---------|------|
| **运行在容器/K8s** | ③ Sidecar | veth 无 DPDK PMD |
| **共享网卡** | 全部 | 必须独占物理网口 |
| **云虚拟机（非 SR-IOV）** | ① ② ④ ⑧ | VM 虚拟网卡不能绑 DPDK |
| **运维团队不熟悉 DPDK** | 全部 | hugepages/NIC 绑定需要内核级调试能力 |
| **需要 tcpdump 排障** | 全部 | DPDK 流量内核不可见 |
| **需要动态扩缩容** | ⑤ ⑥ ⑦ | DPDK 网卡绑定后不能热迁移 |

---

## 11. 大厂 DPDK 应用场景参考

### 11.1 总览

大厂用 DPDK 的规律：**全部在基础设施层，不在业务层。**

```
业务服务 (Thunder 的定位):
  ├── 电商订单服务、用户中心、支付系统
  ├── 技术栈: Spring Boot / Go / C++ RPC 框架
  ├── 瓶颈: 数据库、缓存、业务逻辑
  └── ❌ 不需要 DPDK

基础设施 (DPDK 真正在用的):
  ├── L4/L7 负载均衡、API 网关
  ├── DDoS 高防、WAF
  ├── CDN 边缘节点
  ├── 云网络虚拟化 (VPC 网关、NAT 网关)
  ├── 5G 核心网 UPF
  └── 存储 SPDK
```

### 11.2 国内大厂具体案例

| 公司 | 项目 | 场景 | 规模 |
|------|------|------|------|
| **阿里巴巴** | XGW（雪豹网关） | 专有云 VPC 网络网关，承载阿里云上所有虚拟机的南北向流量 | 单集群 Tbps 级吞吐 |
| **阿里巴巴** | SLB 负载均衡 | L4/L7 负载均衡，替代 LVS/NGINX | 支撑双十一峰值流量 |
| **腾讯** | CLB 负载均衡 | 云负载均衡产品，基于 DPDK + F-Stack (自研，后开源) | 腾讯云全量用户 |
| **腾讯** | DDoS 防护 | 清洗中心，实时检测+dropping 攻击流量 | 单点 Tbps |
| **字节跳动** | 七层网关 BFE | 基于 Go 的七层负载均衡，部分热路径用 DPDK 加速 | 全公司流量入口 |
| **字节跳动** | 服务网格 Agent | Sidecar 代理的数据面加速 | 百万级 pod |
| **快手** | CDN 边缘节点 | 直播/短视频边缘缓存与转发 | 全国数千节点 |
| **美团** | MGW（四层网关） | 数据中心内网 L4 负载均衡 | 替代 LVS/DPVS |
| **百度** | BFE 负载均衡 | 七层流量网关 | 全百度业务入口 |
| **京东** | 网关/负载均衡 | 618/双十一大促流量 | 峰值 Tbps |
| **爱奇艺** | DPVS | 基于 DPDK 的 L4 负载均衡（开源） | 替代 LVS，支撑视频 CDN |

### 11.3 海外案例

| 公司 | 项目 | 场景 |
|------|------|------|
| **Cloudflare** | DDoS 防护 + 边缘计算 | Magic Transit、Spectrum（L4 代理），基于 DPDK 自研 |
| **AWS** | Nitro 系统 | Nitro 卡硬件卸载 + DPDK 软件加速。ENA 驱动基于 DPDK |
| **Google** | Andromeda (云网络) | GCP 的软件定义网络虚拟化，数据面用 DPDK |
| **ScyllaDB** | ScyllaDB 数据库 | 基于 Seastar (DPDK 用户态 TCP) 的 NoSQL 数据库 |
| **AT&T / 中国移动** | 5G UPF | 5G 核心网用户面功能，DPDK 处理 GTP 隧道封装/解封装 |

### 11.4 规律总结

```
大厂用 DPDK 的统一特征:

  1. 都是云基础设施 / 网络转发面组件（非业务应用）
     SLB/XGW/CLB → 负载均衡/网关，转发流量
     CDN/DDoS → 边缘过滤/清洗，转发流量
     VPC/Nitro/Andromeda → 虚拟网络，转发流量
     5G UPF → 核心网用户面，转发流量
     特点: 处理"流过"的包，不处理业务逻辑

  2. 都追求"线速"（网卡支持多快就跑多快）
     10Gbps → 25Gbps → 100Gbps，PPS 是核心指标
     业务应用追求的是 QPS/TP99，不是 PPS

  3. 都运行在云厂商/基础架构团队自有的物理机上
     有 root、有物理网卡、有 hugepages
     业务应用跑在租用的 VM/容器里，不具备这些条件

  4. 都有专门的基础架构/内核团队维护
     人数通常是业务团队的数倍
     需要深厚的网络协议栈 + DPDK + 内核知识

  5. 都是自研或重度定制（不用社区原版 mTCP/F-Stack）
     mTCP 和 F-Stack 的开源版本停留在学术/演示级别
     生产级需要大量 bugfix + 性能调优

  6. ★ 最关键: 都是云服务的提供者，不是云服务的使用者
     阿里云 → 提供 SLB (DPDK) → 用户买 SLB 用 → 用户跑 Thunder 在 SLB 后面
     腾讯云 → 提供 CLB (DPDK) → 用户买 CLB
     AWS    → 提供 NLB (Nitro/DPDK) → 用户买 NLB
     用户的应用永远跑在云基础设施的上面一层，不需要也不应该用 DPDK

Thunder 作为业务框架 → 跑在 ECS/K8s 容器里 → 流量经 SLB → 
具备上述任何特征的反面 → DPDK 过度设计
```

---

## 12. 参考资料

- DPDK: https://www.dpdk.org/
- mTCP: https://github.com/mtcp-stack/mtcp
- F-Stack (腾讯开源的基于 FreeBSD TCP 的 DPDK 用户态网络栈): https://github.com/F-Stack/f-stack
- Seastar (DPDK + 用户态 TCP 的 C++ 异步框架): https://github.com/scylladb/seastar
- TLDK (FD.io 传输层开发套件): https://github.com/FDio/tldk
- DPVS (爱奇艺基于 DPDK 的 L4 负载均衡): https://github.com/iqiyi/dpvs
- OpenOnload (Solarflare 用户态 TCP, LD_PRELOAD 方案): https://github.com/Xilinx-CNS/onload
- AF_XDP (内核旁路包 I/O, 保留 TCP 栈): https://www.kernel.org/doc/html/latest/networking/af_xdp.html
- VPP (FD.io 矢量包处理器): https://github.com/FDio/vpp
