#include "EtcdWatcher.hpp"

#include "util/encrypt/base64.h"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <log4cplus/loggingmacros.h>

namespace net
{
using NetB64 = std::string(*)(const std::string&);
extern std::string util_base64_encode(const std::string&);

EtcdWatcher::EtcdWatcher(struct ev_loop* loop, std::string host, int port,
                         std::string prefix, const log4cplus::Logger& logger,
                         LineCb onLine)
    : m_loop(loop), m_host(std::move(host)), m_port(port),
      m_prefix(std::move(prefix)), m_logger(logger), m_onLine(std::move(onLine))
{
    m_rangeEnd = m_prefix;
    if (!m_rangeEnd.empty())
        m_rangeEnd.back() = static_cast<char>(static_cast<unsigned char>(m_rangeEnd.back()) + 1);
}

EtcdWatcher::~EtcdWatcher() { Stop(); }

void EtcdWatcher::Start(int64_t rev)
{
    m_lastRevision = rev; m_stopping = false;
    // #28: 缓存 base64, 避免每次重连重复计算
    if (m_b64Prefix.empty())
    {
        auto b64 = [](const std::string& s) {
            int len = Base64encode_len(static_cast<int>(s.size()));
            std::vector<char> buf(len);
            Base64encode(buf.data(), s.data(), static_cast<int>(s.size()));
            return std::string(buf.data(), len - 1);  // strip \0
        };
        m_b64Prefix   = b64(m_prefix);
        m_b64RangeEnd = b64(m_rangeEnd);
    }
    Connect();
}

void EtcdWatcher::Stop()
{
    m_stopping = true;
    if (m_timerStarted) { ev_timer_stop(m_loop, &m_reconnectTimer); m_timerStarted = false; }
    if (m_rioStarted)   { ev_io_stop(m_loop, &m_rio); m_rioStarted = false; }
    if (m_wioStarted)   { ev_io_stop(m_loop, &m_wio); m_wioStarted = false; }
    if (m_fd >= 0)      { ::close(m_fd); m_fd = -1; }
    m_state = State::Idle;
    m_headerBuf.clear(); m_chunkBuf.clear();
    if (m_recv.ReadableBytes() > 0) m_recv.SkipBytes(m_recv.ReadableBytes());
}

void EtcdWatcher::Connect()
{
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(static_cast<uint16_t>(m_port));
    inet_pton(AF_INET, m_host.c_str(), &addr.sin_addr);
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { Reconnect(); return; }
    int flags = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int rc = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) { ::close(fd); Reconnect(); return; }
    m_fd = fd; m_state = State::Connecting;
    ev_io_init(&m_rio, ReadCb, fd, EV_READ); ev_io_init(&m_wio, WriteCb, fd, EV_WRITE);
    m_rio.data = this; m_wio.data = this;
    ev_io_start(m_loop, &m_rio); m_rioStarted = true;
    ev_io_start(m_loop, &m_wio); m_wioStarted = true;
}

void EtcdWatcher::OnWritable()
{
    if (m_state == State::Connecting)
    {
        int err = 0; socklen_t len = sizeof(err);
        if (getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0)
        { Reconnect(); return; }
        // Build watch request (base64 cached in m_b64Prefix/m_b64RangeEnd)
        const std::string& bPrefix = m_b64Prefix;
        const std::string& bRange  = m_b64RangeEnd;
        std::ostringstream body;
        body << "{\"create_request\":{\"key\":\"" << bPrefix << "\",\"range_end\":\"" << bRange
             << "\",\"start_revision\":" << (m_lastRevision + 1) << "}}";
        std::string b = body.str();
        std::ostringstream req;
        req << "POST /v3/watch HTTP/1.1\r\nHost: " << m_host << ":" << m_port
            << "\r\nContent-Type: application/json\r\nContent-Length: " << b.size()
            << "\r\nConnection: keep-alive\r\n\r\n" << b;
        std::string r = req.str();
        m_send.Write(r.data(), r.size());
        m_state = State::ReadingHeader;
    }
    // Flush
    while (m_send.ReadableBytes() > 0)
    {
        ssize_t n = ::send(m_fd, m_send.GetRawReadBuffer(), m_send.ReadableBytes(), MSG_NOSIGNAL);
        if (n > 0) { m_send.AdvanceReadIndex(static_cast<int>(n)); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0 && errno == EINTR) continue;
        Reconnect(); return;
    }
    if (m_send.ReadableBytes() == 0 && m_wioStarted) { ev_io_stop(m_loop, &m_wio); m_wioStarted = false; }
}

void EtcdWatcher::OnReadable()
{
    char buf[8192];
    while (true)
    {
        ssize_t n = ::recv(m_fd, buf, sizeof(buf), 0);
        if (n > 0) { m_recv.Write(buf, static_cast<size_t>(n)); continue; }
        if (n == 0) { Reconnect(); return; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        Reconnect(); return;
    }
    ParseBufferedData();
}

void EtcdWatcher::ParseBufferedData()
{
    if (m_state == State::ReadingHeader)
    {
        while (m_recv.ReadableBytes() > 0)
        {
            char ch; if (!m_recv.ReadByte(ch)) break;
            m_headerBuf += ch;
            if (m_headerBuf.size() > 8192) { Reconnect(); return; }
            if (m_headerBuf.size() >= 4 && m_headerBuf.substr(m_headerBuf.size()-4) == "\r\n\r\n")
            {
                std::istringstream hs(m_headerBuf);
                std::string line;
                if (!std::getline(hs, line) || line.find("200") == std::string::npos) { Reconnect(); return; }
                while (std::getline(hs, line))
                {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty()) continue;
                    std::string kl = line; for (auto& c : kl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (kl.find("transfer-encoding:") == 0 && kl.find("chunked") != std::string::npos) m_isChunked = true;
                }
                if (!m_isChunked) { Reconnect(); return; }
                m_state = State::Streaming; m_chState = ChState::SizeLine; m_headerBuf.clear();
                m_chunkRemaining = 0; m_chunkBuf.clear();
                ParseBufferedData();
                return;
            }
        }
    }
    else if (m_state == State::Streaming) ParseChunks();
}

void EtcdWatcher::ParseChunks()
{
    while (m_recv.ReadableBytes() > 0)
    {
        char ch; if (!m_recv.ReadByte(ch)) break;
        if (m_chState == ChState::SizeLine)
        {
            if (ch == '\n' && !m_chunkBuf.empty() && m_chunkBuf.back() == '\r')
            {
                m_chunkBuf.pop_back();
                // #26: hex 尺寸行无长度上限 → Reconnect 防止 OOM
                if (m_chunkBuf.size() > 1024) { Reconnect(); return; }
                m_chunkRemaining = std::strtoll(m_chunkBuf.c_str(), nullptr, 16);
                m_chunkBuf.clear();
                if (m_chunkRemaining == 0) { Reconnect(); return; }
                m_chState = ChState::Data;
            }
            else m_chunkBuf += ch;
        }
        else
        {
            if (m_chunkRemaining > 0)
            {
                m_chunkBuf += ch;
                --m_chunkRemaining;
            }
            else if (m_chunkRemaining == 0 && ch == '\r') { --m_chunkRemaining; }
            else if (m_chunkRemaining == -1 && ch == '\n')
            {
                // chunk data complete, emit accumulated content as lines
                std::istringstream cs(m_chunkBuf);
                std::string line;
                while (std::getline(cs, line))
                {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!line.empty() && line[0] == '{' && m_onLine) m_onLine(line);
                }
                m_chunkBuf.clear(); m_chunkRemaining = 0; m_chState = ChState::SizeLine;
            }
        }
    }
}

void EtcdWatcher::Reconnect()
{
    Stop();
    if (m_stopping) return;
    ev_timer_init(&m_reconnectTimer, TimerCb, 1.0, 0.0);
    m_reconnectTimer.data = this;
    ev_timer_start(m_loop, &m_reconnectTimer);
    m_timerStarted = true;
}

void EtcdWatcher::TimerCb(struct ev_loop*, ev_timer* w, int)
{ auto* s = static_cast<EtcdWatcher*>(w->data); s->m_timerStarted = false; if (!s->m_stopping) s->Start(s->m_lastRevision); }
void EtcdWatcher::ReadCb(struct ev_loop*, ev_io* w, int) { static_cast<EtcdWatcher*>(w->data)->OnReadable(); }
void EtcdWatcher::WriteCb(struct ev_loop*, ev_io* w, int) { static_cast<EtcdWatcher*>(w->data)->OnWritable(); }

}  // namespace net
