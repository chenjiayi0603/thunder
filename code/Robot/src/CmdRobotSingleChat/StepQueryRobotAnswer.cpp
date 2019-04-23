/*******************************************************************************
 * Project:  RobotServer
 * @file     StepQueryRobotAnswer.cpp
 * @brief 
 * @author   cjy
 * @date:    2017年2月5日
 * @note
 * Modify history:
 ******************************************************************************/
#include "StepQueryRobotAnswer.hpp"
#include "util/CommonUtils.hpp"
#include "util/UnixTime.hpp"
#include "util/bzhash.hpp"

namespace robot
{

StepQueryRobotAnswer::StepQueryRobotAnswer(
                const net::tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const robot_session::robot_single_msg_req &oRobotSingleMsg,
                const user_basic &basicInfo,
                RobotSession* pRobotSession,std::string &strFilteredQuestion)
    : m_stReqMsgShell(stMsgShell),m_oReqMsgHead(oInMsgHead),m_oRobotSingleMsg(oRobotSingleMsg),m_obasicInfo(basicInfo),
      m_pRobotSession(pRobotSession),m_stage(eStepQueryRobotAnswer_Start),m_nIndexid(0)
{
    m_strFilteredQuestion = strFilteredQuestion;
    m_uiTimeOut = 0;
    StartClock();
}

StepQueryRobotAnswer::~StepQueryRobotAnswer()
{
}


net::E_CMD_STATUS StepQueryRobotAnswer::GetPreQuestionFromRedis()
{
    /*
                           机器人前置问题列表。
                      数据类型： hash
              Key:    1:17:${appid}其中第一位1表示hash结构，第二位17表示智能前置问题列表,第三位${appid}表示应用id；
            Field：
            ${question_id}  Record
            ${question_id}  Record
            ${question_id}  Record
     * */
    char szRedisKey[32];//前置问题列表
    snprintf(szRedisKey,sizeof(szRedisKey),"%u:%u:%u",REDIS_T_HASH, IM_DATA_AI_PRE_QUESTION_LIST,
                    m_obasicInfo.appid());
    net::RedisOperator oRedisOper(
                    0,
                    szRedisKey,
                    "",
                    "HGET");
    net::uint64 question_id = lnet::HashStrToUint64(m_strFilteredQuestion.c_str(),m_strFilteredQuestion.size());
    oRedisOper.AddRedisField("",question_id);//0

    MsgHead oMsgHead;
    MsgBody oMsgBody;
    oMsgBody.set_body(oRedisOper.MakeMemOperate()->SerializeAsString());
    oMsgHead.set_cmd(net::CMD_REQ_STORATE);
    oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
    oMsgHead.set_seq(GetSequence());
    if (!SendToNext("PROXY", oMsgHead, oMsgBody))
    {
        LOG4_ERROR("%s() send to dataproxy error!",__FUNCTION__);
        return net::STATUS_CMD_FAULT;
    }
    LOG4_TRACE("%s() oRedisOper(%s)",__FUNCTION__,oRedisOper.MakeMemOperate()->DebugString().c_str());
    m_stage = eStepQueryRobotAnswer_Inquery_Pre_question;
    return net::STATUS_CMD_RUNNING;
}

net::E_CMD_STATUS StepQueryRobotAnswer::GetPreQuestion()
{
    /*
                           机器人前置问题列表。
                      数据类型： hash
              Key:    1:17:${appid}其中第一位1表示hash结构，第二位17表示智能前置问题列表,第三位${appid}表示应用id；
            Field：
            ${question_id}  Record
            ${question_id}  Record
            ${question_id}  Record
     * */
    char szRedisKey[32];//前置问题列表
    snprintf(szRedisKey,sizeof(szRedisKey),"%u:%u:%u",REDIS_T_HASH, IM_DATA_AI_PRE_QUESTION_LIST,
                    m_obasicInfo.appid());
    net::MemOperator oMemOper(
                    0,
                    "tb_ai_pre_question",
                    DataMem::MemOperate::DbOperate::SELECT,
                    szRedisKey,
                    "HMSET",
                    "HMGET");
    net::uint64 question_id = lnet::HashStrToUint64(m_strFilteredQuestion.c_str(),m_strFilteredQuestion.size());
    oMemOper.AddRedisField("",question_id);//0
    oMemOper.AddDbField("question_id");//0
    oMemOper.AddDbField("appid");//1
    oMemOper.AddDbField("question_type");//2
    oMemOper.AddDbField("question");//3
    oMemOper.AddDbField("answer");//4
    oMemOper.AddDbField("answer_template");//5
    oMemOper.AddDbField("create_date");//6
    oMemOper.AddDbField("update_date");//7
    {//db条件
        oMemOper.AddCondition(DataMem::MemOperate::DbOperate::Condition::EQ,
                        "appid",
                        m_obasicInfo.appid());
        oMemOper.AddCondition(DataMem::MemOperate::DbOperate::Condition::EQ,
                        "question_id",
                        question_id);
    }
    MsgHead oMsgHead;
    MsgBody oMsgBody;
    oMsgBody.set_body(oMemOper.MakeMemOperate()->SerializeAsString());
    oMsgHead.set_cmd(net::CMD_REQ_STORATE);
    oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
    oMsgHead.set_seq(GetSequence());
    if (!SendToNext("PROXY", oMsgHead, oMsgBody))
    {
        LOG4_ERROR("%s() send to dataproxy error!",__FUNCTION__);
        return net::STATUS_CMD_FAULT;
    }
    LOG4_TRACE("%s() oMemOper(%s)",__FUNCTION__,oMemOper.MakeMemOperate()->DebugString().c_str());
    m_stage = eStepQueryRobotAnswer_Inquery_Pre_question;
    return net::STATUS_CMD_RUNNING;
}


net::E_CMD_STATUS StepQueryRobotAnswer::GetSphinxEngineAnswerId()
{
    /*
    1  tb_ai_engine_question   智能引擎问题表 不分表
    2.表结构字段详细说明
            字段描述见如下表（表3-5-3）：
            序号  字段  类型  描述  是否为主键
    1   index_id    BIGINT(24) UNSIGNED 索引问题id  自增主键，也用于作为搜索索引，并作为增量索引依据
    2   standard_question_id    BIGINT(24) UNSIGNED 标准问题id
    2   appid   INT(11) UNSIGNED    应用id
    3   question_attribute  TINYINT(4) UNSIGNED 问题属性 1：标准问题 2相似问题
    4   question_type   TINYINT(4) UNSIGNED 问题类型 1：未分类 2：常用语
    5   question    VARCHAR(128)    请求的问题（标准问题或相似问题）
    6   answer  VARCHAR(1024)   标准问题的答案
    6   create_date DATETIME    该条记录的生成时间;默认值为 '1970-01-01 08:00:00'
    7   update_date DATETIME    该条记录的更新时间;默认值为 '1970-01-01 08:00:00'
     * */
    LOG4_TRACE("%s() basicInfo(%s) strFilteredQuestion(%u,%s) oRobotSingleMsg(%s)",
                    __FUNCTION__,m_obasicInfo.DebugString().c_str(),
                    m_strFilteredQuestion.size(),m_strFilteredQuestion.c_str(),
                    m_oRobotSingleMsg.DebugString().c_str());
    if(!m_pRobotSession->QuerySphinxAnswer(m_strFilteredQuestion,"question",
                    "appid",m_obasicInfo.appid(),m_nIndexid,m_strAnswer))
    {
        LOG4_TRACE("%s() failed to get answer",__FUNCTION__);
    }
    /*
     //客户发送机器人单聊消息响应
    message robot_single_msg_ack
    {
        common.errorinfo error = 1; // 应答信息
        uint64 msg_id     = 2; // 消息ID(微秒级时间戳)
        uint32 send_time  = 3; // 发送时间(从1970年开始的秒数)
        uint32 msg_type = 4;//消息类型 1:文字类型
        bytes msg  = 5; // 响应消息内容
    }
     * */
    if (m_strAnswer.size() > 0)
    {
        LOG4_TRACE("%s() ok to get nIndexid(%llu) strAnswer(%s) QuerySphinxAnswer",
                        __FUNCTION__,m_nIndexid,m_strAnswer.c_str());
        robot_session::robot_single_msg_ack oAck;
        oAck.set_msg_id(lnet::GetUniqueId(GetNodeId(),GetWorkerIndex()));
        oAck.set_send_time(lnet::GetCurrentTime());
        oAck.set_msg_type(eRobotMsgType_text);
        oAck.set_msg_template(1);
        oAck.set_msg(m_strAnswer);
        EndClock();
        Response(ERR_OK,oAck);
        m_stage = eStepQueryRobotAnswer_Sphinx_Engine_question_ok;
        return(net::STATUS_CMD_COMPLETED);
    }
    else
    {
        LOG4_TRACE("%s() failed to get answer,QuerySphinxAnswer",__FUNCTION__);
        DefaultResponse();
        m_stage = eStepQueryRobotAnswer_Sphinx_Engine_question_ok;
        return net::STATUS_CMD_COMPLETED;
    }
}

net::E_CMD_STATUS StepQueryRobotAnswer::GetSessionAiEngineQuestion()
{
    SessionAiEngine* pSessionAiEngine = GetSessionAiEngine(GetLabor());
    if (!pSessionAiEngine)
    {
        LOG4_WARN("%s() GetSessionAiEngine failed",__FUNCTION__);
        Response(ERR_SYSTEM_ERROR);
        return net::STATUS_CMD_FAULT;
    }
    std::vector<ai_engine_question> aiQuestionVec;
    if (!pSessionAiEngine->GetAiQuestionListByReqQuestion(m_strFilteredQuestion,m_obasicInfo.appid(),aiQuestionVec))
    {
        LOG4_TRACE("%s() GetBestAiQuestionByQuestion no match ai queston",__FUNCTION__);
        DefaultResponse();
        m_stage = eStepQueryRobotAnswer_Session_Ai_Engine_question_ok;
        return net::STATUS_CMD_COMPLETED;
    }
    if (aiQuestionVec.size() > 0)
    {
        std::string answer;
        BuildAnswer(answer,aiQuestionVec);
        /*
         {"title":"测试引导1","sub_questions":["引导子问题1"]}
         * */
        robot_session::robot_single_msg_ack oAck;
        //logic生成msgid
        //oAck.set_msg_id(lnet::GetUniqueId(GetNodeId(),GetWorkerIndex()));
        oAck.set_send_time(lnet::GetCurrentTime());
        oAck.set_msg_type(eRobotMsgType_text);
        oAck.set_msg(answer);
        oAck.set_msg_template(ePreQuestionAnswerTemplate_guide);

        EndClock();
        Response(ERR_OK,oAck);
        m_stage = eStepQueryRobotAnswer_Session_Ai_Engine_question_ok;
        MatchQuestionLog(aiQuestionVec);
        return net::STATUS_CMD_COMPLETED;
    }
    LOG4_WARN("%s() GetBestAiQuestionByQuestion no match",__FUNCTION__);
    DefaultResponse();
    m_stage = eStepQueryRobotAnswer_Session_Ai_Engine_question_ok;
    return net::STATUS_CMD_COMPLETED;
}

net::E_CMD_STATUS StepQueryRobotAnswer::GetAiAnswer()
{
    /*
                机器人搜索引擎问题列表。
          数据类型： hash
          Key:    1:19:${appid}其中第一位1表示hash结构，第二位19表示智能机器人引擎问题列表,第三位${appid}表示应用id；
        Field：
        ${index_id}  Record
        ${index_id}  Record
        ${index_id}  Record
        Record顺序 {index_id(0) standard_question_id(1) appid(2) question_attribute(3) question_type(4) question(5) answer(6) create_date(7) update_date(8)}
     * */
    MsgHead oMsgHead;
    MsgBody oMsgBody;
    // 1:19:${appid}
    char szRedisKey[32];
    snprintf(szRedisKey,sizeof(szRedisKey),"%u:%u:%u",REDIS_T_HASH, IM_DATA_AI_ROBOT_QUESTION_LIST,
                    m_obasicInfo.appid());
    net::MemOperator oMemOper(
                    0,
                    "tb_ai_engine_question",
                    DataMem::MemOperate::DbOperate::SELECT,
                    szRedisKey,
                    "HSET",
                    "HGET");
    oMemOper.AddField("index_id");//0
    oMemOper.AddDbField("standard_question_id");//1
    oMemOper.AddDbField("appid");//2
    oMemOper.AddDbField("question_attribute");//3
    oMemOper.AddDbField("question_type");//4
    oMemOper.AddDbField("question");//5
    oMemOper.AddDbField("answer");//6
    oMemOper.AddDbField("create_date");//7
    oMemOper.AddDbField("update_date");//8
    {//db条件
        oMemOper.AddCondition(DataMem::MemOperate::DbOperate::Condition::EQ,
                        "appid",
                        m_obasicInfo.appid());
        oMemOper.AddCondition(DataMem::MemOperate::DbOperate::Condition::EQ,
                        "index_id",
                        (net::uint64)m_nIndexid);
    }
    oMsgBody.set_body(oMemOper.MakeMemOperate()->SerializeAsString());
    oMsgHead.set_cmd(net::CMD_REQ_STORATE);
    oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
    oMsgHead.set_seq(GetSequence());
    LOG4_TRACE("%s() query for ai answer,appid(%u),m_nIndexid(%llu)",__FUNCTION__,m_obasicInfo.appid(),m_nIndexid);
    if (!SendToNext("PROXY", oMsgHead, oMsgBody))
    {
        LOG4_ERROR("%s() send to dataproxy error!",__FUNCTION__);
        Response(ERR_SYSTEM_ERROR);
        return(net::STATUS_CMD_FAULT);
    }
    m_stage = eStepQueryRobotAnswer_Inquery_Engine_question;
    return(net::STATUS_CMD_RUNNING);
}

net::E_CMD_STATUS StepQueryRobotAnswer::Emit(int iErrno, const std::string& strErrMsg, const std::string& strErrShow)
{
    if(eStepQueryRobotAnswer_Start == m_stage)
    {
        return GetPreQuestionFromRedis();//只读取redis获取前置问题
    }
    else if (eStepQueryRobotAnswer_Inquery_Pre_question_ok == m_stage)
    {
        return GetSessionAiEngineQuestion();
    }
    else if(eStepQueryRobotAnswer_Inquery_Engine_question_ok == m_stage)
    {
        return (net::STATUS_CMD_COMPLETED);
    }
    return(net::STATUS_CMD_RUNNING);
}

void StepQueryRobotAnswer::Response(int iErrno)
{
    MsgHead oOutMsgHead;
    MsgBody oOutMsgBody;
    robot_session::robot_single_msg_ack oRsp;
    common::errorinfo* pError = new common::errorinfo();
    pError->set_error_code(robot_err_code(iErrno));
    pError->set_error_info(robot_err_msg(iErrno));
    pError->set_error_client_show(robot_err_msg(iErrno));
    oRsp.set_allocated_error(pError);
    oOutMsgBody.set_body(oRsp.SerializeAsString());
    oOutMsgHead.set_cmd(m_oReqMsgHead.cmd() + 1);
    oOutMsgHead.set_seq(m_oReqMsgHead.seq());
    oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    if (!GetLabor()->SendTo(m_stReqMsgShell, oOutMsgHead, oOutMsgBody))
    {
        LOG4_ERROR("%s()failed to send error info to fd %d fd_seq %u",__FUNCTION__,
                        m_stReqMsgShell.iFd, m_stReqMsgShell.ulSeq);
    }
}

void StepQueryRobotAnswer::DefaultResponse()
{
    robot_session::robot_single_msg_ack oAck;
    //logic生成msgid
    //oAck.set_msg_id(lnet::GetUniqueId(GetNodeId(),GetWorkerIndex()));
    oAck.set_send_time(lnet::GetCurrentTime());
    oAck.set_msg_type(eRobotMsgType_text);
    oAck.set_msg_template(1);
    oAck.set_msg(m_pRobotSession->GetDefaultAnswer());
    EndClock();
    Response(ERR_OK,oAck);
}

void StepQueryRobotAnswer::Response(int iErrno,robot_session::robot_single_msg_ack &oRsp)
{
    MsgHead oOutMsgHead;
    MsgBody oOutMsgBody;
    common::errorinfo* pError = new common::errorinfo();
    pError->set_error_code(robot_err_code(iErrno));
    pError->set_error_info(robot_err_msg(iErrno));
    pError->set_error_client_show(robot_err_msg(iErrno));
    oRsp.set_allocated_error(pError);
    oOutMsgBody.set_body(oRsp.SerializeAsString());
    oOutMsgHead.set_cmd(m_oReqMsgHead.cmd() + 1);
    oOutMsgHead.set_seq(m_oReqMsgHead.seq());
    oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    if (!GetLabor()->SendTo(m_stReqMsgShell, oOutMsgHead, oOutMsgBody))
    {
        LOG4_ERROR("%s()failed to send error info to fd %d fd_seq %u",__FUNCTION__,
                        m_stReqMsgShell.iFd, m_stReqMsgShell.ulSeq);
    }
}

void StepQueryRobotAnswer::BuildAnswer(std::string &answer,const std::vector<ai_engine_question> &aiQuestionVec)
{
    const ai_engine_question& question = *aiQuestionVec.begin();
    LOG4_TRACE("%s() GetAiQuestionListByReqQuestion m_strFilteredQuestion(%s) match ai question(%s) answer(%s)",
                    __FUNCTION__,m_strFilteredQuestion.c_str(),question.question.c_str(),question.answer.c_str());
    /*
     {"title":"测试引导1","sub_questions":["引导子问题1"]}
     * */
    if (question.GetLevenshtein() == 0)
    {
        lnet::CJsonObject objSubQuestions;
        objSubQuestions.Add(question.answer);
        lnet::CJsonObject objAnswerJson;
        objAnswerJson.Add("title",question.question);
        objAnswerJson.Add("sub_questions",objSubQuestions);
        answer = objAnswerJson.ToString();
    }
    else
    {
        lnet::CJsonObject objSubQuestions;
        objSubQuestions.Add(question.question);
        if (aiQuestionVec.size() > 1)
        {
            std::vector<ai_engine_question>::const_iterator cit = ++aiQuestionVec.begin();
            std::vector<ai_engine_question>::const_iterator citEnd = ++aiQuestionVec.end();
            for(;cit != citEnd;++cit)
            {
                if (question.uiMatchWordsCounter == cit->uiMatchWordsCounter && (cit->GetLevenshtein() - question.GetLevenshtein() < 3))
                {
                    objSubQuestions.Add(cit->question);
                }
            }
        }
        if (objSubQuestions.GetArraySize() > 1)
        {
            lnet::CJsonObject objAnswerJson;
            objAnswerJson.Add("title",m_pRobotSession->GetAiQuestionGuide());
            objAnswerJson.Add("sub_questions",objSubQuestions);
            answer = objAnswerJson.ToString();
        }
        else
        {
            objSubQuestions.Clear();
            objSubQuestions.Add(question.answer);
            lnet::CJsonObject objAnswerJson;
            objAnswerJson.Add("title",question.question);
            objAnswerJson.Add("sub_questions",objSubQuestions);
            answer = objAnswerJson.ToString();
        }
    }
}

void StepQueryRobotAnswer::MatchQuestionLog(const std::vector<ai_engine_question> &aiQuestionVec)
{
    int questionNo(1);
    std::vector<ai_engine_question>::const_iterator it = aiQuestionVec.begin();
    std::vector<ai_engine_question>::const_iterator itEnd = aiQuestionVec.end();
    for(;it != itEnd;++it,++questionNo)
    {
        LOG4_TRACE("%s() m_strFilteredQuestion(%s) ai_engine_question(questionNo:%d,question:%s,uiMatchWordsCounter(%u) "
                        "Levenshtein(%d)) strReqQuestion(%s)",
                        __FUNCTION__,m_strFilteredQuestion.c_str(),questionNo,it->question.c_str(),it->uiMatchWordsCounter,
                        it->GetLevenshtein(),it->strReqQuestion.c_str());
    }
    /*
    m_strFilteredQuestion(企业标) ai_engine_question(questionNo:1,question:企业的标,uiMatchWordsCounter(3) Levenshtein(3)) strReqQuestion(企业标)
    m_strFilteredQuestion(企业标) ai_engine_question(questionNo:2,question:企业的标1,uiMatchWordsCounter(3) Levenshtein(4)) strReqQuestion(企业标)
    m_strFilteredQuestion(企业标) ai_engine_question(questionNo:3,question:你我企业的标,uiMatchWordsCounter(3) Levenshtein(9)) strReqQuestion(企业标)
    m_strFilteredQuestion(企业标) ai_engine_question(questionNo:4,question:你我的企业标,uiMatchWordsCounter(3) Levenshtein(9)) strReqQuestion(企业标)
    m_strFilteredQuestion(企业标) ai_engine_question(questionNo:5,question:标企业,uiMatchWordsCounter(2) Levenshtein(6)) strReqQuestion(企业标)
    m_strFilteredQuestion(企业标) ai_engine_question(questionNo:6,question:流标,uiMatchWordsCounter(1) Levenshtein(5)) strReqQuestion(企业标)
    m_strFilteredQuestion(企业标) ai_engine_question(questionNo:7,question:流标1,uiMatchWordsCounter(1) Levenshtein(6)) strReqQuestion(企业标)
    m_strFilteredQuestion(企业标) ai_engine_question(questionNo:8,question:转让标,uiMatchWordsCounter(1) Levenshtein(6)) strReqQuestion(企业标)
    m_strFilteredQuestion(企业标) ai_engine_question(questionNo:9,question:秒标,uiMatchWordsCounter(1) Levenshtein(6)) strReqQuestion(企业标)
    m_strFilteredQuestion(企业标) ai_engine_question(questionNo:10,question:投标,uiMatchWordsCounter(1) Levenshtein(6)) strReqQuestion(企业标)
    m_strFilteredQuestion(企业标) ai_engine_question(questionNo:11,question:转让标1,uiMatchWordsCounter(1) Levenshtein(7)) strReqQuestion(企业标)
    m_strFilteredQuestion(企业标) ai_engine_question(questionNo:12,question:自动投标,uiMatchWordsCounter(1) Levenshtein(9)) strReqQuestion(企业标)
     * */
}

net::E_CMD_STATUS StepQueryRobotAnswer::Callback(
                    const net::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody,
                    void* data)
{
    if (net::CMD_RSP_SYS_ERROR == oInMsgHead.cmd())
    {
        LOG4_ERROR("%s()system response error",__FUNCTION__);
        Response(ERR_SYSTEM_ERROR);
        return net::STATUS_CMD_FAULT;
    }
    DataMem::MemRsp oRsp;
    if(!oRsp.ParseFromString(oInMsgBody.body()))
    {
        LOG4_ERROR("%s()parse protobuf data fault",__FUNCTION__);
        Response(ERR_SYSTEM_ERROR);
        return net::STATUS_CMD_FAULT;
    }
    //读存储出错
    if(0 != oRsp.err_no())
    {
        if(oRsp.err_msg().size() > 0)
        {
            LOG4_ERROR("%s() dataproxy error %d: %s!",__FUNCTION__,oRsp.err_no(),oRsp.err_msg().c_str());
        }
        else
        {
            LOG4_ERROR("%s() dataproxy error %d!",__FUNCTION__,oRsp.err_no());
        }
        Response(ERR_SYSTEM_ERROR);
        return net::STATUS_CMD_FAULT;
    }
    if (eStepQueryRobotAnswer_Inquery_Pre_question == m_stage)
    {
        int iRecSize = oRsp.record_data_size();
        LOG4_TRACE("%s() iRecSize(%d) oMemRsp(%s)",
                                    __FUNCTION__,iRecSize,oRsp.DebugString().c_str());
        if(iRecSize > 0 && oRsp.record_data(0).field_info_size() > 0)
        {
            ::DataMem::Record oRecord;
            if (oRecord.ParseFromString(oRsp.record_data(0).field_info(0).col_value()))
            {
                if (oRecord.field_info_size() == 8)
                {
                    /*
                     {"title":"测试引导1","sub_questions":["引导子问题1"]}
                     * */
                    robot_session::robot_single_msg_ack oAck;
                    //logic生成msgid
                    //oAck.set_msg_id(lnet::GetUniqueId(GetNodeId(),GetWorkerIndex()));
                    oAck.set_send_time(lnet::GetCurrentTime());
                    oAck.set_msg_type(eRobotMsgType_text);
                    oAck.set_msg(oRecord.field_info(4).col_value());
                    oAck.set_msg_template(::strtoul(oRecord.field_info(5).col_value().c_str(),NULL,10));
                    Response(ERR_OK,oAck);
                    LOG4_TRACE("%s() robot_single_msg_ack(%s) oRecord(%s)",__FUNCTION__,
                                    oAck.DebugString().c_str(),oRecord.DebugString().c_str());
                    InqueryPrequestionClock();
                    m_stage = eStepQueryRobotAnswer_Inquery_Pre_question_ok;
                    return(net::STATUS_CMD_COMPLETED);
                }
                else
                {
                    LOG4_ERROR("%s() oRecord.field_info_size() != 8",__FUNCTION__);
                }
            }

        }
        InqueryPrequestionClock();
        m_stage = eStepQueryRobotAnswer_Inquery_Pre_question_ok;
        return Emit();
    }
    else if (eStepQueryRobotAnswer_Inquery_Engine_question == m_stage)
    {
        int iRecSize = oRsp.record_data_size();
        LOG4_TRACE("%s() iRecSize(%d) oMemRsp(%s)",
                                    __FUNCTION__,iRecSize,oRsp.DebugString().c_str());
        if(oRsp.record_data_size() > 0 && oRsp.record_data(0).field_info_size() > 0)
        {
//            数据类型： hash
//            Key:    1:19:${appid}其中第一位1表示hash结构，第二位19表示智能机器人引擎问题列表,第三位${appid}表示应用id；
//          Field：
//          ${question_id}  Record
//          ${question_id}  Record
//          ${question_id}  Record
//          Record顺序 {index_id(0) standard_question_id(1) appid(2) question_attribute(3) question_type(4) question(5) answer(6) create_date(7) update_date(8)}
            /*
             //客户发送机器人单聊消息响应
            message robot_single_msg_ack
            {
                common.errorinfo error = 1; // 应答信息
                uint64 msg_id     = 2; // 消息ID(微秒级时间戳)
                uint32 send_time  = 3; // 发送时间(从1970年开始的秒数)
                uint32 msg_type = 4;//消息类型 1:文字类型
                bytes msg  = 5; // 响应消息内容
            }
             * */
            DataMem::Record oRecord;
            if (oRecord.ParseFromString(oRsp.record_data(0).field_info(0).col_value()))
            {
                LOG4_TRACE("%s() parse from oRecord(%s).appid(%u),m_nIndexid(%llu)!",
                                __FUNCTION__,oRecord.DebugString().c_str(),m_obasicInfo.appid(),m_nIndexid);
                if (oRecord.field_info_size() == 9)
                {
                    LOG4_TRACE("%s() got answer m_nIndexid(%llu) answer(%s)!",
                            __FUNCTION__,m_nIndexid,oRecord.field_info(6).col_value().c_str());
                    robot_session::robot_single_msg_ack oAck;
                    //logic生成msgid
                    //oAck.set_msg_id(lnet::GetUniqueId(GetNodeId(),GetWorkerIndex()));
                    oAck.set_send_time(lnet::GetCurrentTime());
                    oAck.set_msg_type(eRobotMsgType_text);
                    oAck.set_msg(oRecord.field_info(6).col_value());
                    RobotAnswerEnginequestionClock();
                    EndClock();
                    Response(ERR_OK,oAck);
                    m_stage = eStepQueryRobotAnswer_Inquery_Engine_question_ok;
                    return(net::STATUS_CMD_COMPLETED);
                }
                else
                {
                    LOG4_ERROR("%s() oRecord.field_info_size() != 9.appid(%u),m_nIndexid(%llu)!",__FUNCTION__,m_obasicInfo.appid(),m_nIndexid);
                    Response(ERR_SYSTEM_ERROR);
                    return(net::STATUS_CMD_FAULT);
                }
            }
            else
            {
                LOG4_ERROR("%s() failed to parse from oRecord.appid(%u),m_nIndexid(%llu)!",__FUNCTION__,m_obasicInfo.appid(),m_nIndexid);
                Response(ERR_SYSTEM_ERROR);
                return(net::STATUS_CMD_FAULT);
            }
        }
        else
        {
            LOG4_TRACE("%s() do not found answer for appid(%u),m_nIndexid(%llu)!",__FUNCTION__,m_obasicInfo.appid(),m_nIndexid);
            RobotAnswerEnginequestionClock();
            EndClock();
            Response(ERR_SYSTEM_ERROR);
            return(net::STATUS_CMD_COMPLETED);
        }
    }
    return(net::STATUS_CMD_COMPLETED);
}

net::E_CMD_STATUS StepQueryRobotAnswer::Timeout()
{
    if (m_uiTimeOut < 3)
    {
        ++m_uiTimeOut;
        return(net::STATUS_CMD_RUNNING);
    }
    LOG4CPLUS_WARN_FMT(GetLogger(), "cmd %u, seq %lu, robot timeout!", m_oReqMsgHead.cmd(), m_oReqMsgHead.seq());
    Response(ERR_SERVER_TIMEOUT);
    return(net::STATUS_CMD_FAULT);
}



} /* namespace robot */
