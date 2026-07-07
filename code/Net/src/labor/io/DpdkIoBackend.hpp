/*******************************************************************************
 * Project:  Net
 * @file     DpdkIoBackend.hpp
 * @brief    DPDK + mTCP 用户态 TCP 后端 — 策略模式具体实现
 *
 * === 架构 ===
 *
 *   Worker  --> IoBackend (策略接口) --> DpdkIoBackend
 *                                              |
 *                          +-------------------+
 *                          |  mTCP API         |  libev hook
 *                          |  (user-space fd)  |  (event bridge)
 *                          v                   v
 *                    mTCP user-space TCP    libev main loop
 *                          |
 *                          v
 *                    DPDK PMD (poll-mode driver)
 *                          |
 *                          v
 *                    Physical NIC
 *
 * === sockid isolation ===
 *
 * mTCP sockid may collide with kernel fd. Offset 10,000,000 avoids overlap.
 *
 * === Event loop bridge ===
 *
 * mTCP sockets are not kernel fds, cannot register to libev epoll.
 * Use ev_check + ev_idle dual hooks to drive mTCP epoll:
 *
 *   ev_check: after epoll_wait, non-blocking reap of mTCP ready events
 *   ev_idle:  when no libev events, block-wait mTCP with 1ms timeout,
 *             also drives mTCP internal TCP timers (retransmit/keepalive)
 *
 * === Prerequisites ===
 *
 * 1. Physical machine (not cloud VM)
 * 2. DPDK-compatible NIC (Intel X710/X520/82599, Mellanox CX4+, etc.)
 * 3. Kernel boot params: hugepages, iommu=pt
 * 4. mTCP compiled and linked
 * 5. Config: "io_backend": "dpdk"
 *
 * === Build ===
 *
 *   cmake -DTHUNDER_IO_DPDK=ON
 *
 * Without DPDK/mTCP SDK: compiles, all ops return empty/error (skeleton mode)
 *
 * @see      IoBackend
 * @see      docs/uring/DPDK+mTCP设计文档.md
 ******************************************************************************/
#ifndef DPDKIOBACKEND_HPP_
#define DPDKIOBACKEND_HPP_

#include "labor/IoBackend.hpp"
#include "libev/ev.h"
#include <cstdint>
#include <memory>
#include <unordered_map>

// ---- DPDK/mTCP headers (only when HAVE_DPDK) ----
#ifdef THUNDER_IO_DPDK
#include <mtcp_api.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#endif

namespace util { class CBuffer; }

namespace net
{

/// mTCP sockid offset to avoid kernel fd namespace collision
constexpr int kDpdkSockIdOffset = 10000000;

inline int ToDpdkFd(int mctxSockId) { return mctxSockId + kDpdkSockIdOffset; }
inline int ToMtcpSockId(int dpdkFd) { return dpdkFd - kDpdkSockIdOffset; }
inline bool IsDpdkFd(int fd) { return fd >= kDpdkSockIdOffset; }

// ===================================================================
// DpdkContext -- DPDK EAL + mTCP per-Worker singleton
// ===================================================================
class DpdkContext
{
public:
    struct Config
    {
        int    ealArgc       = 0;
        char** ealArgv       = nullptr;
        int    nbPorts       = 1;
        int    nbRxQueues    = 1;
        int    nbTxQueues    = 1;
        int    mbufPoolSize  = 8192;

        int    maxConcurrency = 100000;
        int    maxNumBuffers  = 100000;
        int    rcvBufSize     = 8192;
        int    sndBufSize     = 8192;
        int    tcpTimeout     = 30;

        int    lcoreId        = 0;

        Config() = default;
    };

    DpdkContext()  = default;
    ~DpdkContext() { Destroy(); }

    bool Init(const Config& cfg);
    void Destroy();

#ifdef THUNDER_IO_DPDK
    mctx_t  GetCtx()     const { return m_mctx; }
#else
    void*   GetCtx()     const { return nullptr; }
#endif
    int     GetEpollFd() const { return m_epId; }
    bool    IsInitialized() const { return m_initialized; }

private:
#ifdef THUNDER_IO_DPDK
    mctx_t m_mctx = nullptr;
#endif
    int    m_epId = -1;
    bool   m_initialized = false;
    static bool s_ealInitialized;
};

// ===================================================================
// DpdkIoBackend -- IoBackend strategy pattern implementation
// ===================================================================
class DpdkIoBackend : public IoBackend
{
public:
    explicit DpdkIoBackend(DpdkContext* ctx);
    ~DpdkIoBackend() override;

    bool Init(struct ev_loop* loop, IoCompletionCallback callback, void* user_data) override;
    void Destroy() override;
    const char* Name() const override { return "dpdk"; }

    int  CreateListenSocket(const char* ip, uint16_t port, bool boReusePort, int backlog) override;
    int  Accept(int listenFd, PeerAddr& outPeerAddr) override;

    bool SubmitRead(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq) override;
    bool SubmitWrite(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq) override;

    void CancelFd(int fd) override;
    void CloseFd(int fd) override;
    bool HasPending(int fd) const override;

    void SetSocketOpt(int fd) override;
    bool GetPeerName(int fd, PeerAddr& outAddr) override;

    // ---- mTCP epoll event processing ----
    void ProcessMtcpEvents();
    void ProcessMtcpEventsBlocking(int timeoutMs);

private:
    struct FdState
    {
        uint32_t seq          = 0;
        int      readPending  = 0;
        int      writePending = 0;
        bool     cancelled    = false;
    };

#ifdef THUNDER_IO_DPDK
    bool EpollCtl(int sockid, int op, uint32_t events);
#endif

    void HandleRead(int fd);
    void HandleWrite(int fd);
    void HandleError(int fd);

    static void OnEvCheck(struct ev_loop*, ev_check* w, int);
    static void OnEvIdle(struct ev_loop*, ev_idle* w, int);

    DpdkContext*          m_dpdkCtx  = nullptr;
    struct ev_loop*       m_evLoop   = nullptr;
    IoCompletionCallback  m_callback = nullptr;
    void*                 m_userData = nullptr;

    ev_check              m_evCheckWatcher{};
    ev_idle               m_evIdleWatcher{};
    bool                  m_started = false;

    std::unordered_map<int, FdState>                     m_fds;
    std::unordered_map<int, std::weak_ptr<util::CBuffer>> m_readBufs;
    std::unordered_map<int, std::weak_ptr<util::CBuffer>> m_writeBufs;
    std::unordered_map<int, PeerAddr>                    m_mapPeerAddr;
};

} /* namespace net */

#endif /* DPDKIOBACKEND_HPP_ */
