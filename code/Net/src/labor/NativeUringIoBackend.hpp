/*******************************************************************************
 * Project:  Net
 * @file     NativeUringIoBackend.hpp
 * @brief    原生 liburing I/O 后端（单线程，libev 主循环驱动）
 * @note     Path B：自管 SQ/CQ，eventfd + ev_io 收割 CQE，ev_check 兜底。
 *           不 thread-per-op（io_engine_uring.cpp 反面教材）。SQPOLL env 门控。
 *           安全模型对齐 AsioUringIoBackend：PendingOp 由本后端持有，
 *           CancelFd 后陈旧 CQE 只释放 PendingOp、绝不触碰已释放的连接缓冲。
 *           骨架仅普通 recv/send；send_zc 见 Path B-3。
 ******************************************************************************/
#ifndef NATIVEURINGIOBACKEND_HPP_
#define NATIVEURINGIOBACKEND_HPP_

#include "labor/IoBackend.hpp"
#include "libev/ev.h"
#include <liburing.h>
#include <cstdint>
#include <unordered_map>

namespace util { class CBuffer; }

namespace net
{

class NativeUringIoBackend : public IoBackend
{
public:
    NativeUringIoBackend();
    ~NativeUringIoBackend() override;

    bool Init(struct ev_loop* loop, IoCompletionCallback callback, void* user_data) override;
    void Destroy() override;
    bool SubmitRead(int fd, util::CBuffer* buf, uint32_t seq) override;
    bool SubmitWrite(int fd, util::CBuffer* buf, uint32_t seq) override;
    void CancelFd(int fd) override;
    const char* Name() const override { return "native_uring"; }
    bool HasPending(int fd) const override;

private:
    // 仅 libev 主线程访问（单线程，无锁）
    struct FdState
    {
        uint32_t seq          = 0;
        int      readPending  = 0;
        int      writePending = 0;
        bool     cancelled    = false;
    };
    // 本后端 heap 持有，生命周期 = 直到其 CQE 被收割。绝不持有连接资源所有权。
    struct PendingOp
    {
        int                 fd;
        uint32_t            seq;
        IoOp                op;
        util::CBuffer*      buf;       // 仅当 fd/seq 仍有效且未取消时才解引用
        // send_zc：后端自有 bounce 缓冲，NOTIF 前内核 DMA 它，与连接 buffer
        // 生命周期解耦（DestroyConnect 同步释放连接 buffer 不会 UAF）。
        bool                isZc      = false;
        char*               zcBuf     = nullptr;  // bounce 缓冲，NOTIF 时 free
        int                 zcBytes   = 0;        // 结果 CQE 的字节数（NOTIF 时回传）
        bool                gotResult = false;
    };

    bool GetSqe(struct io_uring_sqe** out);   // 满则先 submit 再取
    void ReapCqes();
    static void OnEvfd(struct ev_loop*, ev_io* w, int);
    static void OnCheck(struct ev_loop*, ev_check* w, int);

    struct io_uring m_ring{};
    bool            m_ringInit = false;
    int             m_evfd     = -1;

    struct ev_loop*      m_loop     = nullptr;
    IoCompletionCallback m_callback = nullptr;
    void*                m_userData = nullptr;

    ev_io    m_evWatcher{};
    ev_check m_check{};
    bool     m_started = false;

    std::unordered_map<int, FdState> m_fds;

    unsigned m_sqDepth = 4096;
    // send_zc：>= 阈值且启用才走 prep_send_zc（小包零拷贝净亏，见 docs）
    bool   m_zcEnabled   = false;
    size_t m_zcThreshold = 16384;   // THUNDER_URING_ZC_THRESHOLD
};

} /* namespace net */

#endif /* NATIVEURINGIOBACKEND_HPP_ */
