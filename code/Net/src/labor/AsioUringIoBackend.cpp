/*******************************************************************************
 * Project:  Net
 * @file     AsioUringIoBackend.cpp
 * @brief    Single-threaded: io_context runs on libev main loop via
 *           ev_prepare/ev_check hooks + ev_io ring_fd wakeup.
 ******************************************************************************/
#ifdef THUNDER_IO_ASIO_URING

#include "AsioUringIoBackend.hpp"
#include "util/CBuffer.hpp"
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace net
{

AsioUringIoBackend::AsioUringIoBackend() = default;

AsioUringIoBackend::~AsioUringIoBackend()
{
    Destroy();
}

int AsioUringIoBackend::FindIoUringRingFd()
{
    DIR* dir = opendir("/proc/self/fd");
    if (!dir) return -1;
    int found = -1;
    struct dirent* e;
    while ((e = readdir(dir)) != nullptr)
    {
        char src[64], dst[256];
        snprintf(src, sizeof(src), "/proc/self/fd/%s", e->d_name);
        ssize_t n = ::readlink(src, dst, sizeof(dst) - 1);
        if (n > 0 && strncmp(dst, "anon_inode:[io_uring]", 21) == 0)
        {
            found = atoi(e->d_name);
            break;
        }
    }
    closedir(dir);
    return found;
}

bool AsioUringIoBackend::Init(struct ev_loop* loop, IoCompletionCallback callback, void* user_data)
{
    m_loop     = loop;
    m_callback = callback;
    m_userData = user_data;
    m_workGuard.emplace(m_ioCtx.get_executor());

    /* Trigger lazy io_uring_service initialization */
    int pfd[2] = {-1, -1};
    if (::pipe2(pfd, O_NONBLOCK | O_CLOEXEC) == 0)
    {
        {
            asio::posix::stream_descriptor tmp(m_ioCtx, pfd[0]);
            (void)tmp.release();
        }
        ::close(pfd[0]);
        ::close(pfd[1]);
        (void)m_ioCtx.poll();
    }

    /* Register ring_fd with libev so epoll_wait wakes when CQEs arrive */
    m_ringFd = FindIoUringRingFd();
    if (m_ringFd >= 0)
    {
        ev_io_init(&m_ringWatcher, &OnRingReady, m_ringFd, EV_READ);
        m_ringWatcher.data = this;
        ev_io_start(loop, &m_ringWatcher);
    }

    /* ev_prepare: drain CQEs before libev blocks in epoll_wait
     * ev_check:   drain CQEs that arrived during the epoll_wait window */
    ev_prepare_init(&m_prepare, &OnPrepare);
    m_prepare.data = this;
    ev_prepare_start(loop, &m_prepare);

    ev_check_init(&m_check, &OnCheck);
    m_check.data = this;
    ev_check_start(loop, &m_check);

    return true;
}

void AsioUringIoBackend::Destroy()
{
    if (!m_loop) return;

    if (m_ringFd >= 0) ev_io_stop(m_loop, &m_ringWatcher);
    ev_prepare_stop(m_loop, &m_prepare);
    ev_check_stop(m_loop, &m_check);

    for (auto& [fd, sp] : m_fds)
    {
        asio::error_code ec;
        sp->cancelled = true;
        sp->sock.cancel(ec);
        (void)sp->sock.release();
    }
    m_fds.clear();

    m_workGuard.reset();
    (void)m_ioCtx.poll();
    m_ioCtx.stop();

    m_loop      = nullptr;
    m_callback  = nullptr;
    m_userData  = nullptr;
}

std::shared_ptr<AsioUringIoBackend::FdState>& AsioUringIoBackend::EnsureFdState(int fd)
{
    auto it = m_fds.find(fd);
    if (it != m_fds.end()) return it->second;
    auto sp = std::make_shared<FdState>(m_ioCtx, fd);
    auto [ins, _] = m_fds.emplace(fd, std::move(sp));
    return ins->second;
}

bool AsioUringIoBackend::SubmitRead(int fd, util::CBuffer* buf, uint32_t seq)
{
    if (!buf || !m_callback) return false;
    auto& sp = EnsureFdState(fd);
    if (sp->readPending) return true;
    sp->readPending = true;

    buf->EnsureWritableBytes(8192);
    char*  dst = const_cast<char*>(buf->GetRawWriteBuffer());
    size_t cap = buf->WriteableBytes();

    std::weak_ptr<FdState> wp = sp;
    sp->sock.async_read_some(
        asio::buffer(dst, cap),
        [this, wp, fd, seq, buf](const asio::error_code& ec, std::size_t n)
        {
            auto live = wp.lock();
            if (!live || live->cancelled) return;
            live->readPending = false;
            if (ec)
            {
                if (ec != asio::error::operation_aborted)
                    m_callback(fd, seq, IoOp::Read, -ec.value(), m_userData);
                return;
            }
            if (n == 0) { m_callback(fd, seq, IoOp::Read, 0, m_userData); return; }
            buf->AdvanceWriteIndex(static_cast<int>(n));
            m_callback(fd, seq, IoOp::Read, static_cast<int>(n), m_userData);
        });
    return true;
}

bool AsioUringIoBackend::SubmitWrite(int fd, util::CBuffer* buf, uint32_t seq)
{
    if (!buf || !m_callback) return false;
    int readable = static_cast<int>(buf->ReadableBytes());
    if (readable <= 0)
    {
        m_callback(fd, seq, IoOp::Write, 0, m_userData);
        return true;
    }
    auto& sp = EnsureFdState(fd);
    if (sp->writePending) return true;
    sp->writePending = true;

    const char* src = buf->GetRawReadBuffer();

    std::weak_ptr<FdState> wp = sp;
    sp->sock.async_write_some(
        asio::buffer(src, readable),
        [this, wp, fd, seq, buf](const asio::error_code& ec, std::size_t n)
        {
            auto live = wp.lock();
            if (!live || live->cancelled) return;
            live->writePending = false;
            if (ec)
            {
                if (ec != asio::error::operation_aborted)
                    m_callback(fd, seq, IoOp::Write, -ec.value(), m_userData);
                return;
            }
            buf->AdvanceReadIndex(static_cast<int>(n));
            m_callback(fd, seq, IoOp::Write, static_cast<int>(n), m_userData);
        });
    return true;
}

void AsioUringIoBackend::CancelFd(int fd)
{
    auto it = m_fds.find(fd);
    if (it == m_fds.end()) return;
    asio::error_code ec;
    it->second->cancelled = true;
    it->second->sock.cancel(ec);
    (void)it->second->sock.release();
    m_fds.erase(it);
}

bool AsioUringIoBackend::HasPending(int fd) const
{
    (void)fd;
    return false;
}

void AsioUringIoBackend::OnPrepare(struct ev_loop*, ev_prepare* w, int)
{
    (void)static_cast<AsioUringIoBackend*>(w->data)->m_ioCtx.poll();
}

void AsioUringIoBackend::OnCheck(struct ev_loop*, ev_check* w, int)
{
    (void)static_cast<AsioUringIoBackend*>(w->data)->m_ioCtx.poll();
}

void AsioUringIoBackend::OnRingReady(struct ev_loop*, ev_io* w, int)
{
    (void)static_cast<AsioUringIoBackend*>(w->data)->m_ioCtx.poll();
}

} /* namespace net */

#endif /* THUNDER_IO_ASIO_URING */
