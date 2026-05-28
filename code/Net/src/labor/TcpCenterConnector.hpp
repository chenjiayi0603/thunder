/*******************************************************************************
 * Project:  Net
 * @file     TcpCenterConnector.hpp
 * @brief    CenterConnector 的 TCP + Protobuf 实现
 * @author   cjy
 * @date:    2026年5月24日
 * @note
 *   从 Manager.cpp 迁移 Center 通信逻辑到此独立插件。
 *   自行管理到 Center 的 TCP 连接（ev_io/ev_timer），
 *   复用现有的 MsgHead/MsgBody/NodeReport/NodeReportRsp 协议格式。
 *
 *   连接生命周期:
 *     1. Init() 时从配置解析 Center 地址列表
 *     2. ReportNodeStatus() → 对未连接的 Center 发起 connect
 *     3. connect 成功后发送 CMD_REQ_CONNECT_TO_WORKER 握手
 *     4. 收到 CMD_RSP_CONNECT_TO_WORKER 后发送 CMD_REQ_TELL_WORKER
 *     5. 握手完成后发送缓冲的注册/心跳数据
 *     6. 收到 Center 响应后解析并 emit CenterEvent
 ******************************************************************************/
#ifndef TCP_CENTER_CONNECTOR_HPP_
#define TCP_CENTER_CONNECTOR_HPP_

#include <memory>
#include <string>
#include <unordered_map>
#include "labor/CenterConnector.hpp"
#include "NetDefine.hpp"

struct ev_io;
struct ev_timer;
struct ev_loop;

namespace util { class CBuffer; class CJsonObject; }

namespace net
{

class TcpCenterConnector;  // 前向声明

/**
 * @brief Center TCP 连接内部属性
 * @note 包含回指 connector 的指针，供静态 IO 回调使用
 */
struct CenterConnectionAttr
{
    int          iFd              = -1;
    uint32_t     ulSeq            = 0;
    double       dActiveTime      = 0.0;
    std::unique_ptr<util::CBuffer> pRecvBuff;
    std::unique_ptr<util::CBuffer> pSendBuff;
    std::unique_ptr<util::CBuffer> pWaitForSendBuff;
    std::string  strIdentify;                    ///< "host:port.0"
    TcpCenterConnector* pConnector = nullptr;    ///< 回指指针

    // ev watchers（由 connector 管理生命周期）
    ev_io*       pIoWatcher       = nullptr;
    ev_timer*    pTimeoutWatcher  = nullptr;

    bool         boHandshakeDone  = false;       ///< CMD_REQ_TELL_WORKER 已发送
    bool         boConnecting     = false;       ///< connect 已发起但未完成
};

class TcpCenterConnector : public CenterConnector
{
public:
    explicit TcpCenterConnector(const util::CJsonObject& oCenterConf);
    ~TcpCenterConnector() override;

    // ---- CenterConnector 接口 ----
    bool        Init(struct ev_loop* loop, CenterEventCallback cb, void* user_data) override;
    void        Destroy() override;
    const char* Name() const override { return "tcp"; }

    bool        ReportNodeStatus(const std::string& node_report, bool is_register) override;

    bool        IsConnected() const override;
    size_t      CenterCount() const override;

    bool        TryConsumeMessage(int iFd, uint32_t ulSeq, const std::string& strIdentify,
                                  uint32_t cmd, uint32_t seq, const std::string& body) override;
    bool        IsCenterConnection(const std::string& strIdentify) const override;
    void        OnConnectionDestroy(const std::string& strIdentify, int iFd, uint32_t ulSeq) override;

    // ---- 供 Manager 设置本节点信息 ----
    void SetNodeInfo(const std::string& nodeType,
                     const std::string& hostForServer, int portForServer,
                     const std::string& hostForClient, int portForClient,
                     const std::string& gateway, int gatewayPort,
                     unsigned int workerNum);

private:
    friend struct CenterConnectionAttr;

    // ---- 内部连接管理 ----
    CenterConnectionAttr* CreateConnAttr(int iFd, uint32_t ulSeq);
    void DestroyConn(const std::string& strIdentify);
    void DestroyConnByIter(
        std::unordered_map<std::string, std::unique_ptr<CenterConnectionAttr>>::iterator iter);
    void ResetConnForReconnect(CenterConnectionAttr* pConn);

    bool AddIoRead(CenterConnectionAttr* pConn);
    bool AddIoWrite(CenterConnectionAttr* pConn);
    bool RemoveIoWrite(CenterConnectionAttr* pConn);
    bool AddIoTimeout(CenterConnectionAttr* pConn, double dTimeout);

    // ---- 发送 ----
    bool SendRegOrBeat(CenterConnectionAttr* pConn, uint32_t cmd, uint32_t seq, const std::string& body);
    bool FlushSendBuf(CenterConnectionAttr* pConn);
    bool AutoConnect(const std::string& strIdentify, uint32_t cmd, uint32_t seq,
                     const std::string& body);

    // ---- IO 回调（静态） ----
    static void IoCallback(struct ev_loop* loop, struct ev_io* watcher, int revents);
    static void IoTimerCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents);

    void OnIoRead(CenterConnectionAttr* pConn);
    void OnIoWrite(CenterConnectionAttr* pConn);
    void OnIoError(CenterConnectionAttr* pConn);
    void OnIoTimeout(CenterConnectionAttr* pConn);

    // ---- 协议处理 ----
    void RecvAndDispatch(CenterConnectionAttr* pConn);
    void OnHandshakeComplete(CenterConnectionAttr* pConn);

    // ---- 事件发射 ----
    void Emit(CenterEventType type, CenterEvent ev = CenterEvent{});

    // ---- 辅助 ----
    uint32_t GetSeq();
    bool     ResolveHostPort(const std::string& strHost, int iPort,
                             struct sockaddr& addr, int& iFd);
    double   Now() const;

    // ---- 配置 ----
    util::CJsonObject m_oCenterConf;

    // ---- 运行时 ----
    struct ev_loop*   m_loop       = nullptr;
    CenterEventCallback m_callback;
    void*             m_user_data  = nullptr;
    bool              m_bInited    = false;

    std::unordered_map<std::string, std::unique_ptr<CenterConnectionAttr>> m_mapCenterConn;
    std::string m_strRaftLeaderKey;

    uint32_t m_ulConnSeq = 0;

    // 本节点信息
    std::string  m_nodeType;
    std::string  m_hostForServer;
    int          m_portForServer = 0;
    std::string  m_hostForClient;
    int          m_portForClient = 0;
    std::string  m_gateway;
    int          m_gatewayPort   = 0;
    unsigned int m_workerNum     = 0;

    char m_errBuf[256] = {};
    static constexpr int    kErrBufLen   = 256;
    static constexpr double kIoKeepAlive = 300.0;
    static constexpr double kConnTimeout = 5.0;
};

} /* namespace net */

#endif /* TCP_CENTER_CONNECTOR_HPP_ */
