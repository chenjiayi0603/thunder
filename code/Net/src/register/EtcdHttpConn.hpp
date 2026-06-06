/**
 * @file     EtcdHttpConn.hpp
 * @brief    自管的异步 HTTP 短请求连接（issus #24）
 *
 * EtcdCenterConnector 运行在 Manager 进程,Manager 没有 Worker 的 HttpStep/SentTo 栈,
 * 其读分派(ProcessMessages)又硬编码二进制 S2S 协议、不可挂 codec。因此这里在 Manager
 * 的 libev 主循环上**自管一条到 etcd 的 TCP 连接**,收发用框架 `HttpCodec` 编解码,
 * 完全替代原 libcurl + 独立线程方案,无独立线程、无 ev_async 跨线程投递。
 *
 * 本类只负责"短请求-响应"(lease/keepalive/register/txn/range):串行,一次一个在途请求,
 * 复用同一 keep-alive 连接。长连接 watch 的 chunked 流式读由另一条连接处理(Phase C)。
 */
#ifndef ETCD_HTTP_CONN_HPP_
#define ETCD_HTTP_CONN_HPP_

#include <ev.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>

#include <log4cplus/logger.h>

#include "codec/HttpCodec.hpp"
#include "util/CBuffer.hpp"

namespace net
{

class EtcdHttpConn
{
public:
    /// 响应回调:(ok=是否拿到完整响应, http 状态码, 响应体)
    using RespCallback = std::function<void(bool ok, int status, const std::string& body)>;

    EtcdHttpConn(struct ev_loop* loop, std::string host, int port,
                 const log4cplus::Logger& logger);
    ~EtcdHttpConn();
    EtcdHttpConn(const EtcdHttpConn&)            = delete;
    EtcdHttpConn& operator=(const EtcdHttpConn&) = delete;

    /// 发一个 POST 短请求(path 如 "/v3/lease/grant")。串行:有在途请求则排队。
    void Post(const std::string& path, const std::string& body, RespCallback cb);

    /// 主动关闭连接(下次请求会重连)。
    void Close();

private:
    struct Pending
    {
        std::string  path;
        std::string  body;
        RespCallback cb;
    };

    bool Connect();                 ///< 非阻塞 connect,注册读写 watcher
    void OnWritable();              ///< 连接完成 / 继续发送
    void OnReadable();              ///< 读响应 → HttpCodec 解码 → 回调
    void FlushSend();               ///< 把 m_send 写入 socket
    void EncodeFrontRequest();      ///< 队首请求编码进 m_send 并启用写
    void DeliverFront(bool ok, int status, const std::string& body);  ///< 完成队首并出队
    void FailAll(const std::string& why);  ///< 连接错误:回调所有在途/排队请求 ok=false
    void Reset();                   ///< 关 fd、停 watcher、清状态(保留队列)

    static void ReadCb(struct ev_loop* loop, ev_io* w, int revents);
    static void WriteCb(struct ev_loop* loop, ev_io* w, int revents);

    struct ev_loop*     m_loop;
    std::string         m_host;
    int                 m_port;
    log4cplus::Logger   m_logger;
    std::string         m_urlBase;     ///< "http://host:port"

    int                 m_fd          = -1;
    bool                m_connected   = false;   ///< TCP 已建立
    bool                m_inflight    = false;   ///< 队首已发出、等响应
    ev_io               m_rio{};
    ev_io               m_wio{};
    bool                m_rioStarted  = false;
    bool                m_wioStarted  = false;

    HttpCodec           m_codec;
    util::CBuffer       m_recv;
    util::CBuffer       m_send;
    std::deque<Pending> m_queue;
};

}  // namespace net

#endif /* ETCD_HTTP_CONN_HPP_ */
