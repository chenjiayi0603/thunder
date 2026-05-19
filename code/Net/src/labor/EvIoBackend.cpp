/*******************************************************************************
 * Project:  Net
 * @file     EvIoBackend.cpp
 * @brief    libev-based I/O backend implementation
 ******************************************************************************/
#include "EvIoBackend.hpp"
#include "util/CBuffer.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace net
{

// ---- 辅助: 字符串 IP + port → sockaddr_in -------------------------------------------------
static bool IpPort2SockAddr(const char* ip, uint16_t port, sockaddr_in& outAddr)
{
    std::memset(&outAddr, 0, sizeof(outAddr));
    outAddr.sin_family = AF_INET;
    outAddr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &outAddr.sin_addr) != 1)
    {
        return false;
    }
    return true;
}

EvIoBackend::~EvIoBackend()
{
    Destroy();
}

bool EvIoBackend::Init(struct ev_loop* loop, IoCompletionCallback callback, void* user_data)
{
    if (!loop || !callback)
    {
        return false;
    }
    m_loop = loop;
    m_callback = callback;
    m_userData = user_data;
    return true;
}

void EvIoBackend::Destroy()
{
    // 清理所有注册的 fd
    for (auto& pair : m_mapFdData)
    {
        WatcherData* pData = pair.second.get();
        if (pData->pWatcher)
        {
            ev_io_stop(m_loop, pData->pWatcher);
            delete pData->pWatcher;
            pData->pWatcher = nullptr;
        }
    }
    m_mapFdData.clear();
    m_loop = nullptr;
    m_callback = nullptr;
    m_userData = nullptr;
}

bool EvIoBackend::SubmitRead(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq)
{
    if (!m_loop || !buf)
    {
        return false;
    }

    auto it = m_mapFdData.find(fd);
    WatcherData* pData = nullptr;

    if (it == m_mapFdData.end())
    {
        auto uptr = std::make_unique<WatcherData>();
        pData = uptr.get();
        pData->iFd = fd;
        pData->pBackend = this;

        pData->pWatcher = new ev_io();
        if (!pData->pWatcher)
        {
            return false;
        }
        ev_init(pData->pWatcher, IoEventCallback);
        pData->pWatcher->data = static_cast<void*>(pData);

        m_mapFdData[fd] = std::move(uptr);
    }
    else
    {
        pData = it->second.get();
    }

    pData->ulSeq = seq;
    pData->pReadBuf = buf.get();

    // 刷新/启动读事件（幂等）
    ev_io_set(pData->pWatcher, fd, pData->pWatcher->events | EV_READ);
    ev_io_start(m_loop, pData->pWatcher);

    return true;
}

bool EvIoBackend::SubmitWrite(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq)
{
    if (!m_loop || !buf)
    {
        return false;
    }

    auto it = m_mapFdData.find(fd);
    WatcherData* pData = nullptr;

    if (it == m_mapFdData.end())
    {
        auto uptr = std::make_unique<WatcherData>();
        pData = uptr.get();
        pData->iFd = fd;
        pData->pBackend = this;

        pData->pWatcher = new ev_io();
        if (!pData->pWatcher)
        {
            return false;
        }
        ev_init(pData->pWatcher, IoEventCallback);
        pData->pWatcher->data = static_cast<void*>(pData);

        m_mapFdData[fd] = std::move(uptr);
    }
    else
    {
        pData = it->second.get();
    }

    pData->ulSeq = seq;
    pData->pWriteBuf = buf.get();

    // 刷新/启动写事件（幂等）
    ev_io_set(pData->pWatcher, fd, pData->pWatcher->events | EV_WRITE);
    ev_io_start(m_loop, pData->pWatcher);

    return true;
}

void EvIoBackend::CancelFd(int fd)
{
    auto it = m_mapFdData.find(fd);
    if (it == m_mapFdData.end())
    {
        return;
    }
    WatcherData* pData = it->second.get();
    if (pData->pWatcher)
    {
        ev_io_stop(m_loop, pData->pWatcher);
        delete pData->pWatcher;
        pData->pWatcher = nullptr;
    }
    m_mapFdData.erase(it);
}

bool EvIoBackend::HasPending(int fd) const
{
    return m_mapFdData.find(fd) != m_mapFdData.end();
}

void EvIoBackend::IoEventCallback(struct ev_loop* loop, struct ev_io* watcher, int revents)
{
    if (!watcher->data)
    {
        return;
    }
    WatcherData* pData = static_cast<WatcherData*>(watcher->data);
    EvIoBackend* pBackend = pData->pBackend;
    int iFd = pData->iFd;

    if (revents & EV_READ)
    {
        int iErrno = 0;
        int n = pData->pReadBuf->ReadFD(iFd, iErrno);
        int result = (n >= 0) ? n : -iErrno;
        pBackend->m_callback(iFd, pData->ulSeq, IoOp::Read, result, pBackend->m_userData);
    }

    if (revents & EV_WRITE)
    {
        // 检查 watcher 是否仍在 map 中（读回调中可能已销毁）
        if (pBackend->m_mapFdData.find(iFd) == pBackend->m_mapFdData.end())
        {
            return;
        }
        int iErrno = 0;
        int n = pData->pWriteBuf->WriteFD(iFd, iErrno);
        int result = (n >= 0) ? n : -iErrno;
        pBackend->m_callback(iFd, pData->ulSeq, IoOp::Write, result, pBackend->m_userData);
    }
}

// ========== 连接生命周期: 内核 socket API 包装 ==========

int EvIoBackend::CreateListenSocket(const char* ip, uint16_t port, bool boReusePort, int backlog)
{
    sockaddr_in addr;
    if (!IpPort2SockAddr(ip, port, addr))
    {
        return -1;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -1;
    }

    int iOpt = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &iOpt, sizeof(iOpt)) != 0)
    {
        ::close(fd);
        return -1;
    }

#ifdef SO_REUSEPORT
    if (boReusePort)
    {
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &iOpt, sizeof(iOpt)) != 0)
        {
            ::close(fd);
            return -1;
        }
    }
#endif

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        ::close(fd);
        return -1;
    }

    if (::listen(fd, backlog) < 0)
    {
        ::close(fd);
        return -1;
    }

    return fd;
}

int EvIoBackend::Accept(int listenFd, PeerAddr& outPeerAddr)
{
    sockaddr_in stClientAddr;
    socklen_t clientAddrSize = sizeof(stClientAddr);
    int clientFd = ::accept(listenFd, reinterpret_cast<sockaddr*>(&stClientAddr), &clientAddrSize);
    if (clientFd < 0)
    {
        return -1;
    }

    // 填充对端地址
    if (inet_ntop(AF_INET, &stClientAddr.sin_addr, outPeerAddr.ip, sizeof(outPeerAddr.ip)) != nullptr)
    {
        outPeerAddr.port = ntohs(stClientAddr.sin_port);
    }

    // 设置 TCP_NODELAY 等属性
    SetSocketOpt(clientFd);

    return clientFd;
}

void EvIoBackend::CloseFd(int fd)
{
    if (fd >= 0)
    {
        ::close(fd);
    }
}

void EvIoBackend::SetSocketOpt(int fd)
{
    if (fd < 0) return;

    int iOpt = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &iOpt, sizeof(iOpt));

    iOpt = 60;  // keepalive idle 60s
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &iOpt, sizeof(iOpt));
}

bool EvIoBackend::GetPeerName(int fd, PeerAddr& outAddr)
{
    sockaddr_in addr;
    socklen_t addrLen = sizeof(addr);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0)
    {
        return false;
    }
    if (inet_ntop(AF_INET, &addr.sin_addr, outAddr.ip, sizeof(outAddr.ip)) == nullptr)
    {
        return false;
    }
    outAddr.port = ntohs(addr.sin_port);
    return true;
}

} /* namespace net */
