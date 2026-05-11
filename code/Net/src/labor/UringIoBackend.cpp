/*******************************************************************************
 * Project:  Net
 * @file     UringIoBackend.cpp
 * @brief    io_uring-based I/O backend implementation
 ******************************************************************************/
#include "UringIoBackend.hpp"

#ifdef THUNDER_IO_URING

#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include "util/CBuffer.hpp"

/*
 * 架构说明（与 EvIoBackend / 纯用户态栈的区别）：
 * - Linux 上 libev 背后多为 epoll；此处 ev_io 只监听 io_uring 的 ring_fd，表示「完成队列可收割」时醒来。
 * - 业务 socket 的读写在 io_uring 内由内核执行；用户态负责填 SQE、io_uring_submit、peek CQE、io_uring_cqe_seen。
 * - 这不是「全程用户态网络栈」；数据仍经内核协议栈进入用户提供的缓冲区。
 * - 相对「每个 fd 挂 epoll + 每次 read/write 一次 syscall」，通常 syscall 更少、完成事件更易批处理。
 */

namespace net
{

UringIoBackend::~UringIoBackend()
{
    Destroy();
}

bool UringIoBackend::Init(struct ev_loop* loop, IoCompletionCallback callback, void* user_data)
{
    if (!loop || !callback)
    {
        return false;
    }

    // 初始化 io_uring 队列，失败则返回 false
    if (io_uring_queue_init(kQueueDepth, &m_ring, 0) < 0)
    {
        return false;
    }
    m_ringInitialized = true;
    m_loop = loop;
    m_callback = callback;
    m_userData = user_data;

    // 注册 io_uring fd 到 libev
    m_pRingWatcher = new ev_io();
    if (!m_pRingWatcher)
    {
        io_uring_queue_exit(&m_ring);
        m_ringInitialized = false;
        return false;
    }

    ev_io_init(m_pRingWatcher, RingEventCallback, m_ring.ring_fd, EV_READ);
    m_pRingWatcher->data = static_cast<void*>(this);
    ev_io_start(m_loop, m_pRingWatcher);

    return true;
}

void UringIoBackend::Destroy()
{
    if (m_pRingWatcher)
    {
        ev_io_stop(m_loop, m_pRingWatcher);
        delete m_pRingWatcher;
        m_pRingWatcher = nullptr;
    }

    if (m_ringInitialized)
    {
        io_uring_queue_exit(&m_ring);
        m_ringInitialized = false;
    }

    m_mapPending.clear();
    m_loop = nullptr;
    m_callback = nullptr;
    m_userData = nullptr;
}

bool UringIoBackend::SubmitRead(int fd, util::CBuffer* buf, uint32_t seq)
{
    if (!m_ringInitialized || !buf)
    {
        return false;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe)
    {
        return false;
    }

    // Ensure the buffer has writable space (may be uninitialized/default-constructed)
    // Note: do NOT call Compact on an empty/new buffer — Compact frees the buffer when
    // ReadableBytes()==0, undoing the allocation.
    buf->EnsureWritableBytes(8192);

    // 为本次操作分配唯一的 user_data（用于后续完成事件的识别）
    uint64_t user_data = m_nextUserData++;
    // 设置 io_uring 的 recv 操作（socket-safe），准备从 fd 读取数据到缓冲区
    io_uring_prep_recv(sqe, fd, const_cast<char*>(buf->GetRawWriteBuffer()),
                       buf->WriteableBytes(), 0);
    // 将 user_data 附加到 SQE，用于后续完成事件匹配
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(user_data));

    PendingOp op;
    op.fd = fd;
    op.seq = seq;
    op.op = IoOp::Read;
    op.buf = buf;
    m_mapPending[user_data] = op;

    io_uring_submit(&m_ring);
    return true;
}

bool UringIoBackend::SubmitWrite(int fd, util::CBuffer* buf, uint32_t seq)
{
    // For writes, fall back to synchronous approach. io_uring async writes
    // can interact poorly with the event-driven TLS/codec state machines,
    // adding unnecessary round-trips for small/buffered sends that complete
    // synchronously in the kernel anyway.
    if (!buf)
    {
        return false;
    }

    int readable = static_cast<int>(buf->ReadableBytes());
    if (readable <= 0)
    {
        // Nothing to write - call completion immediately with 0
        m_callback(fd, seq, IoOp::Write, 0, m_userData);
        return true;
    }

    // Synchronous write via the buffer's own WriteFD
    int iErrno = 0;
    int written = buf->WriteFD(fd, iErrno);
    buf->Compact(8192);

    if (written > 0)
    {
        // Call completion callback with the write result
        m_callback(fd, seq, IoOp::Write, written, m_userData);
    }
    else if (written < 0 && (iErrno == EAGAIN || iErrno == EINTR))
    {
        // Partial write - notify with negative errno for retry
        m_callback(fd, seq, IoOp::Write, -iErrno, m_userData);
    }
    else
    {
        // Error
        m_callback(fd, seq, IoOp::Write, written, m_userData);
    }

    return true;
}

void UringIoBackend::CancelFd(int fd)
{
    // 移除该 fd 的所有待处理操作
    auto it = m_mapPending.begin();
    while (it != m_mapPending.end())
    {
        if (it->second.fd == fd)
        {
            it = m_mapPending.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool UringIoBackend::HasPending(int fd) const
{
    for (const auto& pair : m_mapPending)
    {
        if (pair.second.fd == fd)
        {
            return true;
        }
    }
    return false;
}

void UringIoBackend::ReapCqes()
{
    struct io_uring_cqe* cqe = nullptr;
    unsigned head;
    int count = 0;

    io_uring_for_each_cqe(&m_ring, head, cqe)
    {
        if (count >= (int)kMaxCqeBatch)
            break;

        // 从完成队列条目中获取用户数据
        uint64_t user_data = reinterpret_cast<uint64_t>(io_uring_cqe_get_data(cqe));
        // 获取完成队列条目的结果
        int result = cqe->res;

        auto it = m_mapPending.find(user_data);
        if (it != m_mapPending.end())
        {
            PendingOp op = it->second;
            m_mapPending.erase(it);

            if (result >= 0 && op.buf)
            {
                if (op.op == IoOp::Read)
                    op.buf->AdvanceWriteIndex(result);
                else
                    op.buf->AdvanceReadIndex(result);
            }

            m_callback(op.fd, op.seq, op.op, result, m_userData);
        }
        // else: CQE with no matching pending entry — stale/duplicate completion
        // (e.g. from CancelFd removing the entry while the kernel still completed it)

        ++count;
    }

    // Advance the CQ ring
    io_uring_cq_advance(&m_ring, count);
}

void UringIoBackend::RingEventCallback(struct ev_loop* loop, struct ev_io* watcher, int revents)
{
    if (!watcher->data)
    {
        return;
    }
    // 将 watcher 的 data 指针转换为 UringIoBackend 指针
    UringIoBackend* pBackend = static_cast<UringIoBackend*>(watcher->data);
    // 如果事件是读事件，则调用 ReapCqes 方法，处理完成队列中的操作
    if (revents & EV_READ)
    {
        // 处理完成队列中的操作
        pBackend->ReapCqes();
    }
}

} /* namespace net */

#endif /* THUNDER_IO_URING */
