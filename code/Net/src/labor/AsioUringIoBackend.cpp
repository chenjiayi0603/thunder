/*******************************************************************************
 * Project:  Net
 * @file     AsioUringIoBackend.cpp
 ******************************************************************************/
#ifdef THUNDER_IO_ASIO_URING

#include "AsioUringIoBackend.hpp"
#include "util/CBuffer.hpp"

namespace net
{

AsioUringIoBackend::AsioUringIoBackend() = default;

AsioUringIoBackend::~AsioUringIoBackend()
{
    Destroy();
}

bool AsioUringIoBackend::Init(struct ev_loop* loop, IoCompletionCallback callback, void* user_data)
{
    m_loop     = loop;
    m_callback = callback;
    m_userData = user_data;
    m_workGuard.emplace(m_ioCtx.get_executor());

    ev_async_init(&m_asyncWatcher, &OnAsyncWake);
    m_asyncWatcher.data = this;
    ev_async_start(loop, &m_asyncWatcher);
    m_asyncStarted = true;

    m_ioThread = std::thread([this]() { IoThreadFunc(); });
    return true;
}

void AsioUringIoBackend::Destroy()
{
    if (m_asyncStarted && m_loop)
    {
        ev_async_stop(m_loop, &m_asyncWatcher);
        m_asyncStarted = false;
    }

    // 通知 io_context 线程取消所有挂起操作
    asio::post(m_ioCtx, [this]() {
        for (auto& kv : m_fds)
        {
            asio::error_code ec;
            kv.second->cancelled = true;
            kv.second->sock.cancel(ec);
            (void)kv.second->sock.release();
        }
        m_fds.clear();
    });

    // 允许 io_context.run() 自然退出
    m_workGuard.reset();
    if (m_ioThread.joinable())
    {
        m_ioThread.join();
    }
    m_ioCtx.stop();

    m_loop     = nullptr;
    m_callback = nullptr;
    m_userData = nullptr;
}

void AsioUringIoBackend::IoThreadFunc()
{
    m_ioCtx.run();
}

std::shared_ptr<AsioUringIoBackend::FdState>& AsioUringIoBackend::EnsureFdState(int fd)
{
    auto it = m_fds.find(fd);
    if (it != m_fds.end())
    {
        return it->second;
    }
    auto sp = std::make_shared<FdState>(m_ioCtx, fd);
    auto [ins, _] = m_fds.emplace(fd, std::move(sp));
    return ins->second;
}

void AsioUringIoBackend::PushCompletion(int fd, uint32_t seq, IoOp op, int result)
{
    {
        std::lock_guard<std::mutex> lk(m_queueMtx);
        m_pendingQueue.push_back({fd, seq, op, result});
    }
    // CAS：仅当尚无待处理信号时才发送一次 ev_async_send，避免每个完成事件都写 eventfd
    bool expected = false;
    if (m_loop && m_asyncPending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        ev_async_send(m_loop, &m_asyncWatcher);
    }
}

bool AsioUringIoBackend::SubmitRead(int fd, util::CBuffer* buf, uint32_t seq)
{
    if (!buf || !m_callback)
    {
        return false;
    }
    buf->EnsureWritableBytes(8192);
    char*  dst = const_cast<char*>(buf->GetRawWriteBuffer());
    size_t cap = buf->WriteableBytes();

    asio::post(m_ioCtx, [this, fd, buf, seq, dst, cap]() {
        auto& sp = EnsureFdState(fd);
        if (sp->readPending)
        {
            return;
        }
        sp->readPending = true;

        std::weak_ptr<FdState> wp = sp;
        sp->sock.async_read_some(
            asio::buffer(dst, cap),
            [this, wp, fd, seq, buf](const asio::error_code& ec, std::size_t n)
            {
                auto live = wp.lock();
                if (!live || live->cancelled)
                {
                    return;
                }
                live->readPending = false;
                if (ec)
                {
                    if (ec == asio::error::operation_aborted)
                    {
                        return;
                    }
                    PushCompletion(fd, seq, IoOp::Read, -ec.value());
                    return;
                }
                if (n == 0)
                {
                    PushCompletion(fd, seq, IoOp::Read, 0);
                    return;
                }
                buf->AdvanceWriteIndex(static_cast<int>(n));
                PushCompletion(fd, seq, IoOp::Read, static_cast<int>(n));
            });
    });
    return true;
}

bool AsioUringIoBackend::SubmitWrite(int fd, util::CBuffer* buf, uint32_t seq)
{
    if (!buf || !m_callback)
    {
        return false;
    }
    int readable = static_cast<int>(buf->ReadableBytes());
    if (readable <= 0)
    {
        m_callback(fd, seq, IoOp::Write, 0, m_userData);
        return true;
    }

    const char* src = buf->GetRawReadBuffer();

    asio::post(m_ioCtx, [this, fd, buf, seq, src, readable]() {
        auto& sp = EnsureFdState(fd);
        if (sp->writePending)
        {
            return;
        }
        sp->writePending = true;

        std::weak_ptr<FdState> wp = sp;
        sp->sock.async_write_some(
            asio::buffer(src, readable),
            [this, wp, fd, seq, buf](const asio::error_code& ec, std::size_t n)
            {
                auto live = wp.lock();
                if (!live || live->cancelled)
                {
                    return;
                }
                live->writePending = false;
                if (ec)
                {
                    if (ec == asio::error::operation_aborted)
                    {
                        return;
                    }
                    PushCompletion(fd, seq, IoOp::Write, -ec.value());
                    return;
                }
                buf->AdvanceReadIndex(static_cast<int>(n));
                PushCompletion(fd, seq, IoOp::Write, static_cast<int>(n));
            });
    });
    return true;
}

void AsioUringIoBackend::CancelFd(int fd)
{
    asio::post(m_ioCtx, [this, fd]() {
        auto it = m_fds.find(fd);
        if (it == m_fds.end())
        {
            return;
        }
        asio::error_code ec;
        it->second->cancelled = true;
        it->second->sock.cancel(ec);
        (void)it->second->sock.release();
        m_fds.erase(it);
    });
}

bool AsioUringIoBackend::HasPending(int fd) const
{
    // io_context 线程独占 m_fds；SubmitRead/Write 内有 readPending/writePending 幂等保护。
    // 返回 false 让调用方总是尝试重新提交，内部保证幂等。
    (void)fd;
    return false;
}

void AsioUringIoBackend::OnAsyncWake(struct ev_loop* /*loop*/, ev_async* w, int /*revents*/)
{
    auto* self = static_cast<AsioUringIoBackend*>(w->data);

    // 先清标记再取队列：确保 PushCompletion 在 swap 之后加入的事件能再次触发信号
    self->m_asyncPending.store(false, std::memory_order_release);

    std::vector<CompletionEvent> batch;
    {
        std::lock_guard<std::mutex> lk(self->m_queueMtx);
        batch.swap(self->m_pendingQueue);
    }

    for (auto& evt : batch)
    {
        self->m_callback(evt.fd, evt.seq, evt.op, evt.result, self->m_userData);
    }
}

} /* namespace net */

#endif /* THUNDER_IO_ASIO_URING */
