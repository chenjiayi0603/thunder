/*******************************************************************************
 * Project:  Net
 * @file     AsioUringIoBackend.hpp
 * @brief    standalone Asio io_uring backend（单线程，主循环直驱）
 * @note     io_context 跑在 libev 主循环线程，由 ev_prepare/ev_check/ev_io(ring_fd)
 *           三路驱动 io_context.poll()，彻底消除线程跳开销。
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

    bool Init(struct ev_loop* loop, IoCompletionCallback callback, void* user_data) override;
    void Destroy() override;
    bool SubmitRead(int fd, util::CBuffer* buf, uint32_t seq) override;
    bool SubmitWrite(int fd, util::CBuffer* buf, uint32_t seq) override;
    void CancelFd(int fd) override;
    const char* Name() const override { return "asio_uring"; }
    bool HasPending(int fd) const override;

private:
    // 仅在 libev 主线程访问（单线程，无锁）
    struct FdState
    {
        asio::posix::stream_descriptor sock;
        bool readPending  = false;
        bool writePending = false;
        bool cancelled    = false;
        explicit FdState(asio::io_context& io, int fd) : sock(io, fd) {}
    };

    std::shared_ptr<FdState>& EnsureFdState(int fd);

    // libev 回调（全部在主线程，直接 poll io_context）
    static void OnPrepare(struct ev_loop*, ev_prepare* w, int);
    static void OnCheck(struct ev_loop*, ev_check* w, int);
    static void OnRingReady(struct ev_loop*, ev_io* w, int);

    static int FindIoUringRingFd();
    void UpdateRingWatcher();  // 5.A: 按需启停 ring_fd 监听

    // A1: 有界注册缓冲池（send_zc 地基）。仅写路径使用，env 门控默认关。
    // 池满/超长/未启用 → 回退现有非注册 async_write_some 路径（即今天的行为）。
    bool InitFixedBuffers();
    int  BorrowSlot();              // 返回 slot 下标，无可用返回 -1
    void ReturnSlot(int idx);       // 归还 slot（写完成时调用；send_zc 接入后改为 NOTIF 后调用）

    asio::io_context m_ioCtx{1};
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> m_workGuard;

    // A1 注册池
    bool   m_fixedBufEnabled = false;
    size_t m_slotSize  = 65536;     // 单槽字节，THUNDER_ASIO_URING_FIXEDBUF_SLOTSIZE
    size_t m_slotCount = 256;       // 槽数，THUNDER_ASIO_URING_FIXEDBUF_SLOTS（默认 256×64KB=16MB ≤ memlock）
    std::unique_ptr<char[]> m_slotMem;
    std::unique_ptr<asio::buffer_registration<std::vector<asio::mutable_buffer>>> m_bufReg;
    std::vector<int> m_freeSlots;

    // 仅主线程访问
    std::unordered_map<int, std::shared_ptr<FdState>> m_fds;

    struct ev_loop* m_loop      = nullptr;
    IoCompletionCallback m_callback = nullptr;
    void*           m_userData  = nullptr;

    ev_prepare m_prepare{};
    ev_check   m_check{};
    ev_io      m_ringWatcher{};
    int        m_ringFd = -1;
    bool       m_ringWatcherActive = false;  // 5.A: ring_fd 按需启停状态
};

} /* namespace net */

#endif /* THUNDER_IO_ASIO_URING */
#endif /* ASIOURINGIOBACKEND_HPP_ */
