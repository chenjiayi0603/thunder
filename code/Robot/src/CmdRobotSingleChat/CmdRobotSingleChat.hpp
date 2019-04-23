/*******************************************************************************
 * Project:  LogicServer
 * @file     CmdRobotSingleChat.hpp
 * @brief    用户信息更新
 * @author   cjy
 * @date:    2017年1月19日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_USER_CMDUSERQUERY_CMDUSERUPDATE_HPP_
#define SRC_USER_CMDUSERQUERY_CMDUSERUPDATE_HPP_
#include "cmd/Cmd.hpp"
#include "common.pb.h"
#include "user_basic.pb.h"
#include "user.pb.h"
#include "robot_session.pb.h"
#include "RobotError.h"
#include "StepQueryRobotAnswer.hpp"
#include "../RobotSession.h"
#include "../SessionAiEngine.hpp"

#ifdef __cplusplus
extern "C" {
#endif
net::Cmd* create();
#ifdef __cplusplus
}
#endif

namespace robot
{

class CmdRobotSingleChat: public net::Cmd
{
public:
    CmdRobotSingleChat();
    virtual ~CmdRobotSingleChat();
    bool Init();
    virtual bool AnyMessage(
                    const net::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
protected:
    bool parseMsg(const MsgBody& oInMsgBody);
    void Response(int iErrno);
    void Response(int iErrno,robot_session::robot_single_msg_ack &oRsp);
public:
    bool m_boInit;
    RobotSession* m_pRobotSession;
    net::tagMsgShell m_stMsgShell;
    MsgHead m_oInMsgHead;
    robot_session::robot_single_msg_req m_oRobotSingleMsgReq;
    StepQueryRobotAnswer *pStepQueryRobotAnswer;
};

} /* namespace robot */

#endif /* SRC_USER_CMDUSERQUERY_CMDUSERUPDATE_HPP_ */
