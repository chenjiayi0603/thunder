/*******************************************************************************
 * Project:  Net
 * @file     UringIoBackend.hpp
 * @brief    io_uring-based I/O backend (internal)
 ******************************************************************************/
#ifndef URINGIOBACKEND_HPP_
#define URINGIOBACKEND_HPP_

#ifdef THUNDER_IO_URING

#include "labor/IoBackend.hpp"
#include "libev/ev.h"
#include <liburing.h>
#include <unordered_map>
#include <vector>
#include <memory>

namespace util { class CBuffer; }

namespace net
{

/**
 * @brief io_uring 异步 I/O 后端
 * @note  通过 ev_io 监听 io_uring fd 来集成到 libev 事件循环
 */
class UringIoBackend : public IoBackend
{
public:
    UringIoBackend() = default;
    ~UringIoBackend() override;

    bool Init(struct ev_loop* loop, IoCompletionCallback callback, void* user_data) override;
    void Destroy() override;
    bool SubmitRead(int fd, util::CBuffer* buf, uint32_t seq) override;
    bool SubmitWrite(int fd, util::CBuffer* buf, uint32_t seq) override;
    void CancelFd(int fd) override;
    const char* Name() const override { return "uring"; }
    bool HasPending(int fd) const override;

private:
    static constexpr unsigned kQueueDepth = 256;
    static constexpr unsigned kMaxCqeBatch = 32;

    struct PendingOp
    {
        int fd = -1;
        uint32_t seq = 0;
        IoOp op = IoOp::Read;
        util::CBuffer* buf = nullptr;
    };

    static void RingEventCallback(struct ev_loop* loop, struct ev_io* watcher, int revents);
    void ReapCqes();

    struct io_uring m_ring;
    bool m_ringInitialized = false;
    struct ev_loop* m_loop = nullptr;
    IoCompletionCallback m_callback = nullptr;
    void* m_userData = nullptr;
    ev_io* m_pRingWatcher = nullptr;
    std::unordered_map<uint64_t, PendingOp> m_mapPending;  // user_data -> PendingOp
    uint64_t m_nextUserData = 1;
};

} /* namespace net */

#endif /* THUNDER_IO_URING */
#endif /* URINGIOBACKEND_HPP_ */
