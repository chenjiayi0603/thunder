/*******************************************************************************
 * Project:  Net
 * @file     AsioUringIoBackend.cpp
 * @brief    ASIO io_uring 后端实现 — 单线程，libev 主循环三路驱动 poll()
 *
 * 详见同目录 AsioUringIoBackend.hpp 文件头核心流程与关键设计说明。
 ******************************************************************************/
#ifdef THUNDER_IO_ASIO_URING

#include "AsioUringIoBackend.hpp"
#include "util/CBuffer.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <exception>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// 诊断默认关闭 — 生产环境 fopen+fflush 每条 CQE 路经都有开销，仅排障时开。
static bool g_diag_enabled = (::getenv("THUNDER_ASIO_URING_DIAG") != nullptr);
FILE* g_diag_fp = nullptr;
__thread char g_diag_tmp[512];  // __thread 避免 malloc，诊断路径零分配

void diag_log(const char* fmt, ...) {
    if (!g_diag_enabled) return;
    if (!g_diag_fp) g_diag_fp = fopen("/tmp/asio_uring_diag.log", "a");
    if (!g_diag_fp) return;
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

// 每秒输出一次诊断计数器 — 空唤醒率 (ring_empty/ring_ready) 衡量 NOP-SQE 副作用程度
struct DiagStats {
    uint64_t ring_ready   = 0;  // ring_fd 可读触发次数
    uint64_t ring_empty   = 0;  // 其中 poll()=0 (NOP/中断 CQE, 无用户 op 完成)
    uint64_t ring_real    = 0;  // 其中 poll()>0 (有真实完成事件)
    uint64_t prepare_real = 0;  // OnPrepare 收割到 CQE 次数
    uint64_t check_real   = 0;  // OnCheck 收割到 CQE 次数
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

// 扫描 /proc/self/fd 找到 ASIO io_uring_service 的 ring_fd。
// 原因: ASIO 不暴露 ring_fd，只能通过 readlink 检查每个 fd 指向的 anon_inode
//       是否为 [io_uring] 来定位。仅在 Init 时调用一次，之后缓存于 m_ringFd。
// 安全性: 收集全部 [io_uring] fd，若有多个则取最大值（ASIO service fd 最后创建）
//         并通过日志报告。这样即使进程已有其他 io_uring fd，行为也是确定的。
int AsioUringIoBackend::FindIoUringRingFd()
{
    DIR* dir = opendir("/proc/self/fd");
    if (!dir) return -1;
    int maxFd = -1;
    int count = 0;
    struct dirent* e;
    while ((e = readdir(dir)) != nullptr)
    {
        char src[64], dst[256];
        snprintf(src, sizeof(src), "/proc/self/fd/%s", e->d_name);
        ssize_t n = ::readlink(src, dst, sizeof(dst) - 1);
        if (n > 0 && strncmp(dst, "anon_inode:[io_uring]", 21) == 0)
        {
            int fd = atoi(e->d_name);
            if (fd > maxFd) maxFd = fd;
            ++count;
        }
    }
    closedir(dir);
    if (count > 1)
        diag_log("[IODIAG AsioUring FindIoUringRingFd: found %d io_uring fds, using max=%d\n", count, maxFd);
    return maxFd;
}

// ========== 按需启停三路-第 2 路 ring_fd 监听 ==========
// 核心目的: 消除 idle 场景下的 NOP-SQE 空唤醒，实现 "idle 友好"。
//
// 问题: ASIO waitable reactor 通过 NOP-SQE 在 io_uring 中注册 ring_fd 的可读性。
//       即使无用户 I/O op，内核也可能因中断/定时器产生内部 CQE → ring_fd 可读
//       → epoll_wait 唤醒 → OnRingReady → poll()=0 → 空转（纯浪费 CPU）。
//
// 方案: 有挂起 op 时 start ring_fd 监听，无挂起 op 时 stop。
//       idle 连接不触发任何 op → ring_fd 不监听 → epoll_wait 不被空唤醒。
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

// 注册 io_uring fixed buffers — send_zc 地基。
// 原因: send_zc(MSG_ZEROCOPY) 需要 registered buffers，内核 DMA 直接读写注册内存。
//       当前普通写路径也用此池命中 prep_write_fixed 减少一次内核拷贝。
// env 门控: THUNDER_ASIO_URING_FIXEDBUF=1 才启用，默认关闭（零影响）。
// 回退: 注册失败或未启用时，SubmitWrite 自动回退非注册 async_write_some。
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
    m_workGuard.emplace(m_ioCtx.get_executor());  // 防止 io_context 在无 op 时退出

    // 触发 ASIO io_uring_service 惰性初始化 — 原因:
    // ASIO 不在 io_context 构造时创建 io_uring，而是在第一个 I/O object 构造时。
    // 此处创建临时 pipe + stream_descriptor 触发 io_uring_setup 内核调用，
    // poll() 一次让 ASIO 完成内部注册。pipe 用完即关，仅作初始化 side effect。
    int pfd[2] = {-1, -1};
    if (::pipe2(pfd, O_NONBLOCK | O_CLOEXEC) == 0)
    {
        {
            asio::posix::stream_descriptor tmp(m_ioCtx, pfd[0]);  // 触发 io_uring_service 构造
            (void)tmp.release();  // 归还 fd 所有权，不关闭
        }
        ::close(pfd[0]);
        ::close(pfd[1]);
        (void)m_ioCtx.poll();  // 完成 io_uring_setup + 清理内部状态
    }

    // io_uring_service 已就绪，此时可安全调用 register_buffers()
    InitFixedBuffers();

    // ========== 三路 libev 驱动: 分别挂在 prepare / io(ring_fd) / check 三个钩子 ==========
    //
    // 第 1 路 ev_prepare — 每次 epoll_wait 之前: 批量提交 SQE + 抢先收割 CQE
    //   投递: SubmitRead/Write 攒下的 async ops 在此一次 io_uring_enter 批量提交到内核 SQ
    //   收割: 同时收割已到的 CQE → lambda → m_callback（减少延迟）
    //
    // 第 2 路 ev_io(ring_fd) — ring_fd 可读时: 收割内核新完成的 CQE
    //   接货: 内核完成 I/O → 写入 CQE → 标记 ring_fd 可读 → epoll 唤醒 → poll() 排空整批 CQE
    //   启停: UpdateRingWatcher 按需启停 — 无挂起 op 时 stop，消除 NOP-SQE 空唤醒
    //
    // 第 3 路 ev_check — 每次 epoll_wait 之后: 补刀收割 + 善后
    //   补刀: 收割 OnRingReady 与 epoll_wait 之间 race window 中新到的 CQE
    //   善后: UpdateRingWatcher 更新 ring_fd 监听状态 + 每秒诊断统计

    // 找 ring_fd 并初始化第 2 路 watcher，但不立即 start — 等有 op 时按需启动
    m_ringFd = FindIoUringRingFd();
    diag_log("[IODIAG AsioUring Init ring_fd=%d\n", m_ringFd);
    if (m_ringFd >= 0)
    {
        ev_io_init(&m_ringWatcher, &OnRingReady, m_ringFd, EV_READ);
        m_ringWatcher.data = this;
    }

    // 第 1 路和第 3 路 always-on — 即使无 op 也要运行（准备投递 + 兜底收割）
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

    // 停止所有 watcher
    if (m_ringFd >= 0 && m_ringWatcherActive) ev_io_stop(m_loop, &m_ringWatcher);
    m_ringWatcherActive = false;
    ev_prepare_stop(m_loop, &m_prepare);
    ev_check_stop(m_loop, &m_check);

    // 取消所有 fd — Destroy 中可以安全调 cancel(), 因为不会再 Submit 新 op
    for (auto& [fd, sp] : m_fds)
    {
        asio::error_code ec;
        sp->cancelled = true;
        sp->sock.cancel(ec);    // Destroy 中不存在竞态: 此后再无 Submit 调用
        (void)sp->sock.release();
    }
    m_fds.clear();

    // 先注销注册池（bufReg 析构调 unregister_buffers），再拆 io_context
    m_bufReg.reset();
    m_slotMem.reset();
    m_freeSlots.clear();
    m_fixedBufEnabled = false;

    // 释放 work guard → poll 清空队列 → stop
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

bool AsioUringIoBackend::SubmitRead(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq)
{
    if (!buf || !m_callback) {
        diag_log("[IODIAG AsioUring SubmitRead fd=%d FAIL buf=%p callback=%p\n", fd, (void*)buf.get(), (void*)m_callback);
        return false;
    }
    auto& sp = EnsureFdState(fd);
    if (sp->readPending) {  // 同一 fd 已有未完成读，跳过（防重入）
        diag_log("[IODIAG AsioUring SubmitRead fd=%d SKIP (readPending)\n", fd);
        return true;
    }
    sp->readPending = true;

    // 确保 CBuffer 有接收空间（8KB 预分配，够 HTTP 头/小包）
    buf->EnsureWritableBytes(8192);
    char*  dst = const_cast<char*>(buf->GetRawWriteBuffer());
    size_t cap = buf->WriteableBytes();

    diag_log("[IODIAG AsioUring SubmitRead fd=%d seq=%u buf_cap=%zu\n", fd, seq, cap);

    // completion lambda 持有 weak_ptr — CancelFd erase 后回调自动丢弃
    std::weak_ptr<FdState> wp = sp;
    sp->sock.async_read_some(
        asio::buffer(dst, cap),
        [this, wp, fd, seq, buf](const asio::error_code& ec, std::size_t n)
        {
            auto live = wp.lock();
            // live 为空或已 cancelled → fd 已被 CancelFd，丢弃本次回调
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
            buf->AdvanceWriteIndex(static_cast<int>(n));  // CBuffer 写指针推进
            m_callback(fd, seq, IoOp::Read, static_cast<int>(n), m_userData);
        });
    // ⚠️ 不在此处调 poll()/io_uring_enter — SQE 由三路-第 1 路 OnPrepare 批量提交
    // 此处只构造 SQE（写入内存 SQ），零 syscall。批量提交将 N 次 syscall 压为 1 次。
    UpdateRingWatcher();  // 有挂起读 → 确保三路-第 2 路 ring_fd 监听已启动
    return true;
}

bool AsioUringIoBackend::SubmitWrite(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq)
{
    if (!buf || !m_callback) return false;
    int readable = static_cast<int>(buf->ReadableBytes());
    if (readable <= 0)
    {
        m_callback(fd, seq, IoOp::Write, 0, m_userData);  // 空写, 直接回调完成
        return true;
    }
    auto& sp = EnsureFdState(fd);
    if (sp->writePending) return true;  // 同一 fd 已有未完成写, 跳过
    sp->writePending = true;

    const char* src = buf->GetRawReadBuffer();

    // 写路径分两路:
    //   [Fixed Buf 路] 池启用 + 有空槽 + payload≤槽 → memcpy 到注册内存, prep_write_fixed
    //   [普通路]      回退 — async_write_some(CBuffer 原始指针), 零额外拷贝
    int slot = -1;
    if (m_fixedBufEnabled && static_cast<size_t>(readable) <= m_slotSize)
        slot = BorrowSlot();

    std::weak_ptr<FdState> wp = sp;

    if (slot >= 0)
    {
        // Fixed Buf 路径: 拷贝到注册缓冲后 async_write_some(registered buffer)。
        // 内核通过 buffer index 直接 DMA，减少一次内核→用户态拷贝。
        std::memcpy(m_slotMem.get() + static_cast<size_t>(slot) * m_slotSize,
                    src, static_cast<size_t>(readable));
        sp->sock.async_write_some(
            asio::buffer((*m_bufReg)[static_cast<std::size_t>(slot)],
                         static_cast<std::size_t>(readable)),
            [this, wp, fd, seq, buf, slot](const asio::error_code& ec, std::size_t n)
            {
                // 普通写: CQE 到达 = 内核已消费缓冲，可立即归还 slot。
                // send_zc 后改为 NOTIF CQE 才归还（缓冲仍在内核待确认）。
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
        // 普通路径: 直接用 CBuffer 原始指针（无注册，零额外拷贝）。
        // 适合小包或 Fixed Buf 未启用/池满/超长场景。
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
    // ⚠️ 同 SubmitRead: 不在此处提交 — SQE 由三路-第 1 路 OnPrepare 批量提交
    UpdateRingWatcher();  // 有挂起写 → 确保三路-第 2 路 ring_fd 监听已启动
    return true;
}

void AsioUringIoBackend::CancelFd(int fd)
{
    auto it = m_fds.find(fd);
    if (it == m_fds.end()) return;
    it->second->cancelled = true;

    // 不调 sock.cancel() — 原因:
    //   sock.cancel() 提交 IORING_OP_ASYNC_CANCEL，该 op 与 io_uring 中的其他 SQE
    //   异步执行。若紧接 SubmitRead/SubmitWrite 复用同一 fd，新 op 可能在 cancel
    //   生效后被误取消。改用 cancelled 标志 + release() 让已入队 op 完成后丢弃回调。
    (void)it->second->sock.release();  // 归还 fd 所有权给调用方（不 close）
    m_fds.erase(it);                   // erase → shared_ptr 析构（最后一个 lambda 释放后）
    UpdateRingWatcher();               // fd 移除后检查是否还需 ring_fd 监听
}

bool AsioUringIoBackend::HasPending(int fd) const
{
    auto it = m_fds.find(fd);
    return it != m_fds.end() && (it->second->readPending || it->second->writePending);
}

// ========== 三路-第 1 路: ev_prepare ==========
// 时机: libev 每次 epoll_wait 之前
// 投递: SubmitRead/Write 攒下的 async ops → 一次 io_uring_enter 批量提交到内核 SQ
// 收割: poll() 同时收割已到 CQE → lambda → m_callback (提前处理，减少延迟)
// 核心价值: N 个 I/O 操作 → 1 次 syscall (vs ev 的 N 次 read/write)
void AsioUringIoBackend::OnPrepare(struct ev_loop*, ev_prepare* w, int)
{
    auto* be = static_cast<AsioUringIoBackend*>(w->data);
    auto n = be->m_ioCtx.poll();  // poll() 内部: 提交 SQ → 收割 CQE → 触发 completion lambda
    if (n > 0) {
        diag_log("[IODIAG AsioUring OnPrepare poll=%zu\n", n);
        ++g_stats.prepare_real;
    }
}

// ========== 三路-第 3 路: ev_check ==========
// 时机: libev 每次 epoll_wait 之后
// 补刀: poll() 收割 OnRingReady 与 epoll_wait 之间 race window 中新到的 CQE
//       内核可能在 epoll_wait 返回与 OnRingReady 执行之间刚好写入 CQE，
//       若不在 check 中补刀收割，这些 CQE 会延迟到下一轮事件循环
// 善后: UpdateRingWatcher (无挂起 op 则停 ring_fd 监听) + 每秒诊断统计
void AsioUringIoBackend::OnCheck(struct ev_loop*, ev_check* w, int)
{
    auto* be = static_cast<AsioUringIoBackend*>(w->data);
    auto n = be->m_ioCtx.poll();
    if (n > 0) {
        diag_log("[IODIAG AsioUring OnCheck poll=%zu\n", n);
        ++g_stats.check_real;
    }
    be->UpdateRingWatcher();  // 收割完后，若无挂起 op 则停 ring_fd 监听
    g_stats.tick();           // 每秒一次诊断输出
}

// ========== 三路-第 2 路: ev_io(ring_fd) ==========
// 时机: ring_fd 可读（内核有 CQE 完成通知）
// 接货: poll() 批量收割 CQE → ASIO 触发 completion lambda → m_callback 分发到业务层
//      一次唤醒可能排空多个 CQE（epoll 边沿触发，poll() 排空整批）
// 启停: 由 UpdateRingWatcher 按需管理 — 有挂起 op(readPending||writePending) 才 start
void AsioUringIoBackend::OnRingReady(struct ev_loop*, ev_io* w, int)
{
    auto* be = static_cast<AsioUringIoBackend*>(w->data);
    auto n = be->m_ioCtx.poll();
    ++g_stats.ring_ready;
    if (n == 0) ++g_stats.ring_empty;  // 空唤醒: poll() 只处理了 NOP/中断 CQE
    else        ++g_stats.ring_real;   // 真实完成: 有用户 op 的 CQE 被收割
    diag_log("[IODIAG AsioUring OnRingReady ring_fd=%d poll=%zu\n", be->m_ringFd, n);
}

// ========== 连接生命周期: 内核 socket API 包装 (ev/uring 共用) ==========

int AsioUringIoBackend::CreateListenSocket(const char* ip, uint16_t port, bool boReusePort, int backlog)
{
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return -1;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) { ::close(fd); return -1; }
#ifdef SO_REUSEPORT
    if (boReusePort && ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) != 0) { ::close(fd); return -1; }
#endif
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) { ::close(fd); return -1; }
    if (::listen(fd, backlog) < 0) { ::close(fd); return -1; }
    return fd;
}

int AsioUringIoBackend::Accept(int listenFd, PeerAddr& outPeerAddr)
{
    sockaddr_in stClientAddr;
    socklen_t clientAddrSize = sizeof(stClientAddr);
    int clientFd = ::accept(listenFd, reinterpret_cast<sockaddr*>(&stClientAddr), &clientAddrSize);
    if (clientFd < 0) return -1;

    inet_ntop(AF_INET, &stClientAddr.sin_addr, outPeerAddr.ip, sizeof(outPeerAddr.ip));
    outPeerAddr.port = ntohs(stClientAddr.sin_port);
    SetSocketOpt(clientFd);
    return clientFd;
}

void AsioUringIoBackend::CloseFd(int fd)
{
    if (fd >= 0) ::close(fd);
}

void AsioUringIoBackend::SetSocketOpt(int fd)
{
    if (fd < 0) return;
    int opt = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
}

bool AsioUringIoBackend::GetPeerName(int fd, PeerAddr& outAddr)
{
    sockaddr_in addr;
    socklen_t addrLen = sizeof(addr);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) return false;
    if (inet_ntop(AF_INET, &addr.sin_addr, outAddr.ip, sizeof(outAddr.ip)) == nullptr) return false;
    outAddr.port = ntohs(addr.sin_port);
    return true;
}

} /* namespace net */

#endif /* THUNDER_IO_ASIO_URING */
