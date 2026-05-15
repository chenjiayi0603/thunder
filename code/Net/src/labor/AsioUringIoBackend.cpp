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
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <exception>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <cstring>

namespace {

// 5.B: THUNDER_ASIO_URING_DIAG=1 才输出，默认关闭，防止生产环境 fflush 风暴
static bool g_diag_enabled = (::getenv("THUNDER_ASIO_URING_DIAG") != nullptr);
FILE* g_diag_fp = nullptr;
__thread char g_diag_tmp[512];

void diag_log(const char* fmt, ...) {
    if (!g_diag_enabled) return;  // 5.B: 默认不输出
    if (!g_diag_fp) {
        g_diag_fp = fopen("/tmp/asio_uring_diag.log", "a");
    }
    if (g_diag_fp) {
        time_t now = time(nullptr);
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);
        int off = snprintf(g_diag_tmp, sizeof(g_diag_tmp),
                          "[%02d:%02d:%02d] ", tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(g_diag_tmp + off, sizeof(g_diag_tmp) - off, fmt, ap);
        va_end(ap);
        fputs(g_diag_tmp, g_diag_fp);
        fflush(g_diag_fp);
    }
}

// 5.D: 诊断计数器（env 门控，每秒输出一次，用于修复前后对比空唤醒率）
struct DiagStats {
    uint64_t ring_ready   = 0;  // OnRingReady 触发次数
    uint64_t ring_empty   = 0;  // 其中 poll()=0（NOP/中断 CQE，无用户 op）
    uint64_t ring_real    = 0;  // 其中 poll()>0（含真实完成事件）
    uint64_t prepare_real = 0;  // OnPrepare poll()>0 次数
    uint64_t check_real   = 0;  // OnCheck poll()>0 次数
    time_t   last_flush   = 0;

    void tick() {
        if (!g_diag_enabled) return;
        time_t now = time(nullptr);
        if (now == last_flush) return;
        last_flush = now;
        diag_log("[IODIAG STATS/s ring_ready=%lu ring_empty=%lu ring_real=%lu "
                 "prepare_real=%lu check_real=%lu\n",
                 ring_ready, ring_empty, ring_real, prepare_real, check_real);
        ring_ready = ring_empty = ring_real = prepare_real = check_real = 0;
    }
} g_stats;

} // namespace

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

// 5.A: 按需启停 ring_fd 监听 — 有挂起 op 才 start，无则 stop，断开 NOP 驱动的忙循环
void AsioUringIoBackend::UpdateRingWatcher()
{
    if (m_ringFd < 0) return;
    bool hasOp = false;
    for (auto& [fd, sp] : m_fds)
    {
        if (sp->readPending || sp->writePending) { hasOp = true; break; }
    }
    if (hasOp && !m_ringWatcherActive)
    {
        ev_io_start(m_loop, &m_ringWatcher);
        m_ringWatcherActive = true;
        diag_log("[IODIAG AsioUring RingWatcher START\n");
    }
    else if (!hasOp && m_ringWatcherActive)
    {
        ev_io_stop(m_loop, &m_ringWatcher);
        m_ringWatcherActive = false;
        diag_log("[IODIAG AsioUring RingWatcher STOP\n");
    }
}

// A1: 注册缓冲池 — env 门控（THUNDER_ASIO_URING_FIXEDBUF=1），默认关，不回归生产默认。
// 仅写路径使用：写完成后归还槽；本质是 send_zc 的注册池+槽生命周期地基。
bool AsioUringIoBackend::InitFixedBuffers()
{
    const char* en = ::getenv("THUNDER_ASIO_URING_FIXEDBUF");
    if (!en || en[0] != '1') { m_fixedBufEnabled = false; return false; }

    if (const char* s = ::getenv("THUNDER_ASIO_URING_FIXEDBUF_SLOTS"))
    { long v = ::atol(s); if (v > 0 && v <= 65536) m_slotCount = static_cast<size_t>(v); }
    if (const char* s = ::getenv("THUNDER_ASIO_URING_FIXEDBUF_SLOTSIZE"))
    { long v = ::atol(s); if (v >= 4096 && v <= (16 << 20)) m_slotSize = static_cast<size_t>(v); }

    size_t total = m_slotCount * m_slotSize;
    m_slotMem = std::make_unique<char[]>(total);

    std::vector<asio::mutable_buffer> seq;
    seq.reserve(m_slotCount);
    for (size_t i = 0; i < m_slotCount; ++i)
        seq.emplace_back(m_slotMem.get() + i * m_slotSize, m_slotSize);

    try
    {
        m_bufReg = std::make_unique<
            asio::buffer_registration<std::vector<asio::mutable_buffer>>>(
                m_ioCtx.get_executor(), seq);
    }
    catch (const std::exception& e)
    {
        diag_log("[IODIAG AsioUring FixedBuf register FAILED: %s\n", e.what());
        m_bufReg.reset();
        m_slotMem.reset();
        m_fixedBufEnabled = false;
        return false;
    }

    m_freeSlots.clear();
    m_freeSlots.reserve(m_slotCount);
    for (int i = static_cast<int>(m_slotCount) - 1; i >= 0; --i)
        m_freeSlots.push_back(i);
    m_fixedBufEnabled = true;
    diag_log("[IODIAG AsioUring FixedBuf enabled slots=%zu size=%zu total=%zuMB\n",
             m_slotCount, m_slotSize, total >> 20);
    return true;
}

int AsioUringIoBackend::BorrowSlot()
{
    if (!m_fixedBufEnabled || m_freeSlots.empty()) return -1;
    int idx = m_freeSlots.back();
    m_freeSlots.pop_back();
    return idx;
}

void AsioUringIoBackend::ReturnSlot(int idx)
{
    if (idx >= 0 && idx < static_cast<int>(m_slotCount))
        m_freeSlots.push_back(idx);
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

    /* A1: io_uring_service 已惰性初始化，此处注册固定缓冲池（env 门控） */
    InitFixedBuffers();

    /* 初始化 ring_fd watcher，但 5.A: 不立即 start，等 SubmitRead/SubmitWrite 时按需启动 */
    m_ringFd = FindIoUringRingFd();
    diag_log("[IODIAG AsioUring Init ring_fd=%d\n", m_ringFd);
    if (m_ringFd >= 0)
    {
        ev_io_init(&m_ringWatcher, &OnRingReady, m_ringFd, EV_READ);
        m_ringWatcher.data = this;
        // 5.A: 不在此处 start，由 UpdateRingWatcher() 按需控制
    }

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

    if (m_ringFd >= 0 && m_ringWatcherActive) ev_io_stop(m_loop, &m_ringWatcher);
    m_ringWatcherActive = false;
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

    // A1: 先注销注册池（RAII 调 unregister_buffers），再拆 io_context
    m_bufReg.reset();
    m_slotMem.reset();
    m_freeSlots.clear();
    m_fixedBufEnabled = false;

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
    if (!buf || !m_callback) {
        diag_log("[IODIAG AsioUring SubmitRead fd=%d FAIL buf=%p callback=%p\n", fd, (void*)buf, (void*)m_callback);
        return false;
    }
    auto& sp = EnsureFdState(fd);
    if (sp->readPending) {
        diag_log("[IODIAG AsioUring SubmitRead fd=%d SKIP (readPending)\n", fd);
        return true;
    }
    sp->readPending = true;

    buf->EnsureWritableBytes(8192);
    char*  dst = const_cast<char*>(buf->GetRawWriteBuffer());
    size_t cap = buf->WriteableBytes();

    diag_log("[IODIAG AsioUring SubmitRead fd=%d seq=%u buf_cap=%zu\n", fd, seq, cap);

    std::weak_ptr<FdState> wp = sp;
    sp->sock.async_read_some(
        asio::buffer(dst, cap),
        [this, wp, fd, seq, buf](const asio::error_code& ec, std::size_t n)
        {
            auto live = wp.lock();
            if (!live || live->cancelled) {
                diag_log("[IODIAG AsioUring ReadComplete fd=%d seq=%u DROPPED live=%d cancelled=%d\n",
                        fd, seq, (live!=nullptr), (live ? live->cancelled : -1));
                return;
            }
            live->readPending = false;
            if (ec)
            {
                diag_log("[IODIAG AsioUring ReadComplete fd=%d seq=%u ERROR ec=%d msg=%s\n",
                        fd, seq, ec.value(), ec.message().c_str());
                if (ec != asio::error::operation_aborted)
                    m_callback(fd, seq, IoOp::Read, -ec.value(), m_userData);
                return;
            }
            diag_log("[IODIAG AsioUring ReadComplete fd=%d seq=%u n=%zu\n", fd, seq, n);
            if (n == 0) { m_callback(fd, seq, IoOp::Read, 0, m_userData); return; }
            buf->AdvanceWriteIndex(static_cast<int>(n));
            m_callback(fd, seq, IoOp::Read, static_cast<int>(n), m_userData);
        });
    // 5.C: 移除再入 m_ioCtx.poll()，SQE 由下一个 OnPrepare 统一批量 flush
    UpdateRingWatcher();  // 5.A: 有挂起读 op，确保 ring_fd 监听已启动
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

    // A1: 满足「池启用 && 有空槽 && payload≤槽」走注册缓冲（命中 prep_write_fixed）。
    // 否则回退既有非注册路径（即今天的行为，零风险）。
    int slot = -1;
    if (m_fixedBufEnabled && static_cast<size_t>(readable) <= m_slotSize)
        slot = BorrowSlot();

    std::weak_ptr<FdState> wp = sp;

    if (slot >= 0)
    {
        std::memcpy(m_slotMem.get() + static_cast<size_t>(slot) * m_slotSize,
                    src, static_cast<size_t>(readable));
        sp->sock.async_write_some(
            asio::buffer((*m_bufReg)[static_cast<std::size_t>(slot)],
                         static_cast<std::size_t>(readable)),
            [this, wp, fd, seq, buf, slot](const asio::error_code& ec, std::size_t n)
            {
                // 普通写：CQE 到达时内核已消费缓冲，任何分支都先归还槽防泄漏。
                // （send_zc 接入后此处改为 NOTIF CQE 才归还）
                ReturnSlot(slot);
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
    }
    else
    {
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
    }
    // 5.C: 移除再入 m_ioCtx.poll()，SQE 由下一个 OnPrepare 统一批量 flush
    UpdateRingWatcher();  // 5.A: 有挂起写 op，确保 ring_fd 监听已启动
    return true;
}

void AsioUringIoBackend::CancelFd(int fd)
{
    auto it = m_fds.find(fd);
    if (it == m_fds.end()) return;
    it->second->cancelled = true;
    // Do NOT call sock.cancel() — it submits an async IORING_OP_ASYNC_CANCEL
    // that races with subsequent SubmitRead/SubmitWrite on the same fd.
    (void)it->second->sock.release();
    m_fds.erase(it);
    UpdateRingWatcher();  // 5.A: fd 移除后检查是否还有挂起 op
}

bool AsioUringIoBackend::HasPending(int fd) const
{
    auto it = m_fds.find(fd);
    return it != m_fds.end() && (it->second->readPending || it->second->writePending);
}

void AsioUringIoBackend::OnPrepare(struct ev_loop*, ev_prepare* w, int)
{
    auto* be = static_cast<AsioUringIoBackend*>(w->data);
    auto n = be->m_ioCtx.poll();
    if (n > 0) {
        diag_log("[IODIAG AsioUring OnPrepare poll=%zu\n", n);
        ++g_stats.prepare_real;
    }
}

void AsioUringIoBackend::OnCheck(struct ev_loop*, ev_check* w, int)
{
    auto* be = static_cast<AsioUringIoBackend*>(w->data);
    auto n = be->m_ioCtx.poll();
    if (n > 0) {
        diag_log("[IODIAG AsioUring OnCheck poll=%zu\n", n);
        ++g_stats.check_real;
    }
    be->UpdateRingWatcher();  // 5.A: 完成收割后，若无挂起 op 则停止 ring_fd 监听
    g_stats.tick();           // 5.D: 每秒输出一次诊断计数
}

void AsioUringIoBackend::OnRingReady(struct ev_loop*, ev_io* w, int)
{
    auto* be = static_cast<AsioUringIoBackend*>(w->data);
    auto n = be->m_ioCtx.poll();
    // 5.D: 统计空唤醒（n=0 表示本次 poll 仅处理了 NOP/中断 CQE，无用户完成事件）
    ++g_stats.ring_ready;
    if (n == 0) ++g_stats.ring_empty;
    else        ++g_stats.ring_real;
    diag_log("[IODIAG AsioUring OnRingReady ring_fd=%d poll=%zu\n", be->m_ringFd, n);
}

} /* namespace net */

#endif /* THUNDER_IO_ASIO_URING */
