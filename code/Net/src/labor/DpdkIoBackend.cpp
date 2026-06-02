/*******************************************************************************
 * Project:  Net
 * @file     DpdkIoBackend.cpp
 * @brief    DPDK + mTCP user-space TCP backend implementation
 *
 * Two compile modes:
 *   THUNDER_IO_DPDK defined  -> full mTCP implementation
 *   THUNDER_IO_DPDK not def  -> skeleton (compiles, returns empty/error)
 *
 * Thread safety: all methods run on libev main thread only (single-threaded).
 ******************************************************************************/
#include "DpdkIoBackend.hpp"
#include "util/CBuffer.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

// ---- helpers shared across both modes ----
static void FillPeerAddr(const struct sockaddr_in& addr, net::PeerAddr& out)
{
    inet_ntop(AF_INET, &addr.sin_addr, out.ip, sizeof(out.ip));
    out.port = ntohs(addr.sin_port);
}

namespace net
{

// ===================================================================
// DpdkContext
// ===================================================================

bool DpdkContext::s_ealInitialized = false;

bool DpdkContext::Init(const Config& cfg)
{
#ifdef THUNDER_IO_DPDK
    // ---- DPDK EAL (process-global, init once) ----
    if (!s_ealInitialized)
    {
        int ealArgc = cfg.ealArgc;
        char** ealArgv = cfg.ealArgv;
        char* defaultArgv[8];
        char argBuf[8][64];

        if (!ealArgv || ealArgc <= 0)
        {
            // Build default EAL args
            snprintf(argBuf[0], sizeof(argBuf[0]), "%s", "thunder");
            snprintf(argBuf[1], sizeof(argBuf[1]), "-l");
            snprintf(argBuf[2], sizeof(argBuf[2]), "%d", cfg.lcoreId);
            snprintf(argBuf[3], sizeof(argBuf[3]), "-n");
            snprintf(argBuf[4], sizeof(argBuf[4]), "4");
            snprintf(argBuf[5], sizeof(argBuf[5]), "--proc-type=primary");
            for (int i = 0; i < 6; ++i) defaultArgv[i] = argBuf[i];
            ealArgc = 6;
            ealArgv = defaultArgv;
        }

        int ret = rte_eal_init(ealArgc, ealArgv);
        if (ret < 0)
        {
            fprintf(stderr, "[DpdkContext] rte_eal_init failed: %s\n", rte_strerror(rte_errno));
            return false;
        }
        s_ealInitialized = true;
    }

    // ---- mTCP context (per-core) ----
    int ret = mtcp_init("/etc/mtcp.conf");
    if (ret < 0)
    {
        fprintf(stderr, "[DpdkContext] mtcp_init failed\n");
        return false;
    }

    m_mctx = mtcp_create_context(cfg.lcoreId);
    if (!m_mctx)
    {
        fprintf(stderr, "[DpdkContext] mtcp_create_context failed for lcore %d\n", cfg.lcoreId);
        return false;
    }

    // ---- mTCP epoll ----
    m_epId = mtcp_epoll_create(m_mctx, cfg.maxConcurrency);
    if (m_epId < 0)
    {
        fprintf(stderr, "[DpdkContext] mtcp_epoll_create failed\n");
        mtcp_destroy_context(m_mctx);
        m_mctx = nullptr;
        return false;
    }

    m_initialized = true;
    return true;
#else
    (void)cfg;
    return false;
#endif
}

void DpdkContext::Destroy()
{
#ifdef THUNDER_IO_DPDK
    if (m_epId >= 0)
    {
        mtcp_epoll_destroy(m_mctx, m_epId);
        m_epId = -1;
    }
    if (m_mctx)
    {
        mtcp_destroy_context(m_mctx);
        m_mctx = nullptr;
    }
#endif
    m_initialized = false;
    // Note: do NOT call rte_eal_cleanup() here -- it's process-global,
    // only call once at process exit.
}

// ===================================================================
// DpdkIoBackend
// ===================================================================

DpdkIoBackend::DpdkIoBackend(DpdkContext* ctx)
    : m_dpdkCtx(ctx)
{
}

DpdkIoBackend::~DpdkIoBackend()
{
    Destroy();
}

// ========== Lifecycle ==========

bool DpdkIoBackend::Init(struct ev_loop* loop, IoCompletionCallback callback, void* user_data)
{
    if (!loop || !callback || !m_dpdkCtx)
        return false;

    if (!m_dpdkCtx->IsInitialized())
        return false;

    m_evLoop   = loop;
    m_callback = callback;
    m_userData = user_data;

    // Register libev bridge hooks
    ev_check_init(&m_evCheckWatcher, OnEvCheck);
    m_evCheckWatcher.data = this;
    ev_check_start(m_evLoop, &m_evCheckWatcher);

    ev_idle_init(&m_evIdleWatcher, OnEvIdle);
    m_evIdleWatcher.data = this;
    ev_idle_start(m_evLoop, &m_evIdleWatcher);

    m_started = true;
    return true;
}

void DpdkIoBackend::Destroy()
{
    if (m_evLoop && m_started)
    {
        ev_check_stop(m_evLoop, &m_evCheckWatcher);
        ev_idle_stop(m_evLoop, &m_evIdleWatcher);
        m_started = false;
    }

    m_fds.clear();
    m_readBufs.clear();
    m_writeBufs.clear();
    m_mapPeerAddr.clear();

    m_evLoop   = nullptr;
    m_callback = nullptr;
    m_userData = nullptr;
}

// ========== Listen socket ==========

int DpdkIoBackend::CreateListenSocket(const char* ip, uint16_t port,
                                       bool /*boReusePort*/, int backlog)
{
#ifdef THUNDER_IO_DPDK
    if (!m_dpdkCtx || !m_dpdkCtx->IsInitialized())
        return -1;

    mctx_t ctx = m_dpdkCtx->GetCtx();
    int epid   = m_dpdkCtx->GetEpollFd();

    // 1. Create mTCP socket
    int sockid = mtcp_socket(ctx, AF_INET, SOCK_STREAM, 0);
    if (sockid < 0)
        return -1;

    // 2. Set non-blocking + reuseaddr
    mtcp_setsock_nonblock(ctx, sockid);
    int on = 1;
    mtcp_setsockopt(ctx, sockid, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    // 3. bind
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1)
    {
        mtcp_close(ctx, sockid);
        return -1;
    }
    if (mtcp_bind(ctx, sockid, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) < 0)
    {
        mtcp_close(ctx, sockid);
        return -1;
    }

    // 4. listen
    if (mtcp_listen(ctx, sockid, backlog) < 0)
    {
        mtcp_close(ctx, sockid);
        return -1;
    }

    // 5. Register to mTCP epoll for accept
    struct mtcp_epoll_event ev;
    ev.events      = MTCP_EPOLLIN;
    ev.data.sockid = sockid;
    mtcp_epoll_ctl(ctx, epid, MTCP_EPOLL_CTL_ADD, sockid, &ev);

    int dpdkFd = ToDpdkFd(sockid);
    // Pre-populate FdState so Accept can use it
    m_fds[dpdkFd] = FdState{};
    return dpdkFd;
#else
    (void)ip; (void)port; (void)backlog;
    return -1;
#endif
}

int DpdkIoBackend::Accept(int listenFd, PeerAddr& outPeerAddr)
{
#ifdef THUNDER_IO_DPDK
    if (!m_dpdkCtx || !m_dpdkCtx->IsInitialized())
        return -1;

    if (!IsDpdkFd(listenFd))
        return -1;

    mctx_t ctx   = m_dpdkCtx->GetCtx();
    int epid     = m_dpdkCtx->GetEpollFd();
    int sockid   = ToMtcpSockId(listenFd);

    struct sockaddr_in addr;
    socklen_t addrLen = sizeof(addr);
    int clientSockid = mtcp_accept(ctx, sockid,
                                    reinterpret_cast<struct sockaddr*>(&addr),
                                    &addrLen);
    if (clientSockid < 0)
        return -1;  // EAGAIN or error

    // Set socket options on new connection
    mtcp_setsock_nonblock(ctx, clientSockid);
    // TCP_NODELAY + SO_KEEPALIVE (best-effort, may not be supported by all mTCP versions)
    int opt = 1;
    mtcp_setsockopt(ctx, clientSockid, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    mtcp_setsockopt(ctx, clientSockid, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));

    // Register to mTCP epoll (add, not mod -- new fd)
    struct mtcp_epoll_event ev;
    ev.events      = MTCP_EPOLLIN;
    ev.data.sockid = clientSockid;
    mtcp_epoll_ctl(ctx, epid, MTCP_EPOLL_CTL_ADD, clientSockid, &ev);

    int dpdkFd = ToDpdkFd(clientSockid);
    FillPeerAddr(addr, outPeerAddr);
    m_mapPeerAddr[dpdkFd] = outPeerAddr;

    return dpdkFd;
#else
    (void)listenFd;
    std::memset(&outPeerAddr, 0, sizeof(outPeerAddr));
    return -1;
#endif
}

// ========== I/O Submit ==========

#ifdef THUNDER_IO_DPDK
bool DpdkIoBackend::EpollCtl(int sockid, int op, uint32_t events)
{
    mctx_t ctx = m_dpdkCtx->GetCtx();
    int epid   = m_dpdkCtx->GetEpollFd();

    struct mtcp_epoll_event ev;
    ev.events      = events;
    ev.data.sockid = sockid;

    return mtcp_epoll_ctl(ctx, epid, op, sockid, &ev) == 0;
}
#endif

bool DpdkIoBackend::SubmitRead(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq)
{
#ifdef THUNDER_IO_DPDK
    if (!buf || !m_callback || !m_dpdkCtx || !m_dpdkCtx->IsInitialized())
        return false;

    if (!IsDpdkFd(fd))
        return false;

    int sockid = ToMtcpSockId(fd);

    auto& st = m_fds[fd];
    if (st.seq != seq)
    {
        st.seq = seq;
        st.readPending  = 0;
        st.writePending = 0;
        st.cancelled    = false;
    }
    if (st.readPending > 0)
        return true;  // already pending

    buf->EnsureWritableBytes(8192);
    m_readBufs[fd] = buf;  // weak_ptr holds reference

    // Register EPOLLIN (MOD since fd is already in epoll from accept)
    EpollCtl(sockid, MTCP_EPOLL_CTL_MOD, MTCP_EPOLLIN);

    st.readPending++;
    return true;
#else
    (void)fd; (void)seq;
    return false;
#endif
}

bool DpdkIoBackend::SubmitWrite(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq)
{
#ifdef THUNDER_IO_DPDK
    if (!buf || !m_callback || !m_dpdkCtx || !m_dpdkCtx->IsInitialized())
        return false;

    if (!IsDpdkFd(fd))
        return false;

    int readable = static_cast<int>(buf->ReadableBytes());
    if (readable <= 0)
    {
        m_callback(fd, seq, IoOp::Write, 0, m_userData);
        return true;
    }

    int sockid = ToMtcpSockId(fd);

    auto& st = m_fds[fd];
    if (st.seq != seq)
    {
        st.seq = seq;
        st.readPending  = 0;
        st.writePending = 0;
        st.cancelled    = false;
    }
    if (st.writePending > 0)
        return true;

    mctx_t ctx = m_dpdkCtx->GetCtx();
    m_writeBufs[fd] = buf;

    // Try immediate synchronous write
    const char* src = buf->GetRawReadBuffer();
    int sent = mtcp_write(ctx, sockid, src, readable);

    if (sent > 0)
    {
        buf->AdvanceReadIndex(sent);
        if (sent == readable)
        {
            // Fully written
            st.writePending = 0;
            m_callback(fd, seq, IoOp::Write, sent, m_userData);
            return true;
        }
        // Partial write: register EPOLLOUT for remainder
    }
    else if (sent == 0)
    {
        // EOF
        m_callback(fd, seq, IoOp::Write, 0, m_userData);
        return true;
    }
    // sent < 0: EAGAIN or error -> register EPOLLOUT

    EpollCtl(sockid, MTCP_EPOLL_CTL_MOD, MTCP_EPOLLIN | MTCP_EPOLLOUT);
    st.writePending++;
    return true;
#else
    (void)fd; (void)seq;
    return false;
#endif
}

// ========== Connection close ==========

void DpdkIoBackend::CancelFd(int fd)
{
    auto it = m_fds.find(fd);
    if (it == m_fds.end())
        return;

    // Mark cancelled. In-flight ops on this fd will be discarded
    // when their mTCP epoll events arrive (fd not in m_fds or seq mismatch).
    it->second.cancelled = true;

#ifdef THUNDER_IO_DPDK
    if (m_dpdkCtx && m_dpdkCtx->IsInitialized() && IsDpdkFd(fd))
    {
        int sockid = ToMtcpSockId(fd);
        EpollCtl(sockid, MTCP_EPOLL_CTL_DEL, 0);
    }
#endif

    m_fds.erase(it);
}

void DpdkIoBackend::CloseFd(int fd)
{
    if (!IsDpdkFd(fd))
        return;

#ifdef THUNDER_IO_DPDK
    if (m_dpdkCtx && m_dpdkCtx->IsInitialized())
    {
        int sockid = ToMtcpSockId(fd);
        mtcp_close(m_dpdkCtx->GetCtx(), sockid);
    }
#else
    (void)fd;
#endif

    m_readBufs.erase(fd);
    m_writeBufs.erase(fd);
    m_mapPeerAddr.erase(fd);
}

bool DpdkIoBackend::HasPending(int fd) const
{
    auto it = m_fds.find(fd);
    if (it == m_fds.end())
        return false;
    return (it->second.readPending > 0 || it->second.writePending > 0)
           && !it->second.cancelled;
}

// ========== Socket options / peer address ==========

void DpdkIoBackend::SetSocketOpt(int fd)
{
    if (!IsDpdkFd(fd))
        return;

#ifdef THUNDER_IO_DPDK
    if (!m_dpdkCtx || !m_dpdkCtx->IsInitialized())
        return;

    int sockid = ToMtcpSockId(fd);
    mctx_t ctx = m_dpdkCtx->GetCtx();
    int opt = 1;
    // Best-effort: some mTCP versions may not support all options
    mtcp_setsockopt(ctx, sockid, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    mtcp_setsockopt(ctx, sockid, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
#else
    (void)fd;
#endif
}

bool DpdkIoBackend::GetPeerName(int fd, PeerAddr& outAddr)
{
    // mTCP has no getpeername() -- return cached address from Accept
    auto it = m_mapPeerAddr.find(fd);
    if (it == m_mapPeerAddr.end())
    {
        std::memset(&outAddr, 0, sizeof(outAddr));
        return false;
    }
    outAddr = it->second;
    return true;
}

// ========== mTCP epoll event processing ==========

void DpdkIoBackend::ProcessMtcpEvents()
{
#ifdef THUNDER_IO_DPDK
    if (!m_dpdkCtx || !m_dpdkCtx->IsInitialized())
        return;

    mctx_t ctx = m_dpdkCtx->GetCtx();
    int epid   = m_dpdkCtx->GetEpollFd();

    struct mtcp_epoll_event events[256];
    int n = mtcp_epoll_wait(ctx, epid, events, 256, 0);  // non-blocking

    for (int i = 0; i < n; ++i)
    {
        int fd    = ToDpdkFd(events[i].data.sockid);
        auto it   = m_fds.find(fd);
        if (it == m_fds.end() || it->second.cancelled)
            continue;

        uint32_t ev = events[i].events;

        if (ev & MTCP_EPOLLERR)
        {
            HandleError(fd);
            continue;
        }
        if (ev & MTCP_EPOLLIN)
        {
            // Accept or Read?
            // Listen fd: FdState is pre-populated but readPending==0 => new connection
            if (it->second.readPending > 0)
                HandleRead(fd);
            else
            {
                // Accept new connection
                PeerAddr peer;
                int clientFd = Accept(fd, peer);
                if (clientFd >= 0 && m_callback)
                {
                    m_callback(clientFd, 0, IoOp::Read, 0, m_userData);
                }
            }
        }
        if (ev & MTCP_EPOLLOUT)
        {
            HandleWrite(fd);
        }
    }
#endif
}

void DpdkIoBackend::ProcessMtcpEventsBlocking(int timeoutMs)
{
#ifdef THUNDER_IO_DPDK
    if (!m_dpdkCtx || !m_dpdkCtx->IsInitialized())
        return;

    mctx_t ctx = m_dpdkCtx->GetCtx();
    int epid   = m_dpdkCtx->GetEpollFd();

    // mtcp_epoll_wait with timeout drives internal TCP timers (retransmit, keepalive, etc.)
    struct mtcp_epoll_event events[256];
    int n = mtcp_epoll_wait(ctx, epid, events, 256, timeoutMs);

    for (int i = 0; i < n; ++i)
    {
        int fd  = ToDpdkFd(events[i].data.sockid);
        auto it = m_fds.find(fd);
        if (it == m_fds.end() || it->second.cancelled)
            continue;

        if (events[i].events & MTCP_EPOLLIN && it->second.readPending > 0)
            HandleRead(fd);
        if (events[i].events & MTCP_EPOLLOUT)
            HandleWrite(fd);
    }
#else
    (void)timeoutMs;
#endif
}

// ========== Internal event handlers ==========

void DpdkIoBackend::HandleRead(int fd)
{
#ifdef THUNDER_IO_DPDK
    auto it = m_fds.find(fd);
    if (it == m_fds.end() || it->second.cancelled)
        return;

    auto bufIt = m_readBufs.find(fd);
    if (bufIt == m_readBufs.end())
        return;

    auto buf = bufIt->second.lock();  // weak_ptr -> shared_ptr
    if (!buf)
        return;

    mctx_t ctx = m_dpdkCtx->GetCtx();
    int sockid = ToMtcpSockId(fd);

    buf->EnsureWritableBytes(8192);
    char*  dst = const_cast<char*>(buf->GetRawWriteBuffer());
    size_t cap = buf->WriteableBytes();

    int n = mtcp_read(ctx, sockid, dst, static_cast<int>(cap));
    if (n > 0)
    {
        buf->AdvanceWriteIndex(n);
        if (it->second.readPending > 0)
            it->second.readPending--;
        if (m_callback)
            m_callback(fd, it->second.seq, IoOp::Read, n, m_userData);
    }
    else if (n == 0)
    {
        // EOF
        if (m_callback)
            m_callback(fd, it->second.seq, IoOp::Read, 0, m_userData);
    }
    // n < 0: EAGAIN, retry later (mtcp epoll will re-trigger)
#else
    (void)fd;
#endif
}

void DpdkIoBackend::HandleWrite(int fd)
{
#ifdef THUNDER_IO_DPDK
    auto it = m_fds.find(fd);
    if (it == m_fds.end() || it->second.cancelled)
        return;

    auto bufIt = m_writeBufs.find(fd);
    if (bufIt == m_writeBufs.end())
        return;

    auto buf = bufIt->second.lock();
    if (!buf)
        return;

    mctx_t ctx = m_dpdkCtx->GetCtx();
    int sockid = ToMtcpSockId(fd);

    int readable = static_cast<int>(buf->ReadableBytes());
    if (readable <= 0)
    {
        if (it->second.writePending > 0)
            it->second.writePending--;
        if (m_callback)
            m_callback(fd, it->second.seq, IoOp::Write, 0, m_userData);
        return;
    }

    const char* src = buf->GetRawReadBuffer();
    int sent = mtcp_write(ctx, sockid, src, readable);

    if (sent > 0)
    {
        buf->AdvanceReadIndex(sent);
        if (static_cast<int>(buf->ReadableBytes()) == 0)
        {
            // Fully written, switch back to EPOLLIN only
            EpollCtl(sockid, MTCP_EPOLL_CTL_MOD, MTCP_EPOLLIN);
            if (it->second.writePending > 0)
                it->second.writePending--;
            if (m_callback)
                m_callback(fd, it->second.seq, IoOp::Write, sent, m_userData);
        }
        // else: partial write, keep EPOLLOUT, wait for next trigger
    }
    // sent <= 0: EAGAIN or error, keep EPOLLOUT
#else
    (void)fd;
#endif
}

void DpdkIoBackend::HandleError(int fd)
{
    auto it = m_fds.find(fd);
    if (it == m_fds.end())
        return;

    if (m_callback)
        m_callback(fd, it->second.seq, IoOp::Read, -ECONNRESET, m_userData);
}

// ========== libev bridge hooks ==========

void DpdkIoBackend::OnEvCheck(struct ev_loop*, ev_check* w, int)
{
    auto* be = static_cast<DpdkIoBackend*>(w->data);
    be->ProcessMtcpEvents();  // non-blocking reap
}

void DpdkIoBackend::OnEvIdle(struct ev_loop*, ev_idle* w, int)
{
    auto* be = static_cast<DpdkIoBackend*>(w->data);
    // Block-wait with 1ms timeout to drive mTCP internal TCP timers
    // (retransmit, keepalive, delayed ACK, etc.)
    be->ProcessMtcpEventsBlocking(1);
}

} /* namespace net */
