/*******************************************************************************
 * Project:  RobotServer
 * @file     CmdSortSessionMessagesLog.hpp
 * @brief    排序分类会话消息日志
 * @author   cjy
 * @date:    2017年1月19日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMD_SORT_SESSION_MESSAGES_LOG_HPP_
#define SRC_CMD_SORT_SESSION_MESSAGES_LOG_HPP_
#include "cmd/Cmd.hpp"
#include "common.pb.h"
#include "user_basic.pb.h"
#include "user.pb.h"
#include "behaviour_common.pb.h"
#include "RobotError.h"
#include "../RobotSession.h"

#ifdef __cplusplus
extern "C" {
#endif
oss::Cmd* create();
#ifdef __cplusplus
}
#endif

namespace robot
{

class CmdSortSessionMessagesLog: public oss::Cmd
{
public:
    CmdSortSessionMessagesLog();
    virtual ~CmdSortSessionMessagesLog();
    bool Init();
    virtual bool AnyMessage(
                    const oss::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
protected:
    bool parseMsg(const MsgBody& oInMsgBody,
                    behaviour_common::sort_tb_session_messages_log_list_req &oSortTbsessionMessagesLogListReq);
public:
    bool Response(int iErrno,
                    behaviour_common::sort_tb_session_messages_log_list_ack &oSortTbsessionMessagesLogListAck);
    oss::tagMsgShell m_stMsgShell;
    MsgHead m_oInMsgHead;
    std::vector<behaviour_common::tb_session_messages_log> m_messageLogs;
    SessionAiEngine* m_pSessionAiEngine;
};

} /* namespace robot */

#endif /* SRC_CMD_EDIT_AI_QUESTION_CMD_EDIT_AI_QUESTION_HPP_ */
