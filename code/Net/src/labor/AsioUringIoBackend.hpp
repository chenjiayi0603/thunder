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
#include <memory>
#include <optional>
#include <unordered_map>

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

    asio::io_context m_ioCtx{1};
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> m_workGuard;

    // 仅主线程访问
    std::unordered_map<int, std::shared_ptr<FdState>> m_fds;

    struct ev_loop* m_loop      = nullptr;
    IoCompletionCallback m_callback = nullptr;
    void*           m_userData  = nullptr;

    ev_prepare m_prepare{};
    ev_check   m_check{};
    ev_io      m_ringWatcher{};
    int        m_ringFd = -1;
};

} /* namespace net */

#endif /* THUNDER_IO_ASIO_URING */
#endif /* ASIOURINGIOBACKEND_HPP_ */
