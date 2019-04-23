/*******************************************************************************
 * Project:  LogicServer
 * @file     CmdRobotPreQuestion.cpp
 * @brief 
 * @author   cjy
 * @date:    2016年12月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include "util/CommonUtils.hpp"
#include "CmdEditAiQuestion.hpp"

#ifdef __cplusplus
extern "C" {
#endif
net::Cmd* create()
{
    net::Cmd* pCmd = new robot::CmdEditAiQuestion();
    return(pCmd);
}
#ifdef __cplusplus
}
#endif

namespace robot
{

CmdEditAiQuestion::CmdEditAiQuestion()
{
    m_pSessionAiEngine = NULL;
}

CmdEditAiQuestion::~CmdEditAiQuestion()
{
}

bool CmdEditAiQuestion::Init()
{
	return true;
}

bool CmdEditAiQuestion::AnyMessage(
                const net::tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
    m_stMsgShell = stMsgShell;
    m_oInMsgHead = oInMsgHead;
    LOG4_TRACE("%s()", __FUNCTION__);
    if(oInMsgBody.additional().size() == 0)
    {
        LOG4_ERROR("%s() additional not exist",__FUNCTION__);
        return(false);
    }
    user_basic basicInfo;
    if(!basicInfo.ParseFromString(oInMsgBody.additional()))
    {
        LOG4_ERROR("%s() Parse additional failed",__FUNCTION__);
        return(false);
    }
    if(!parseMsg(oInMsgBody,basicInfo))
    {
        LOG4_ERROR("parseHttpMsg failed:%d:%s", robot_err_code(ERR_MSG_BODY_DECODE), robot_err_msg(ERR_MSG_BODY_DECODE));
        return(false);
    }
    {//引擎添加编辑问题
        m_pSessionAiEngine = GetSessionAiEngine(GetLabor());
        if (m_pSessionAiEngine)
        {
            /*
           message  similar_question
           {
               uint64 index_id = 1;//问题索引id
               bytes question = 2; //相似问题
               uint32 create_date = 3;//创建时间
               uint32 update_date = 4;//更新时间
           }
           message  ai_question
           {
               uint64 index_id = 1;//问题索引id
               uint64 standard_question_id = 2;//标准问题id
               uint32 question_type = 3;//问题类型  如，0：全部分类 1：未分类 2：常用语
               bytes question = 4; //标准问题,如“你好”
               bytes answer= 5; //标准答案
               uint32 create_date = 6;//创建时间
               uint32 update_date = 7;//更新时间
               repeated similar_question similar_questions_list = 8;//相似问题列表,如 “您好”,”HI”,”亲”（列表请求中不发送）
           }
           message  edit_ai_question_req
           {
               ai_question  question_content = 1;//问题内容
               uint32 update_type = 2;//更新类型 1：增加 2:删除 （添加时如果是同一个标准问题，而答案不同，则会替换）
           }
            * */
            //通知时所有问题放置在相似问题列表
            const ::robot_knowledge::ai_question& question_content = m_oEditAiQuestionReq.question_content();
            int similar_questions_list_size = question_content.similar_questions_list_size();
            if (similar_questions_list_size > 0)
            {
                for(int i = 0;i < similar_questions_list_size;++i)
                {
                    const ::robot_knowledge::similar_question& similar_question = question_content.similar_questions_list(i);
                    ai_engine_question aiEngineQuestion;
                    aiEngineQuestion.index_id = similar_question.index_id();//value index_id
                    aiEngineQuestion.standard_question_id = 0;//value standard_question_id
                    aiEngineQuestion.appid = basicInfo.appid();//value appid
                    aiEngineQuestion.question_attribute = 0;//value appid
                    aiEngineQuestion.question_type = question_content.question_type();//value question_type
                    aiEngineQuestion.question = similar_question.question();//value question
                    aiEngineQuestion.answer = question_content.answer();//value answer
                    aiEngineQuestion.create_date = question_content.create_date();//value create_date
                    aiEngineQuestion.update_date = question_content.update_date();//value update_date
                    if (m_oEditAiQuestionReq.update_type() == eEditAiQuestion_Add)
                    {
                        m_pSessionAiEngine->AddAiEngineQuestionAppend(aiEngineQuestion);
                    }
                    else if (m_oEditAiQuestionReq.update_type() == eEditAiQuestion_Del)
                    {
                        m_pSessionAiEngine->DelAiEngineQuestionAppend(aiEngineQuestion);
                    }
                    else
                    {
                        LOG4_ERROR("%s() invalid update_type(%u)",__FUNCTION__,m_oEditAiQuestionReq.update_type());
                        return false;
                    }
                }
            }
        }
        else
        {
            LOG4_ERROR("%s() failed to GetSessionAiEngine!",__FUNCTION__);
        }
    }
    return(true);
}

bool CmdEditAiQuestion::parseMsg(const MsgBody& oInMsgBody,const user_basic &basicInfo)
{
    m_oEditAiQuestionReq.Clear();
    if (!ParseFromMsg(oInMsgBody,m_oEditAiQuestionReq))
    {
        LOG4_ERROR("%s() ParseFromMsg(oInMsgBody,m_oEditAiQuestionReq) failed!",__FUNCTION__);
        return(false);
    }
    LOG4_DEBUG("%s() m_oEditAiQuestionReq(%s)",m_oEditAiQuestionReq.DebugString().c_str());
    return(true);
}





} /* namespace robot */
