/*******************************************************************************
 * Project:  Net
 * @file     NativeUringIoBackend.cpp
 * @brief    原生 liburing 后端骨架（普通 recv/send；send_zc 见 Path B-3）
 ******************************************************************************/
#include "NativeUringIoBackend.hpp"
#include "util/CBuffer.hpp"
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <sys/eventfd.h>

namespace net
{

NativeUringIoBackend::NativeUringIoBackend() = default;

NativeUringIoBackend::~NativeUringIoBackend()
{
    Destroy();
}

bool NativeUringIoBackend::Init(struct ev_loop* loop, IoCompletionCallback callback, void* user_data)
{
    m_loop     = loop;
    m_callback = callback;
    m_userData = user_data;

    if (const char* s = ::getenv("THUNDER_URING_SQDEPTH"))
    {
        long v = ::atol(s);
        if (v >= 256 && v <= 32768) m_sqDepth = static_cast<unsigned>(v);
    }

    int rc;
    const char* sqp = ::getenv("THUNDER_URING_SQPOLL");
    if (sqp && sqp[0] == '1')
    {
        // 内核 7.0 SQPOLL 免 CAP；sq_thread_idle 设小防自旋吃 cgroup CPU 配额
        struct ::io_uring_params params;
        std::memset(&params, 0, sizeof(params));
        params.flags = IORING_SETUP_SQPOLL;
        unsigned idle = 100;
        if (const char* i = ::getenv("THUNDER_URING_SQPOLL_IDLE"))
        {
            long v = ::atol(i);
            if (v > 0 && v <= 60000) idle = static_cast<unsigned>(v);
        }
        params.sq_thread_idle = idle;
        rc = ::io_uring_queue_init_params(m_sqDepth, &m_ring, &params);
        if (rc < 0)  // SQPOLL 不可用（权限/seccomp）→ 退普通模式
        {
            rc = ::io_uring_queue_init(m_sqDepth, &m_ring, 0);
        }
    }
    else
    {
        rc = ::io_uring_queue_init(m_sqDepth, &m_ring, 0);
    }
    if (rc < 0)
    {
        return false;   // Labor 据此回退 ev
    }
    m_ringInit = true;

    m_evfd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_evfd < 0)
    {
        ::io_uring_queue_exit(&m_ring);
        m_ringInit = false;
        return false;
    }
    if (::io_uring_register_eventfd(&m_ring, m_evfd) < 0)
    {
        ::close(m_evfd);
        m_evfd = -1;
        ::io_uring_queue_exit(&m_ring);
        m_ringInit = false;
        return false;
    }

    ev_io_init(&m_evWatcher, &OnEvfd, m_evfd, EV_READ);
    m_evWatcher.data = this;
    ev_io_start(loop, &m_evWatcher);

    ev_check_init(&m_check, &OnCheck);
    m_check.data = this;
    ev_check_start(loop, &m_check);

    m_started = true;
    return true;
}

void NativeUringIoBackend::Destroy()
{
    if (m_loop && m_started)
    {
        ev_io_stop(m_loop, &m_evWatcher);
        ev_check_stop(m_loop, &m_check);
        m_started = false;
    }
    m_fds.clear();
    if (m_evfd >= 0)
    {
        ::close(m_evfd);
        m_evfd = -1;
    }
    if (m_ringInit)
    {
        ::io_uring_queue_exit(&m_ring);
        m_ringInit = false;
    }
    m_loop     = nullptr;
    m_callback = nullptr;
    m_userData = nullptr;
}

bool NativeUringIoBackend::GetSqe(struct io_uring_sqe** out)
{
    struct io_uring_sqe* sqe = ::io_uring_get_sqe(&m_ring);
    if (!sqe)
    {
        // SQ 满：先 flush 已排队的 SQE，再取一次
        ::io_uring_submit(&m_ring);
        sqe = ::io_uring_get_sqe(&m_ring);
        if (!sqe) return false;
    }
    *out = sqe;
    return true;
}

bool NativeUringIoBackend::SubmitRead(int fd, util::CBuffer* buf, uint32_t seq)
{
    if (!buf || !m_callback) return false;

    auto& st = m_fds[fd];
    if (st.seq != seq)   // fd 复用：重置状态
    {
        st.seq = seq;
        st.readPending = st.writePending = 0;
        st.cancelled = false;
    }
    if (st.readPending > 0) return true;   // 已有读在途（与 Asio 后端语义一致）

    buf->EnsureWritableBytes(8192);
    char*  dst = const_cast<char*>(buf->GetRawWriteBuffer());
    size_t cap = buf->WriteableBytes();

    struct io_uring_sqe* sqe = nullptr;
    if (!GetSqe(&sqe)) return false;

    PendingOp* po = new PendingOp{fd, seq, IoOp::Read, buf};
    ::io_uring_prep_recv(sqe, fd, dst, cap, 0);
    ::io_uring_sqe_set_data(sqe, po);
    st.readPending++;
    ::io_uring_submit(&m_ring);
    return true;
}

bool NativeUringIoBackend::SubmitWrite(int fd, util::CBuffer* buf, uint32_t seq)
{
    if (!buf || !m_callback) return false;

    int readable = static_cast<int>(buf->ReadableBytes());
    if (readable <= 0)
    {
        m_callback(fd, seq, IoOp::Write, 0, m_userData);
        return true;
    }

    auto& st = m_fds[fd];
    if (st.seq != seq)
    {
        st.seq = seq;
        st.readPending = st.writePending = 0;
        st.cancelled = false;
    }
    if (st.writePending > 0) return true;

    const char* src = buf->GetRawReadBuffer();

    struct io_uring_sqe* sqe = nullptr;
    if (!GetSqe(&sqe)) return false;

    PendingOp* po = new PendingOp{fd, seq, IoOp::Write, buf};
    ::io_uring_prep_send(sqe, fd, src, static_cast<size_t>(readable), 0);
    ::io_uring_sqe_set_data(sqe, po);
    st.writePending++;
    ::io_uring_submit(&m_ring);
    return true;
}

void NativeUringIoBackend::CancelFd(int fd)
{
    auto it = m_fds.find(fd);
    if (it == m_fds.end()) return;
    // 标记取消并移除 fd 状态。在途 PendingOp 不在此释放——其 CQE 到达时
    // 会因 fd 不在表（或 seq 不符）被识别为陈旧，仅释放 PendingOp，
    // 绝不触碰已被 DestroyConnect 释放的连接缓冲。不提交 ASYNC_CANCEL
    // （与 AsioUringIoBackend 同样规避 cancel 竞态）。
    it->second.cancelled = true;
    m_fds.erase(it);
}

bool NativeUringIoBackend::HasPending(int fd) const
{
    auto it = m_fds.find(fd);
    return it != m_fds.end() && (it->second.readPending > 0 || it->second.writePending > 0);
}

void NativeUringIoBackend::ReapCqes()
{
    if (!m_ringInit) return;
    struct io_uring_cqe* cqe = nullptr;
    while (::io_uring_peek_cqe(&m_ring, &cqe) == 0 && cqe != nullptr)
    {
        PendingOp* po = static_cast<PendingOp*>(::io_uring_cqe_get_data(cqe));
        int res = cqe->res;
        ::io_uring_cqe_seen(&m_ring, cqe);
        if (!po) continue;

        auto it = m_fds.find(po->fd);
        bool valid = (it != m_fds.end()
                      && it->second.seq == po->seq
                      && !it->second.cancelled);

        if (valid)
        {
            if (po->op == IoOp::Read)
            {
                if (it->second.readPending > 0) it->second.readPending--;
                if (res > 0) po->buf->AdvanceWriteIndex(res);
                m_callback(po->fd, po->seq, IoOp::Read, res, m_userData);
            }
            else
            {
                if (it->second.writePending > 0) it->second.writePending--;
                if (res > 0) po->buf->AdvanceReadIndex(res);
                m_callback(po->fd, po->seq, IoOp::Write, res, m_userData);
            }
        }
        // 陈旧/已取消：不触碰 po->buf，不回调，仅释放 PendingOp
        delete po;
    }
}

void NativeUringIoBackend::OnEvfd(struct ev_loop*, ev_io* w, int)
{
    auto* be = static_cast<NativeUringIoBackend*>(w->data);
    uint64_t cnt;
    while (::read(be->m_evfd, &cnt, sizeof(cnt)) == static_cast<ssize_t>(sizeof(cnt))) { /* drain */ }
    be->ReapCqes();
}

void NativeUringIoBackend::OnCheck(struct ev_loop*, ev_check* w, int)
{
    // 兜底：eventfd 通知与 epoll_wait 窗口期到达的 CQE 不积压超过一轮
    static_cast<NativeUringIoBackend*>(w->data)->ReapCqes();
}

} /* namespace net */
