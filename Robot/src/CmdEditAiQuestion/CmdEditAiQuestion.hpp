/*******************************************************************************
 * Project:  RobotServer
 * @file     CmdEditAiQuestion.hpp
 * @brief    编辑ai问题
 * @author   cjy
 * @date:    2017年1月19日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMD_EDIT_AI_QUESTION_CMD_EDIT_AI_QUESTION_HPP_
#define SRC_CMD_EDIT_AI_QUESTION_CMD_EDIT_AI_QUESTION_HPP_
#include "cmd/Cmd.hpp"
#include "common.pb.h"
#include "user_basic.pb.h"
#include "user.pb.h"
#include "robot_knowledge.pb.h"
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

//更新类型 1：增加 2:删除
enum eEditAiQuestion_Oper
{
    eEditAiQuestion_Add = 1,
    eEditAiQuestion_Del = 2,
};

class CmdEditAiQuestion: public oss::Cmd
{
public:
    CmdEditAiQuestion();
    virtual ~CmdEditAiQuestion();
    bool Init();
    virtual bool AnyMessage(
                    const oss::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
protected:
    bool parseMsg(const MsgBody& oInMsgBody,const user_basic &basicInfo);
public:
    oss::tagMsgShell m_stMsgShell;
    MsgHead m_oInMsgHead;
    robot_knowledge::edit_ai_question_req m_oEditAiQuestionReq;
    SessionAiEngine* m_pSessionAiEngine;
};

} /* namespace robot */

#endif /* SRC_CMD_EDIT_AI_QUESTION_CMD_EDIT_AI_QUESTION_HPP_ */
