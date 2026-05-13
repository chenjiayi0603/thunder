/*******************************************************************************
 * Project:  Net
 * @file     EvIoBackend.cpp
 * @brief    libev-based I/O backend implementation
 ******************************************************************************/
#include "EvIoBackend.hpp"
#include "util/CBuffer.hpp"

namespace net
{

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

bool EvIoBackend::SubmitRead(int fd, util::CBuffer* buf, uint32_t seq)
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
    pData->pReadBuf = buf;

    // 刷新/启动读事件（幂等）
    ev_io_set(pData->pWatcher, fd, pData->pWatcher->events | EV_READ);
    ev_io_start(m_loop, pData->pWatcher);

    return true;
}

bool EvIoBackend::SubmitWrite(int fd, util::CBuffer* buf, uint32_t seq)
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
    pData->pWriteBuf = buf;

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

} /* namespace net */
