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

    // send_zc：env 门控（THUNDER_URING_ZC=1），默认关。阈值默认 16KB
    // （小包零拷贝固定开销 > 省下的 memcpy，净亏，见 docs）。
    if (const char* z = ::getenv("THUNDER_URING_ZC"); z && z[0] == '1')
    {
        m_zcEnabled = true;
        if (const char* t = ::getenv("THUNDER_URING_ZC_THRESHOLD"))
        {
            long v = ::atol(t);
            if (v >= 1024 && v <= (4 << 20)) m_zcThreshold = static_cast<size_t>(v);
        }
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

    if (m_zcEnabled && static_cast<size_t>(readable) >= m_zcThreshold)
    {
        // send_zc：拷到后端自有 bounce 缓冲再零拷贝发送，使内核 DMA 的
        // 内存与连接 buffer 解耦——DestroyConnect 同步释放 pSendBuff 不会
        // UAF。bounce 缓冲在 NOTIF CQE（buffer 可复用）时释放。
        // 注：此拷贝使本步收益≈0（拷贝挪位），真零拷贝需后续让
        // DestroyConnect 延迟到 NOTIF 才释放连接 buffer（见 docs）。
        po->isZc  = true;
        po->zcBuf = static_cast<char*>(::malloc(static_cast<size_t>(readable)));
        if (!po->zcBuf)   // 退回普通 send
        {
            po->isZc = false;
            ::io_uring_prep_send(sqe, fd, src, static_cast<size_t>(readable), 0);
        }
        else
        {
            std::memcpy(po->zcBuf, src, static_cast<size_t>(readable));
            ::io_uring_prep_send_zc(sqe, fd, po->zcBuf,
                                    static_cast<size_t>(readable), 0, 0);
        }
    }
    else
    {
        ::io_uring_prep_send(sqe, fd, src, static_cast<size_t>(readable), 0);
    }

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
        int      res   = cqe->res;
        unsigned flags = cqe->flags;        // 必须在 cqe_seen 前取（zc 双 CQE 靠 flags 分流）
        ::io_uring_cqe_seen(&m_ring, cqe);
        if (!po) continue;

        auto it = m_fds.find(po->fd);
        bool valid = (it != m_fds.end()
                      && it->second.seq == po->seq
                      && !it->second.cancelled);

        // ---- send_zc 双 CQE ----
        if (po->isZc)
        {
            if (flags & IORING_CQE_F_NOTIF)
            {
                // 通知 CQE：内核已脱离 bounce 缓冲，可安全释放/复用。
                int bytes = po->gotResult ? po->zcBytes : res;
                if (valid)
                {
                    if (it->second.writePending > 0) it->second.writePending--;
                    if (bytes > 0) po->buf->AdvanceReadIndex(bytes);
                    m_callback(po->fd, po->seq, IoOp::WriteNotif, bytes, m_userData);
                }
                ::free(po->zcBuf);
                delete po;          // 终态
            }
            else
            {
                // 结果 CQE：记录字节数。F_MORE 表示通知 CQE 仍会来。
                po->zcBytes   = res;
                po->gotResult = true;
                if (!(flags & IORING_CQE_F_MORE))
                {
                    // 无通知后续（多为立即失败）：此刻即终态，
                    // 直接以 WriteNotif 驱动 Worker 回收/报错。
                    if (valid)
                    {
                        if (it->second.writePending > 0) it->second.writePending--;
                        if (res > 0) po->buf->AdvanceReadIndex(res);
                        m_callback(po->fd, po->seq, IoOp::WriteNotif, res, m_userData);
                    }
                    ::free(po->zcBuf);
                    delete po;
                }
                // else：F_MORE → 等通知 CQE，保留 po/zcBuf，不回调不减 pending
            }
            continue;
        }

        // ---- 普通 Read/Write（单 CQE）----
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
