/*******************************************************************************
 * Project:  CenterServer
 * @file     CmdOfflineNode.hpp
 * @brief   更新节点配置
 * @author  chenjiayi
 * @date    2016年8月9日
 * @note    更新节点配置
 * Modify history:
 ******************************************************************************/
#ifndef CMD_OFFLINE_NODE_HPP_
#define CMD_OFFLINE_NODE_HPP_
#include "protocol/oss_sys.pb.h"
#include "server.pb.h"
#include "user_basic.pb.h"
#include "cmd/Cmd.hpp"
#include "../Comm.hpp"
#include "../NodeSession.h"

// "offline":0//挂起节点路由:0，关闭节点:1
enum offlineFlag
{
    eofflineFlag_suspend_routes = 0,
    eofflineFlag_close_note     = 1,
};

namespace starshiplib
{
/**
 * @brief   下线节点
 * @author  chenjiayi
 * @date    2016年8月9日
 * @note   下线节点
 */
class CmdOfflineNode: public oss::Cmd
{
public:
    CmdOfflineNode();
    virtual ~CmdOfflineNode();
    virtual bool Init();
    virtual bool AnyMessage(const oss::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead, const MsgBody& oInMsgBody);
private:
    int SendOfflineToTarget(const std::string& sOfflineNodeIdentify);
    bool parseMsg(const MsgBody& oInMsgBody,const server::user_basic &basicInfo);
    bool Response(int iErrno);
    NodeSession* pSess;
    bool boInit;
    oss::tagMsgShell m_stMsgShell;
    MsgHead m_oInMsgHead;
    server::offline_node_req m_oOfflineNodeReq;
    server::offline_node_ack m_oOfflineNodeAck;
};
} /* namespace starshiplib */

#endif /* CMDTOLDWORKER_HPP_ */
