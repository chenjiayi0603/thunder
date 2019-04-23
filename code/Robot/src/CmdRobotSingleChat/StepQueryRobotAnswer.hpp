/*******************************************************************************
 * Project:  RobotServer
 * @file     StepQueryRobotAnswer.hpp
 * @brief 
 * @author   cjy
 * @date:    2015年10月21日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMDROBOTSINGLECHAT_STEPQUERYROBOTANSWER_HPP_
#define SRC_CMDROBOTSINGLECHAT_STEPQUERYROBOTANSWER_HPP_
#include <time.h>
#include <sys/time.h>
#include "common.pb.h"
#include "user_basic.pb.h"
#include "robot_session.pb.h"
#include "util/json/CJsonObject.hpp"
#include "storage/MemOperator.hpp"
#include "storage/dataproxy.pb.h"
#include "RobotError.h"
#include "RobotErrorMapping.h"
#include "RobotTableName.h"
#include "RobotRedisProto.h"
#include "cmd/Cmd.hpp"
#include "step/Step.hpp"
#include "../RobotSession.h"
#include "../SessionAiEngine.hpp"

namespace robot
{
//答案模板
//1:普通答案 2:引导问题答案 3:复合问题答案
enum PreQuestionAnswerTemplate
{
    ePreQuestionAnswerTemplate_normal = 1,
    ePreQuestionAnswerTemplate_guide = 2,
    ePreQuestionAnswerTemplate_complex = 3,
};

enum eStepQueryRobotAnswer_Stage
{
    eStepQueryRobotAnswer_Start = 0,
    eStepQueryRobotAnswer_Inquery_Pre_question = 1,
    eStepQueryRobotAnswer_Inquery_Pre_question_ok = 2,

    eStepQueryRobotAnswer_Session_Ai_Engine_question = 3,
    eStepQueryRobotAnswer_Session_Ai_Engine_question_ok = 4,

    eStepQueryRobotAnswer_Sphinx_Engine_question = 5,
    eStepQueryRobotAnswer_Sphinx_Engine_question_ok = 6,
    eStepQueryRobotAnswer_Inquery_Engine_question = 7,
    eStepQueryRobotAnswer_Inquery_Engine_question_ok = 8,
};


class StepQueryRobotAnswer: public oss::Step
{
public:
    StepQueryRobotAnswer(
                    const oss::tagMsgShell& stMsgShell,
                                    const MsgHead& oInMsgHead,
                                    const robot_session::robot_single_msg_req &oRobotSingleMsg,
                                    const user_basic &basicInfo,
                                    RobotSession* pRobotSession,std::string &strFilteredQuestion);
    virtual ~StepQueryRobotAnswer();
    virtual oss::E_CMD_STATUS Emit(int iErrno=0, const std::string& strErrMsg = "", const std::string& strErrShow = "");
    oss::E_CMD_STATUS GetPreQuestionFromRedis();
    oss::E_CMD_STATUS GetPreQuestion();
    oss::E_CMD_STATUS GetSessionAiEngineQuestion();
    oss::E_CMD_STATUS GetSphinxEngineAnswerId();
    oss::E_CMD_STATUS GetAiAnswer();

    virtual oss::E_CMD_STATUS Callback(
                    const oss::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody,
                    void* data = NULL);
    virtual oss::E_CMD_STATUS Timeout();
private:
    void Response(int iErrno);
    void Response(int iErrno,robot_session::robot_single_msg_ack &oRsp);
    void DefaultResponse();
    void BuildAnswer(std::string &answer,const std::vector<ai_engine_question> &aiQuestionVec);
    void MatchQuestionLog(const std::vector<ai_engine_question> &aiQuestionVec);
    void StartClock()
    {
        gettimeofday(&m_cBeginClock,NULL);
    }
    void InqueryPrequestionClock()
    {
        gettimeofday(&m_cInqueryPrequestionClock,NULL);
        float useTime=1000000*(m_cInqueryPrequestionClock.tv_sec-m_cBeginClock.tv_sec)+
                        m_cInqueryPrequestionClock.tv_usec-m_cBeginClock.tv_usec;
        useTime/=1000;
        LOG4CPLUS_TRACE_FMT(GetLogger(),"%s() InqueryPrequestionClock use time(%lf) ms",__FUNCTION__,useTime);
    }
    void RobotAnswerEnginequestionClock()
    {
        gettimeofday(&m_cRobotAnswerEnginequestionClock,NULL);
        float useTime=1000000*(m_cRobotAnswerEnginequestionClock.tv_sec-m_cInqueryPrequestionClock.tv_sec)+
                        m_cRobotAnswerEnginequestionClock.tv_usec-m_cInqueryPrequestionClock.tv_usec;
        useTime/=1000;
        LOG4CPLUS_TRACE_FMT(GetLogger(),"%s() AnswerEnginequestion use time(%lf) ms",__FUNCTION__,useTime);
    }
    void EndClock()
    {
        gettimeofday(&m_cEndClock,NULL);
        float useTime=1000000*(m_cEndClock.tv_sec-m_cBeginClock.tv_sec)+
                        m_cEndClock.tv_usec-m_cBeginClock.tv_usec;
        useTime/=1000;
        LOG4CPLUS_TRACE_FMT(GetLogger(),"%s() Step StepQueryRobotAnswer use time(%lf) ms",__FUNCTION__,useTime);
    }
    oss::tagMsgShell m_stReqMsgShell;
    MsgHead m_oReqMsgHead;
    robot_session::robot_single_msg_req m_oRobotSingleMsg;
    std::string m_strFilteredQuestion;

    user_basic m_obasicInfo;
    RobotSession* m_pRobotSession;
    eStepQueryRobotAnswer_Stage m_stage;
    uint64 m_nIndexid;
    std::string m_strAnswer;

    oss::uint32 m_uiTimeOut;

    timeval m_cBeginClock;
    timeval m_cInqueryPrequestionClock;
    timeval m_cRobotAnswerEnginequestionClock;
    timeval m_cEndClock;
};

}/* namespace robot */

#endif /* SRC_CMDFROMCLIENT_StepQueryRobotAnswer_HPP_ */
