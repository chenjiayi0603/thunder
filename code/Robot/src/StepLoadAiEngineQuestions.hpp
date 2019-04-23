/*******************************************************************************
 * Project:  RobotServer
 * @file     StepLoadAiEngineQuestions.hpp
 * @brief 
 * @author   cjy
 * @date:    2015年12月12日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_StepAiEngineQuestions_HPP_
#define SRC_StepAiEngineQuestions_HPP_
#include "util/UnixTime.hpp"
#include "RobotError.h"
#include "step/Step.hpp"
#include "Define.h"
#include "SessionAiEngine.hpp"

namespace robot
{

enum AiEngineWordsLoadQuestionStatus
{
    eAiEngineWordsLoadQuestionStatus_start = 1,
    eAiEngineWordsLoadQuestionStatus_load_app = 2,
    eAiEngineWordsLoadQuestionStatus_load_app_ok = 3,
    eAiEngineWordsLoadQuestionStatus_load_words = 4,
    eAiEngineWordsLoadQuestionStatus_load_words_ok = 5,
    eAiEngineWordsLoadQuestionStatus_load_questions = 6,
    eAiEngineWordsLoadQuestionStatus_load_questions_ok = 7,
    eAiEngineWordsLoadQuestionStatus_end = 8,
};


class StepLoadAiEngineQuestions: public net::Step
{
public:
    StepLoadAiEngineQuestions(bool boForceLoadWords = false,net::Step *pNextStep = NULL);
    virtual ~StepLoadAiEngineQuestions();
    virtual net::E_CMD_STATUS Emit(int iErrno = 0, const std::string& strErrMsg = "", const std::string& strErrShow = "");
    virtual net::E_CMD_STATUS Callback(
                    const net::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody,
                    void* data = NULL);
    net::E_CMD_STATUS LoadApp();
    net::E_CMD_STATUS LoadWords();
    net::E_CMD_STATUS LoadAiEngineQuestion();
    virtual net::E_CMD_STATUS Timeout();
private:
    bool m_boForceLoadWords;
    net::uint16 m_timeout;
    std::list<uint32> m_appidlist;
    AiEngineWordsLoadQuestionStatus m_status;
    SessionAiEngine* m_pSessionAiEngine;
};

} /* namespace robot */

#endif /* SRC_StepLoadAiEngineQuestions_HPP_ */
