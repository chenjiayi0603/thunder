/*******************************************************************************
 * Project:  Net
 * @file     EvIoBackend.hpp
 * @brief    libev-based I/O backend (internal)
 ******************************************************************************/
#ifndef EVIOBACKEND_HPP_
#define EVIOBACKEND_HPP_

#include "labor/IoBackend.hpp"
#include "libev/ev.h"
#include <unordered_map>
#include <memory>

namespace util { class CBuffer; }

namespace net
{

/**
 * @brief libev ev_io 包装后端
 * @note  每个 fd 一个 ev_io watcher；通过 event flags 控制读/写
 */
class EvIoBackend : public IoBackend
{
public:
    EvIoBackend() = default;
    ~EvIoBackend() override;

    // 生命周期
    bool Init(struct ev_loop* loop, IoCompletionCallback callback, void* user_data) override;
    void Destroy() override;
    const char* Name() const override { return "ev"; }

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
    struct WatcherData
    {
        int iFd = -1;
        uint32_t ulSeq = 0;
        util::CBuffer* pReadBuf = nullptr;
        util::CBuffer* pWriteBuf = nullptr;
        EvIoBackend* pBackend = nullptr;
        ev_io* pWatcher = nullptr;
    };

    static void IoEventCallback(struct ev_loop* loop, struct ev_io* watcher, int revents);

    struct ev_loop* m_loop = nullptr;
    IoCompletionCallback m_callback = nullptr;
    void* m_userData = nullptr;
    std::unordered_map<int, std::unique_ptr<WatcherData>> m_mapFdData;
};

} /* namespace net */

#endif /* EVIOBACKEND_HPP_ */
