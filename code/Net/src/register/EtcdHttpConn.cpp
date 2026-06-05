/**
 * @file  EtcdHttpConn.cpp
 * @brief 自管异步 HTTP 短请求连接实现（issus #24）
 */
#include "register/EtcdHttpConn.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include <log4cplus/loggingmacros.h>

namespace net
{

EtcdHttpConn::EtcdHttpConn(struct ev_loop* loop, std::string host, int port,
                           const log4cplus::Logger& logger)
    : m_loop(loop)
    , m_host(std::move(host))
    , m_port(port)
    , m_logger(logger)
    , m_urlBase("http://" + m_host + ":" + std::to_string(port))
    , m_codec(util::CODEC_HTTP)
{
}

EtcdHttpConn::~EtcdHttpConn()
{
    Reset();
}

void EtcdHttpConn::Post(const std::string& path, const std::string& body, RespCallback cb)
{
    m_queue.push_back(Pending{path, body, std::move(cb)});
    if (m_fd < 0)
    {
        if (!Connect())
        {
            // 连接发起失败：立即回调所有请求 ok=false，交由上层(定时器)稍后重试
            FailAll("connect 发起失败 host=" + m_host);
        }
        return;  // 连接完成后由 OnWritable → Pump 发送
    }
    // 已有连接：连上了就泵下一个；还在连接中则等 OnWritable
    if (m_connected && !m_inflight)
    {
        EncodeFrontRequest();
    }
}

void EtcdHttpConn::Close()
{
    // #25: 排队请求静默丢弃 — Close() 时通知所有在途 callback 失败
    std::deque<Pending> q; q.swap(m_queue);
    for (auto& p : q) { if (p.cb) p.cb(false, 0, ""); }
    m_inflight = false;
    Reset();
}

bool EtcdHttpConn::Connect()
{
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res    = nullptr;
    const std::string portS = std::to_string(m_port);
    if (getaddrinfo(m_host.c_str(), portS.c_str(), &hints, &res) != 0 || res == nullptr)
    {
        LOG4CPLUS_WARN(m_logger, "EtcdHttpConn — getaddrinfo 失败 host=" << m_host);
        return false;
    }

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0)
    {
        freeaddrinfo(res);
        return false;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    const int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno != EINPROGRESS)
    {
        LOG4CPLUS_WARN(m_logger, "EtcdHttpConn — connect 失败 " << strerror(errno));
        ::close(fd);
        return false;
    }

    m_fd        = fd;
    m_connected = false;
    ev_io_init(&m_rio, ReadCb, fd, EV_READ);
    ev_io_init(&m_wio, WriteCb, fd, EV_WRITE);
    m_rio.data = this;
    m_wio.data = this;
    ev_io_start(m_loop, &m_rio);
    m_rioStarted = true;
    ev_io_start(m_loop, &m_wio);  // 写就绪 = connect 完成(或失败)
    m_wioStarted = true;
    return true;
}

/*static*/ void EtcdHttpConn::ReadCb(struct ev_loop*, ev_io* w, int)
{
    static_cast<EtcdHttpConn*>(w->data)->OnReadable();
}

/*static*/ void EtcdHttpConn::WriteCb(struct ev_loop*, ev_io* w, int)
{
    static_cast<EtcdHttpConn*>(w->data)->OnWritable();
}

void EtcdHttpConn::OnWritable()
{
    if (!m_connected)
    {
        int       err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0)
        {
            FailAll(std::string("connect 完成检测失败: ") + strerror(err ? err : errno));
            return;
        }
        m_connected = true;
        LOG4CPLUS_DEBUG(m_logger, "EtcdHttpConn — 已连接 " << m_urlBase);
        // 连上后：发起队首请求
        if (!m_inflight && !m_queue.empty())
        {
            EncodeFrontRequest();
            return;  // EncodeFrontRequest 内部已 FlushSend
        }
    }
    FlushSend();
}

void EtcdHttpConn::EncodeFrontRequest()
{
    Pending& p = m_queue.front();

    HttpMsg req;
    req.set_type(HTTP_REQUEST);
    req.set_method(HTTP_POST);
    req.set_http_major(1);
    req.set_http_minor(1);
    req.set_url(m_urlBase + p.path);
    req.set_body(p.body);
    (*req.mutable_headers())["Content-Type"] = "application/json";

    if (m_codec.Encode(req, &m_send) != CODEC_STATUS_OK)
    {
        LOG4CPLUS_WARN(m_logger, "EtcdHttpConn — Encode 失败 path=" << p.path);
        DeliverFront(false, 0, "");
        return;
    }
    m_inflight = true;
    if (!m_wioStarted)
    {
        ev_io_start(m_loop, &m_wio);
        m_wioStarted = true;
    }
    FlushSend();
}

void EtcdHttpConn::FlushSend()
{
    while (m_send.ReadableBytes() > 0)
    {
        const ssize_t n = ::send(m_fd, m_send.GetRawReadBuffer(), m_send.ReadableBytes(), MSG_NOSIGNAL);
        if (n > 0)
        {
            m_send.AdvanceReadIndex(static_cast<int>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            if (!m_wioStarted)
            {
                ev_io_start(m_loop, &m_wio);
                m_wioStarted = true;
            }
            return;  // 等下次可写
        }
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        FailAll(std::string("send 错误: ") + strerror(errno));
        return;
    }
    // 全部发完：停写 watcher（保留读 watcher 等响应）
    if (m_wioStarted)
    {
        ev_io_stop(m_loop, &m_wio);
        m_wioStarted = false;
    }
}

void EtcdHttpConn::OnReadable()
{
    char buf[8192];
    while (true)
    {
        const ssize_t n = ::recv(m_fd, buf, sizeof(buf), 0);
        if (n > 0)
        {
            m_recv.Write(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0)
        {
            FailAll("对端关闭连接");
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        FailAll(std::string("recv 错误: ") + strerror(errno));
        return;
    }

    // 解出尽可能多的完整响应（短请求串行，通常一次一个）
    while (m_inflight && m_recv.ReadableBytes() > 0)
    {
        HttpMsg              resp;
        const E_CODEC_STATUS st = m_codec.Decode(&m_recv, resp);
        if (st == CODEC_STATUS_OK)
        {
            DeliverFront(true, resp.status_code(), resp.body());
        }
        else if (st == CODEC_STATUS_PAUSE)
        {
            break;  // 数据不完整，等更多
        }
        else
        {
            FailAll("HTTP 响应解码错误");
            return;
        }
    }
}

void EtcdHttpConn::DeliverFront(bool ok, int status, const std::string& body)
{
    if (m_queue.empty()) return;
    Pending p = std::move(m_queue.front());
    m_queue.pop_front();
    m_inflight = false;
    if (p.cb) p.cb(ok, status, body);  // 回调可能 Post 新请求(链式状态机)
    // 泵下一个（若回调已触发发送则 m_inflight=true，这里跳过）
    if (m_connected && !m_inflight && !m_queue.empty())
    {
        EncodeFrontRequest();
    }
}

void EtcdHttpConn::FailAll(const std::string& why)
{
    LOG4CPLUS_WARN(m_logger, "EtcdHttpConn — " << why << " (" << m_urlBase << ")");
    Reset();
    std::deque<Pending> q;
    q.swap(m_queue);
    for (auto& p : q)
    {
        if (p.cb) p.cb(false, 0, "");
    }
}

void EtcdHttpConn::Reset()
{
    if (m_rioStarted)
    {
        ev_io_stop(m_loop, &m_rio);
        m_rioStarted = false;
    }
    if (m_wioStarted)
    {
        ev_io_stop(m_loop, &m_wio);
        m_wioStarted = false;
    }
    if (m_fd >= 0)
    {
        ::close(m_fd);
        m_fd = -1;
    }
    m_connected = false;
    m_inflight  = false;
    if (m_recv.ReadableBytes() > 0) m_recv.SkipBytes(m_recv.ReadableBytes());
    if (m_send.ReadableBytes() > 0) m_send.SkipBytes(m_send.ReadableBytes());
}

}  // namespace net
