/*******************************************************************************
 * Project:  CenterServer
 * @file     CmdCheckServerLoad.hpp
 * @brief   更新节点配置
 * @author  chenjiayi
 * @date    2016年8月9日
 * @note    更新节点配置
 * Modify history:
 ******************************************************************************/
#ifndef CMD_CHECK_SERVER_CONFIG_HPP_
#define CMD_CHECK_SERVER_CONFIG_HPP_
#include "protocol/oss_sys.pb.h"
#include "server.pb.h"
#include "cmd/Cmd.hpp"
#include "../Comm.hpp"
#include "../NodeSession.h"

namespace core
{
/**
 * @brief   更新节点配置
 * @author  chenjiayi
 * @date    2016年8月9日
 * @note    更新节点配置
 */
class CmdCheckServerLoad: public net::Cmd
{
public:
    CmdCheckServerLoad();
    virtual ~CmdCheckServerLoad();
    virtual bool Init();
    virtual bool AnyMessage(const net::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead, const MsgBody& oInMsgBody);
private:
    bool parseMsg(const MsgBody& oInMsgBody,const server::user_basic &basicInfo);
    bool Response(int iErrno);
    NodeSession* pSess;
    bool boInit;
    net::tagMsgShell m_stMsgShell;
    MsgHead m_oInMsgHead;
    server::check_server_load_req m_oCheckServerLoadReq;
    server::check_server_load_ack m_oCheckServerLoadAck;
};
} /* namespace core */

#endif /* CMD_CHECK_SERVER_CONFIG_HPP_ */
