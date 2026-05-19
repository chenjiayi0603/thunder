/*******************************************************************************
 * Project:  Net
 * @file     DpdkIoBackend.cpp
 * @brief    DPDK + mTCP 用户态 TCP 后端实现
 *
 * 骨架实现: 无 DPDK/mTCP SDK 时所有操作返回空/错误，
 * 保证编译通过 + 接口契约可测试。
 *
 * 真实实现路径 (HAVE_DPDK=1):
 *   CreateListenSocket → mtcp_socket() + mtcp_bind() + mtcp_listen()
 *   Accept             → mtcp_accept(mctx, sockid, ...)
 *   SubmitRead/Write   → mtcp_epoll + mtcp_read/write
 *   CloseFd            → mtcp_close(mctx, sockid)
 *   SetSocketOpt       → mtcp_setsockopt (TCP_NODELAY 等子集)
 *   GetPeerName        → Accept 时缓存的 PeerAddr
 *
 * @note  每行 //HAVE_DPDK 注释标记真实实现占位点，
 *        便于后续补充 mTCP API 调用。
 ******************************************************************************/
#include "DpdkIoBackend.hpp"
#include "util/CBuffer.hpp"
#include <cstring>

namespace net
{

DpdkIoBackend::DpdkIoBackend()  = default;
DpdkIoBackend::~DpdkIoBackend() { Destroy(); }

// ========== 生命周期 ==========

bool DpdkIoBackend::Init(struct ev_loop* loop, IoCompletionCallback callback, void* user_data)
{
    if (!loop || !callback) return false;

    // HAVE_DPDK: mtcp_init("thunder.conf")
    // HAVE_DPDK: dpdk_port_init(port_id)
    // HAVE_DPDK: mtcp_create_context(mctx)

    m_loop        = loop;
    m_callback    = callback;
    m_userData    = user_data;
    m_initialized = true;
    return true;
}

void DpdkIoBackend::Destroy()
{
    if (!m_initialized) return;

    // HAVE_DPDK: 遍历接受连接 → mtcp_close(mctx, sockid)
    // HAVE_DPDK: mtcp_destroy_context(mctx)

    m_mapPeerAddr.clear();
    m_loop        = nullptr;
    m_callback    = nullptr;
    m_userData    = nullptr;
    m_initialized = false;
}

// ========== 监听 socket ==========

int DpdkIoBackend::CreateListenSocket(const char* ip, uint16_t port, bool /*boReusePort*/, int backlog)
{
    if (!m_initialized) return -1;

    // HAVE_DPDK:
    //   int sockid = mtcp_socket(mctx, AF_INET, SOCK_STREAM, 0);
    //   mtcp_bind(mctx, sockid, &addr, sizeof(addr));
    //   mtcp_listen(mctx, sockid, backlog);
    //   return sockid + kDpdkSockIdOffset;

    (void)ip; (void)port; (void)backlog;
    return -1;  // 骨架: 无 DPDK → 返回 -1
}

int DpdkIoBackend::Accept(int listenFd, PeerAddr& outPeerAddr)
{
    if (!m_initialized) return -1;

    // HAVE_DPDK:
    //   int sockid = listenFd - kDpdkSockIdOffset;
    //   sockaddr_in addr; socklen_t len;
    //   int clientSockid = mtcp_accept(mctx, sockid, &addr, &len);
    //   inet_ntop(...); outPeerAddr.port = ntohs(...);
    //   m_mapPeerAddr[clientSockid + offset] = outPeerAddr;
    //   return clientSockid + kDpdkSockIdOffset;

    (void)listenFd;
    std::memset(&outPeerAddr, 0, sizeof(outPeerAddr));
    return -1;  // 骨架: 无 DPDK → 返回 -1
}

// ========== I/O 提交 ==========

bool DpdkIoBackend::SubmitRead(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq)
{
    if (!m_initialized || !buf) return false;

    // HAVE_DPDK:
    //   int sockid = fd - kDpdkSockIdOffset;
    //   mtcp_epoll_ctl(mctx, m_epollFd, MTCP_EPOLL_CTL_ADD, sockid, &ev);
    //   完成时: mtcp_read(mctx, sockid, buf->WriteBegin(), buf->WritableBytes());
    //          → buf->AdvanceWriteIndex(n) → m_callback(fd, seq, IoOp::Read, n)

    (void)fd; (void)seq;
    return false;  // 骨架: 无 DPDK → 返回 false
}

bool DpdkIoBackend::SubmitWrite(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq)
{
    if (!m_initialized || !buf) return false;

    // HAVE_DPDK:
    //   int sockid = fd - kDpdkSockIdOffset;
    //   int n = mtcp_write(mctx, sockid, buf->ReadBegin(), buf->ReadableBytes());
    //   buf->AdvanceReadIndex(n);
    //   m_callback(fd, seq, IoOp::Write, n);

    (void)fd; (void)seq;
    return false;  // 骨架: 无 DPDK → 返回 false
}

// ========== 连接关闭 ==========

void DpdkIoBackend::CancelFd(int fd)
{
    // HAVE_DPDK:
    //   int sockid = fd - kDpdkSockIdOffset;
    //   mtcp_epoll_ctl(mctx, m_epollFd, MTCP_EPOLL_CTL_DEL, sockid, nullptr);

    (void)fd;
}

void DpdkIoBackend::CloseFd(int fd)
{
    if (!m_initialized || fd < kDpdkSockIdOffset) return;

    // HAVE_DPDK:
    //   int sockid = fd - kDpdkSockIdOffset;
    //   mtcp_close(mctx, sockid);

    m_mapPeerAddr.erase(fd);
}

bool DpdkIoBackend::HasPending(int fd) const
{
    (void)fd;
    return false;  // 骨架: 无操作挂起
}

// ========== Socket 属性 / 对端地址 ==========

void DpdkIoBackend::SetSocketOpt(int fd)
{
    if (fd < kDpdkSockIdOffset) return;

    // HAVE_DPDK:
    //   int sockid = fd - kDpdkSockIdOffset;
    //   int opt = 1;
    //   mtcp_setsockopt(mctx, sockid, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    //   mtcp_setsockopt(mctx, sockid, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    //   不支持的选项静默忽略（mTCP 兼容性子集）
}

bool DpdkIoBackend::GetPeerName(int fd, PeerAddr& outAddr)
{
    // mTCP 无 getpeername → 返回 Accept 时缓存的地址
    auto it = m_mapPeerAddr.find(fd);
    if (it == m_mapPeerAddr.end())
    {
        std::memset(&outAddr, 0, sizeof(outAddr));
        return false;
    }
    outAddr = it->second;
    return true;
}

} /* namespace net */
