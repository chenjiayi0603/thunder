/**
 * @file  EtcdWatcher.hpp
 * @brief etcd watch 长连接(issus #24 Phase C) — 替代 libcurl 线程 + ev_async
 *
 * 在 Manager libev 主循环上自管一条到 etcd 的 TCP 连接, 发送 POST /v3/watch,
 * 读完 HTTP 响应头验证 200 + chunked, 之后按 chunked 编码收流、每个 JSON 行
 * 回调给 EtcdCenterConnector。连接断开自动重连。
 * 无独立线程、无 ev_async——事件直接在主循环回调。
 */
#ifndef ETCD_WATCHER_HPP_
#define ETCD_WATCHER_HPP_

#include <ev.h>
#include <cstdint>
#include <functional>
#include <string>
#include <log4cplus/logger.h>
#include "util/CBuffer.hpp"

namespace net
{

class EtcdWatcher
{
public:
    using LineCb = std::function<void(const std::string& line)>;
    EtcdWatcher(struct ev_loop* loop, std::string host, int port, std::string prefix,
                const log4cplus::Logger& logger, LineCb onLine);
    ~EtcdWatcher();
    EtcdWatcher(const EtcdWatcher&)            = delete;
    EtcdWatcher& operator=(const EtcdWatcher&) = delete;

    void Start(int64_t lastRevision);
    void Stop();

private:
    void Connect();
    void OnWritable();
    void OnReadable();
    void ParseBufferedData();
    void ParseHeaderLine(const std::string& line);
    void SwitchToChunks();
    void ParseChunks();
    void FeedLine(const std::string& line);
    void Reconnect();

    static void ReadCb(struct ev_loop*, ev_io*, int);
    static void WriteCb(struct ev_loop*, ev_io*, int);
    static void TimerCb(struct ev_loop*, ev_timer*, int);

    struct ev_loop* m_loop;
    std::string     m_host; int m_port;
    std::string     m_prefix, m_rangeEnd;
    log4cplus::Logger m_logger;
    LineCb          m_onLine;
    int             m_fd = -1;
    int             m_statusCode = 0;
    bool            m_isChunked = false;
    int64_t         m_lastRevision = 0;
    bool            m_stopping = false;
    enum class State { Idle, Connecting, ReadingHeader, Streaming };
    State           m_state = State::Idle;
    ev_io           m_rio{}, m_wio{};
    ev_timer        m_reconnectTimer{};
    bool            m_rioStarted = false, m_wioStarted = false, m_timerStarted = false;
    util::CBuffer   m_recv, m_send;
    std::string     m_headerBuf;
    enum class ChState { SizeLine, Data, End };
    ChState         m_chState = ChState::SizeLine;
    int64_t         m_chunkRemaining = 0;
    std::string     m_chunkBuf;
};

}  // namespace net
#endif
