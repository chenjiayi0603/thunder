/*******************************************************************************
 * Project:  CenterServer
 * @file     CmdUpdateServerConfig.hpp
 * @brief   更新节点配置
 * @author  chenjiayi
 * @date    2016年8月9日
 * @note    更新节点配置
 * Modify history:
 ******************************************************************************/
#ifndef CMD_UPDATE_SERVER_CONFIG_HPP_
#define CMD_UPDATE_SERVER_CONFIG_HPP_
#include "protocol/oss_sys.pb.h"
#include "server.pb.h"
#include "user_basic.pb.h"
#include "cmd/Cmd.hpp"
#include "../Comm.hpp"
#include "../NodeSession.h"

namespace starshiplib
{
/**
 * @brief   更新节点配置
 * @author  chenjiayi
 * @date    2016年8月9日
 * @note    更新节点配置
 */
class CmdUpdateServerConfig: public oss::Cmd
{
public:
    CmdUpdateServerConfig();
    virtual ~CmdUpdateServerConfig();
    virtual bool Init();
    virtual bool AnyMessage(const oss::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead, const MsgBody& oInMsgBody);
private:
    int CheckReqMsg(const MsgBody& oInMsgBody);
    bool parseMsg(const MsgBody& oInMsgBody,const server::user_basic &basicInfo);
    //更新节点配置应答
    bool Response(int iErrno);
    NodeSession* pSess;
    bool boInit;
    oss::tagMsgShell m_stMsgShell;
    MsgHead m_oInMsgHead;
    server::update_server_config_req m_oUpdateServerConfigReq;
    server::update_server_config_ack m_oUpdateServerConfigAck;
};
} /* namespace starshiplib */

#endif /* CMDTOLDWORKER_HPP_ */
