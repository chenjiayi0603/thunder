/*******************************************************************************
 * Project:  Net
 * @file     AsioUringIoBackend.hpp
 * @brief    standalone Asio io_uring backend (internal)
 * @note     io_context 跑在独立线程；完成事件通过 ev_async 回调 marshal 到 libev
 *           主循环，彻底消除 1ms 轮询抖动。
 ******************************************************************************/
#ifndef ASIOURINGIOBACKEND_HPP_
#define ASIOURINGIOBACKEND_HPP_

#ifdef THUNDER_IO_ASIO_URING

#include "labor/IoBackend.hpp"
#include "libev/ev.h"
#include <asio.hpp>
#include <asio/posix/stream_descriptor.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
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
    // 仅在 io_context 线程访问
    struct FdState
    {
        asio::posix::stream_descriptor sock;
        bool readPending  = false;
        bool writePending = false;
        bool cancelled    = false;
        explicit FdState(asio::io_context& io, int fd) : sock(io, fd) {}
    };

    struct CompletionEvent
    {
        int      fd;
        uint32_t seq;
        IoOp     op;
        int      result;
    };

    // 仅在 io_context 线程调用
    std::shared_ptr<FdState>& EnsureFdState(int fd);
    void PushCompletion(int fd, uint32_t seq, IoOp op, int result);

    // ev_async 回调（libev 主循环线程）
    static void OnAsyncWake(struct ev_loop* loop, ev_async* w, int revents);

    // io_context 线程入口
    void IoThreadFunc();

    asio::io_context m_ioCtx{1};
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> m_workGuard;
    std::thread      m_ioThread;

    // 仅 io_context 线程访问
    std::unordered_map<int, std::shared_ptr<FdState>> m_fds;

    // 跨线程完成队列
    mutable std::mutex           m_queueMtx;
    std::vector<CompletionEvent> m_pendingQueue;
    // 用 atomic 标记去重 ev_async_send：队列从空→非空只触发一次 eventfd write
    std::atomic<bool>            m_asyncPending{false};

    struct ev_loop*      m_loop      = nullptr;
    IoCompletionCallback m_callback  = nullptr;
    void*                m_userData  = nullptr;

    ev_async m_asyncWatcher{};
    bool     m_asyncStarted = false;
};

} /* namespace net */

#endif /* THUNDER_IO_ASIO_URING */
#endif /* ASIOURINGIOBACKEND_HPP_ */
