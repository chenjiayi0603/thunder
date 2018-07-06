/*******************************************************************************
 * Project:  CenterServer
 * @file     CmdOnlineNode.hpp
 * @brief   更新节点配置
 * @author  chenjiayi
 * @date    2016年8月9日
 * @note    更新节点配置
 * Modify history:
 ******************************************************************************/
#ifndef CMD_ONLINE_NODE_HPP_
#define CMD_ONLINE_NODE_HPP_
#include "protocol/oss_sys.pb.h"
#include "server.pb.h"
#include "user_basic.pb.h"
#include "cmd/Cmd.hpp"
#include "../Comm.hpp"
#include "../NodeSession.h"

namespace starshiplib
{

//"online":0//恢复节点路由:0,重启工作者:1
enum oOnlineFlag
{
    eOnlineFlag_restore_routes      = 0,
    eOnlineFlag_restart_workers     = 1,
};

/**
 * @brief   节点上线
 * @author  chenjiayi
 * @date    2016年8月9日
 * @note    节点上线
 */
class CmdOnlineNode: public oss::Cmd
{
public:
    CmdOnlineNode();
    virtual ~CmdOnlineNode();
    virtual bool Init();
    virtual bool AnyMessage(const oss::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead, const MsgBody& oInMsgBody);
private:
    int SendRestartWorkersToTarget(const std::string& sOnlineNodeIdentify);
    bool parseMsg(const MsgBody& oInMsgBody,const server::user_basic &basicInfo);
    bool Response(int iErrno);
    NodeSession* pSess;
    bool boInit;
    oss::tagMsgShell m_stMsgShell;
    MsgHead m_oInMsgHead;
    server::online_node_req m_oOnlineNodeReq;
    server::online_node_ack m_oOnlineNodeAck;
};
} /* namespace starshiplib */

#endif /* CMDTOLDWORKER_HPP_ */
