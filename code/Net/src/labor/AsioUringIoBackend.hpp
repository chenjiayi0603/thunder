/*******************************************************************************
 * Project:  Net
 * @file     AsioUringIoBackend.hpp
 * @brief    基于 standalone ASIO 的 io_uring 后端 — 单线程 libev 主循环直驱，小代码量替代 epoll
 *
 * === 核心流程: 三路驱动 SQE 提交 + CQE 收割／分发 ===
 *
 * 无后台线程 — ASIO io_uring_service 嵌入 libev 主循环，三个 watcher 协同:
 *
 *   ev_prepare (每次 epoll_wait 之前)
 *     │  调用 io_context.poll() 将 SubmitRead/SubmitWrite 攒下的 async_read_some/
 *     │  async_write_some 批量转化为 SQE 一次性提交到内核 SQ（io_uring_enter）。
 *     │  同时收割目前已到的 CQE，触发 ASIO 内部 reactive 层调用 completion lambda，
 *     │  lambda 中: buf->AdvanceWriteIndex/AdvanceReadIndex → m_callback(fd,seq,IoOp,ret)。
 *     │
 *     ▼
 *   epoll_wait (libev 阻塞，等待 fd / timer / signal)
 *     │  ring_fd 可读 → io_uring 新 CQE 到达（epoll 边沿触发，一次唤醒排空整批）
 *     │  业务 fd 就绪 → accept/connect 事件（由 EvIoBackend/NativeUringIoBackend 处理）
 *     │  timer 超时 → Step 超时、心跳等
 *     ▼
 *   ev_io(ring_fd) → OnRingReady
 *     │  epoll 通知 ring_fd 可读 → poll() 收割 CQE（可能存在多个完成的 op）。
 *     │  ASIO lambda 回调分发到 m_callback → Worker::OnIoComplete 处理 codec 状态机。
 *     │  按需启停: 有挂起 op(readPending||writePending) 才 start，无则 stop，
 *     │    阻止 ASIO NOP-SQE 产生的伪 CQE 在 idle 时唤醒 epoll 造成忙循环。
 *     ▼
 *   ev_check (每次 epoll_wait 之后)
 *     │  再次 poll() — 收割 OnRingReady 与本次 epoll_wait 之间可能到达的 CQE。
 *     │  UpdateRingWatcher: 收割完后若无挂起 op 则 stop ring_fd 监听。
 *     │  每秒输出诊断统计（THUNDER_ASIO_URING_DIAG=1 时，包括空唤醒率）。
 *     ▼
 *   invoke_pending — 所有排队的回调在此统一执行（IO / timer / signal 的 pending cb）
 *
 * SQE 与 CQE 时机:
 *   SubmitRead/SubmitWrite 只构造 ASIO async_op（内部生成 SQE），不立即提交。
 *   SQE 实际入队 SQ 的时机是 OnPrepare 和 OnRingReady 中的 poll()。
 *   CQE 收割后 ASIO 同步调用 completion lambda，不跨线程，无需队列转交。
 *
 * === 关键设计 ===
 *
 * FdState shared_ptr + completion lambda weak_ptr:
 *   CancelFd 只需 erase map entry，最后一个 lambda 释放 shared_ptr 后自动析构。
 *   无需显式等待所有异步 op 完成，避免复杂的生命周期同步。
 *
 * CancelFd 不调 sock.cancel():
 *   sock.cancel() 提交 IORING_OP_ASYNC_CANCEL，若后续紧接 SubmitRead/SubmitWrite
 *   可能意外取消新注册的 op。改为 release() + erase + cancelled 标志丢弃回调。
 *
 * RingWatcher 按需启停:
 *   ASIO waitable reactor 通过 NOP-SQE 注册 ring_fd 可读性。即使无用户 op，
 *   ring_fd 也可能因内核中断 CQE 变为可读 → 触发 ev_io → poll()=0 空转。
 *   解决: 无挂起 op 时 stop ev_io(ring_fd)，epoll_wait 不再被 ring_fd 唤醒。
 *
 * Fixed Buffers (env: THUNDER_ASIO_URING_FIXEDBUF=1):
 *   send_zc 地基。注册缓冲池按需启用，池满/超长自动回退非注册路径。
 *
 * 编译: cmake -DENABLE_ASIO_URING=ON  (定义 THUNDER_IO_ASIO_URING)
 * 运行: Hello.json "io_backend": "asio_uring"
 * 诊断: THUNDER_ASIO_URING_DIAG=1 → /tmp/asio_uring_diag.log
 *
 * @warning  若 Init 失败，调用方应降级到 EvIoBackend——本类不自行降级。
 * @see      docs/uring/uring计划优化路线.md（选型决策）, IoBackend, NativeUringIoBackend
 ******************************************************************************/
#ifndef ASIOURINGIOBACKEND_HPP_
#define ASIOURINGIOBACKEND_HPP_

#ifdef THUNDER_IO_ASIO_URING

#include "labor/IoBackend.hpp"
#include "libev/ev.h"
#include <asio.hpp>
#include <asio/posix/stream_descriptor.hpp>
#include <asio/buffer_registration.hpp>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace util { class CBuffer; }

namespace net
{

class AsioUringIoBackend : public IoBackend
{
public:
    AsioUringIoBackend();
    ~AsioUringIoBackend() override;

    // 生命周期
    bool Init(struct ev_loop* loop, IoCompletionCallback callback, void* user_data) override;
    void Destroy() override;
    const char* Name() const override { return "asio_uring"; }

    // 监听 socket
    int  CreateListenSocket(const char* ip, uint16_t port, bool boReusePort, int backlog) override;
    int  Accept(int listenFd, PeerAddr& outPeerAddr) override;

    // I/O 提交
    bool SubmitRead(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq) override;
    bool SubmitWrite(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq) override;

    // 连接关闭
    void CancelFd(int fd) override;
    void CloseFd(int fd) override;
    bool HasPending(int fd) const override;

    // Socket 属性 / 对端地址
    void SetSocketOpt(int fd) override;
    bool GetPeerName(int fd, PeerAddr& outAddr) override;

private:
    /// completion lambda 持有 weak_ptr<FdState>，CancelFd erase 后回调自动丢弃
    struct FdState
    {
        asio::posix::stream_descriptor sock;
        bool readPending  = false;
        bool writePending = false;
        bool cancelled    = false;
        explicit FdState(asio::io_context& io, int fd) : sock(io, fd) {}
    };

    std::shared_ptr<FdState>& EnsureFdState(int fd);

    // ---- libev 三路驱动 ----
    static void OnPrepare(struct ev_loop*, ev_prepare* w, int);   ///< epoll_wait 前: poll() 批量提交 SQE + 收割 CQE
    static void OnCheck(struct ev_loop*, ev_check* w, int);       ///< epoll_wait 后: 再次收割 + UpdateRingWatcher + 每秒诊断
    static void OnRingReady(struct ev_loop*, ev_io* w, int);      ///< ring_fd 可读 → poll() 收割 CQE

    static int FindIoUringRingFd();                               ///< 扫描 /proc/self/fd 找 [io_uring] anon_inode

    /// 按需启停 ring_fd 监听: 有挂起 op→start，无→stop，消除 NOP-SQE 空唤醒
    void UpdateRingWatcher();

    // ---- Fixed Buffers (env: THUNDER_ASIO_URING_FIXEDBUF=1, send_zc 地基) ----
    bool InitFixedBuffers();
    int  BorrowSlot();              ///< 返回 slot 下标，无可用返回 -1
    void ReturnSlot(int idx);       ///< 归还 slot（普通写 CQE 时；send_zc 后改为 NOTIF 时）

    asio::io_context m_ioCtx{1};
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> m_workGuard;

    bool   m_fixedBufEnabled = false;
    size_t m_slotSize  = 65536;
    size_t m_slotCount = 256;
    std::unique_ptr<char[]> m_slotMem;
    std::unique_ptr<asio::buffer_registration<std::vector<asio::mutable_buffer>>> m_bufReg;
    std::vector<int> m_freeSlots;

    std::unordered_map<int, std::shared_ptr<FdState>> m_fds;

    struct ev_loop* m_loop      = nullptr;
    IoCompletionCallback m_callback = nullptr;
    void*           m_userData  = nullptr;

    ev_prepare m_prepare{};
    ev_check   m_check{};
    ev_io      m_ringWatcher{};
    int        m_ringFd = -1;
    bool       m_ringWatcherActive = false;
};

} /* namespace net */

#endif /* THUNDER_IO_ASIO_URING */
#endif /* ASIOURINGIOBACKEND_HPP_ */
