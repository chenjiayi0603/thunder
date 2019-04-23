/*******************************************************************************
 * Project:  RobotServer
 * @file     StepLoadAiEngineQuestions.cpp
 * @brief 
 * @author   chenjiayi
 * @date:    2015年12月12日
 * @note
 * Modify history:
 ******************************************************************************/
#include "StepLoadAiEngineQuestions.hpp"

#include "RobotRedisProto.h"
#include "storage/MemOperator.hpp"
#include "SessionAiEngine.hpp"

namespace robot
{

StepLoadAiEngineQuestions::StepLoadAiEngineQuestions(bool boForceLoadWords,net::Step *pNextStep)
                : net::Step(pNextStep),
                  m_boForceLoadWords(boForceLoadWords),m_timeout(0),m_status(eAiEngineWordsLoadQuestionStatus_start),m_pSessionAiEngine(NULL)
{
}

StepLoadAiEngineQuestions::~StepLoadAiEngineQuestions()
{
}

net::E_CMD_STATUS StepLoadAiEngineQuestions::Emit(int iErrno, const std::string& strErrMsg, const std::string& strErrShow)
{
    LOG4_TRACE("%s() m_status(%d) ",__FUNCTION__,m_status);
    if(net::ERR_OK != iErrno)
    {
        LOG4_ERROR("%s() %s StepLoadAiEngineQuestions::Emit iErrno error(%d) ",__FUNCTION__,iErrno);
        return net::STATUS_CMD_FAULT;
    }
    if(eAiEngineWordsLoadQuestionStatus_start == m_status)
    {
        return LoadApp();
    }
    else if (eAiEngineWordsLoadQuestionStatus_load_app_ok == m_status)
    {
        if (m_appidlist.size() > 0)
        {
            return LoadWords();
        }
        LOG4_WARN("%s() no app to load",__FUNCTION__);
        m_status = eAiEngineWordsLoadQuestionStatus_end;
        return Emit();
    }
    else if (eAiEngineWordsLoadQuestionStatus_load_questions_ok == m_status)
    {
        if (m_appidlist.size() > 0)
        {
            m_appidlist.pop_front();
        }
        if (m_appidlist.size() > 0)
        {
            return LoadAiEngineQuestion();
        }
        if (m_pSessionAiEngine)
        {
            LOG4_TRACE("%s() m_pSessionAiEngine Build m_status(%d) ",__FUNCTION__,m_status);
            m_pSessionAiEngine->Build();
        }
        m_status = eAiEngineWordsLoadQuestionStatus_end;
        return Emit();
    }
    else if (eAiEngineWordsLoadQuestionStatus_end == m_status)
    {
        LOG4_TRACE("%s() eAiEngineWordsLoadQuestionStatus_end m_status(%d) ",__FUNCTION__,m_status);
        if (GetNextStep())
        {
            NextStep();
        }
        return net::STATUS_CMD_COMPLETED;
    }
    LOG4_TRACE("%s() invalid status(%u)",__FUNCTION__,m_status);
    return net::STATUS_CMD_RUNNING;
}

//加载app
net::E_CMD_STATUS StepLoadAiEngineQuestions::LoadApp()
{
    /*
         app域名表tb_app_domain
                            所包含数据库表及分表策略，如下
                            序号  表名  表信息描述   分表策略
        1   tb_app_domain   app域名表  不分表
                        对用户账号表的字段描述见如下表（表3-1-1）：
                        序号  字段  类型  描述  是否主键
        1   domain  VARCHAR(128)    域名  主键
        2   appid   INT(11) UNSIGNED    应用id,服务器逻辑使用
        3   appguid VARCHAR(64) 应用appguid，对外对接使用
        4   appsecret   VARCHAR(64) 应用secret，对外对接使用
        5   app_name    VARCHAR(128)    应用名,如你我金融
        6   create_time DATETIME    创建时间，默认值为 '1970-01-01 08:00:00'
        7   update_time DATETIME    更新时间，默认值为 '1970-01-01 08:00:00'
     * */
    net::DbOperator oDbOper(
                    0,
                    "tb_app_domain",
                    DataMem::MemOperate::DbOperate::SELECT);
    oDbOper.AddDbField("domain");//0
    oDbOper.AddDbField("appid");//1
    oDbOper.AddDbField("appguid");//2
    oDbOper.AddDbField("appsecret");//3
    oDbOper.AddDbField("app_name");//4
    oDbOper.AddDbField("create_time");//5
    oDbOper.AddDbField("update_time");//6
    MsgHead oMsgHead;
    MsgBody oMsgBody;
    oMsgBody.set_body(oDbOper.MakeMemOperate()->SerializeAsString());
    oMsgHead.set_cmd(net::CMD_REQ_STORATE);
    oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
    oMsgHead.set_seq(GetSequence());
    if (!SendToNext("PROXY", oMsgHead, oMsgBody))
    {
        LOG4_ERROR("%s() send to dataproxy error!",__FUNCTION__);
        return net::STATUS_CMD_FAULT;
    }
    LOG4_TRACE("%s() oDbOper(%s)",__FUNCTION__,oDbOper.MakeMemOperate()->DebugString().c_str());
    m_status = eAiEngineWordsLoadQuestionStatus_load_app;
    return net::STATUS_CMD_RUNNING;
}

net::E_CMD_STATUS StepLoadAiEngineQuestions::LoadWords()
{
    if (m_appidlist.size() == 0)
    {
        LOG4_ERROR("%s() m_appidlist.size() == 0!",__FUNCTION__);
        return net::STATUS_CMD_COMPLETED;
    }
    m_pSessionAiEngine = GetSessionAiEngine(GetLabor());
    if (!m_pSessionAiEngine)
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(), "%s() GetSessionAiEngine failed",__FUNCTION__);
        return net::STATUS_CMD_FAULT;
    }
    LOG4CPLUS_TRACE_FMT(GetLogger(), "%s() GetSessionAiEngine ok!",__FUNCTION__);
    {
        if (!m_pSessionAiEngine->LoadAiEngineWords(m_boForceLoadWords))
        {
            LOG4CPLUS_ERROR_FMT(GetLogger(), "%s() LoadAiEngineWords failed",__FUNCTION__);
            return net::STATUS_CMD_FAULT;
        }
        LOG4CPLUS_TRACE_FMT(GetLogger(), "%s() LoadAiEngineWords ok!",__FUNCTION__);
    }
    m_status = eAiEngineWordsLoadQuestionStatus_load_words_ok;
    return LoadAiEngineQuestion();
}

//加载引擎问题
net::E_CMD_STATUS StepLoadAiEngineQuestions::LoadAiEngineQuestion()
{
    if (m_appidlist.size() == 0)
    {
        LOG4_ERROR("%s() m_appidlist.size() == 0!",__FUNCTION__);
        return net::STATUS_CMD_FAULT;
    }
    uint32 appid = m_appidlist.front();
    /*
       tb_ai_question  智能答案表 不分表
                    字段  类型  描述  是否为主键
        question_id  BIGINT(24) UNSIGNED 答案id    自增主键，也用于作为搜索索引
        appid   INT(11) UNSIGNED    应用id
        question    VARCHAR(128)    请求的问题
        question_type   TINYINT(4) UNSIGNED 问题类型
        answer  VARCHAR(1024)   回答的答案
        create_date DATETIME    该条记录的生成时间;默认值为 '1970-01-01 08:00:00'
        update_date DATETIME    该条记录的更新时间;默认值为 '1970-01-01 08:00:00'

                  机器人搜索引擎问题列表。
          数据类型： hash
          Key:    1:19:${appid}其中第一位1表示hash结构，第二位19表示智能机器人引擎问题列表,第三位${appid}表示应用id；
        Field：
        ${index_id}  Record
        ${index_id}  Record
        ${index_id}  Record
        Record顺序 {index_id(0) standard_question_id(1) appid(2) question_attribute(3) question_type(4) question(5) answer(6) create_date(7) update_date(8)}
     * */
    char szRedisKey[32];
    snprintf(szRedisKey,sizeof(szRedisKey),"%u:%u:%u",REDIS_T_HASH, IM_DATA_AI_ROBOT_QUESTION_LIST,
                    appid);
    net::MemOperator oMemOper(
                    0,
                    "tb_ai_engine_question",
                    DataMem::MemOperate::DbOperate::SELECT,
                    szRedisKey,
                    "HMSET",
                    "HGETALL");
    oMemOper.AddDbField("index_id");//0
    oMemOper.AddDbField("standard_question_id");//1
    oMemOper.AddDbField("appid");//2
    oMemOper.AddDbField("question_attribute");//3
    oMemOper.AddDbField("question_type");//4
    oMemOper.AddDbField("question");//5
    oMemOper.AddDbField("answer");//6
    oMemOper.AddDbField("create_date");//7
    oMemOper.AddDbField("update_date");//8

    oMemOper.AddCondition(DataMem::MemOperate::DbOperate::Condition::EQ,
                    "appid",
                    appid);

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
    m_status = eAiEngineWordsLoadQuestionStatus_load_questions;
    return net::STATUS_CMD_RUNNING;
}

net::E_CMD_STATUS StepLoadAiEngineQuestions::Callback(
                const net::tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody, void* data)
{
    LOG4_TRACE("%s() StepLoadAiEngineQuestions::Callback",__FUNCTION__);
    if (net::CMD_RSP_SYS_ERROR == oInMsgHead.cmd())
    {
        LOG4_ERROR("%s() system response error",__FUNCTION__);
        return net::STATUS_CMD_FAULT;
    }
    DataMem::MemRsp oMemRsp;
    if (!oMemRsp.ParseFromString(oInMsgBody.body()))
    {
        LOG4_WARN("%s() StepLoadAiEngineQuestions::Callback,oMemRsp format error!",__FUNCTION__);
        return net::STATUS_CMD_FAULT;
    }
    if (oMemRsp.err_no())
    {
        if (oMemRsp.err_msg().size() > 0)
        {
            LOG4_ERROR("%s() dataproxy error %d: %s!",__FUNCTION__,
                            oMemRsp.err_no(), oMemRsp.err_msg().c_str());
        }
        else
        {
            LOG4_ERROR("%s() dataproxy error %d!",__FUNCTION__,oMemRsp.err_no());
        }
        return (net::STATUS_CMD_FAULT);
    }
    if(eAiEngineWordsLoadQuestionStatus_load_app == m_status)
    {//加载app
        int iRecSize = oMemRsp.record_data_size();
        LOG4_TRACE("%s() eAiEngineWordsLoadQuestionStatus_load_app iRecSize(%d)",__FUNCTION__,iRecSize);
        if(iRecSize > 0)
        {
            m_appidlist.clear();
            /*
               app域名表tb_app_domain
                                            所包含数据库表及分表策略，如下
                                            序号  表名  表信息描述   分表策略
                1   tb_app_domain   app域名表  不分表
                                            对用户账号表的字段描述见如下表（表3-1-1）：
                                            序号  字段  类型  描述  是否主键
                1   domain  VARCHAR(128)        域名  主键
                2   appid   INT(11) UNSIGNED    应用id,服务器逻辑使用
                3   appguid VARCHAR(64)         应用appguid，对外对接使用
                4   appsecret   VARCHAR(64)     应用secret，对外对接使用
                5   app_name    VARCHAR(128)    应用名,如你我金融
                6   create_time DATETIME        创建时间，默认值为 '1970-01-01 08:00:00'
                7   update_time DATETIME        更新时间，默认值为 '1970-01-01 08:00:00'
             * */
            for(int i = 0;i < iRecSize;++i)
            {
                if (oMemRsp.record_data(i).field_info_size() == 7)
                {
                    uint32 appid = strtoul(oMemRsp.record_data(i).field_info(1).col_value().c_str(),NULL,10);
                    if (appid > 0)
                    {
                        m_appidlist.push_back(appid);
                        LOG4_TRACE("%s() eAiEngineWordsLoadQuestionStatus_load_app appid(%u)",__FUNCTION__,appid);
                    }
                    else
                    {
                        LOG4_WARN("%s() invalid oMemRsp.record_data(i).field_info(1):%s",
                                        __FUNCTION__,oMemRsp.record_data(i).field_info(1).col_value().c_str());
                    }
                }
                else
                {
                    LOG4_WARN("%s() oMemRsp.record_data(i).field_info_size() != 7 i(%d)",__FUNCTION__,i);
                    break;
                }
            }
        }
        m_status = eAiEngineWordsLoadQuestionStatus_load_app_ok;
        return Emit(ERR_OK);
    }
    else if (eAiEngineWordsLoadQuestionStatus_load_questions == m_status)
    {
        uint32 appid = m_appidlist.front();
        if (appid <= 0)
        {
            LOG4_WARN("%s() eAiEngineWordsLoadQuestionStatus_load_questions appid(%u)",__FUNCTION__,appid);
            return (net::STATUS_CMD_FAULT);
        }
        int iRecSize = oMemRsp.record_data_size();
        LOG4_TRACE("%s() eAiEngineWordsLoadQuestionStatus_load_questions appid(%u) iRecSize(%d) oMemRsp(%s)",
                        __FUNCTION__,appid,iRecSize,oMemRsp.DebugString().c_str());
        if (iRecSize > 0)
        {
            const DataMem::Record& oRecordData = oMemRsp.record_data(0);
            for(int i = 0;i < oRecordData.field_info_size();++i)
            {
                DataMem::Record oRecord;
                if(oRecord.ParseFromString(oRecordData.field_info(i).col_value()))
                {
                    if (oRecord.field_info_size() == 9)
                    {
                        /*
                                                                          机器人搜索引擎问题列表。
                                                                          数据类型： hash
                              Key:    1:19:${appid}其中第一位1表示hash结构，第二位19表示智能机器人引擎问题列表,第三位${appid}表示应用id；
                            Field：
                            ${index_id}     Record
                            ${index_id}     Record
                            ${index_id}     Record
                            Record顺序 {index_id(0) standard_question_id(1) appid(2) question_attribute(3) question_type(4) question(5) answer(6) create_date(7) update_date(8)}
                         * */
                        ai_engine_question aiEngineQuestion;
                        {
                            aiEngineQuestion.index_id = ::strtoull(oRecord.field_info(0).col_value().c_str(),NULL,10);//value index_id
                            aiEngineQuestion.standard_question_id = ::strtoull(oRecord.field_info(1).col_value().c_str(),NULL,10);//value standard_question_id
                            aiEngineQuestion.appid = ::strtoul(oRecord.field_info(2).col_value().c_str(),NULL,10);//value appid
                            aiEngineQuestion.question_attribute = ::strtoul(oRecord.field_info(3).col_value().c_str(),NULL,10);//value appid
                            aiEngineQuestion.question_type = ::strtoul(oRecord.field_info(4).col_value().c_str(),NULL,10);//value question_type
                            aiEngineQuestion.question = oRecord.field_info(5).col_value();//value question
                            aiEngineQuestion.answer = oRecord.field_info(6).col_value();//value answer
                            aiEngineQuestion.create_date = lnet::TimeStr2time_t(oRecord.field_info(7).col_value());//value create_date
                            aiEngineQuestion.update_date = lnet::TimeStr2time_t(oRecord.field_info(8).col_value());//value update_date
                            LOG4_TRACE("%s() appid(%u) oRecord(%s)",__FUNCTION__,appid,oRecord.DebugString().c_str());
                        }
                        if (m_pSessionAiEngine)
                        {
                            m_pSessionAiEngine->AddAiEngineQuestionAppend(aiEngineQuestion);
                        }
                    }
                    else
                    {
                        LOG4_WARN("%s() oRecord.field_info_size() != 9",__FUNCTION__);
                    }
                }
                else
                {
                    LOG4_WARN("%s() ParseFromString failed",__FUNCTION__);
                }
            }
            if (oMemRsp.curcount() < oMemRsp.totalcount())
            {
                LOG4_TRACE("%s() iRecSize(%d),oMemRsp.curcount(%d) < oMemRsp.totalcount(%d) m_status(%d)",
                                __FUNCTION__,iRecSize,oMemRsp.curcount(),oMemRsp.totalcount(),m_status);
                return (net::STATUS_CMD_RUNNING);
            }
            else
            {
                LOG4_TRACE("%s() iRecSize(%d) oMemRsp.curcount(%d) == oMemRsp.totalcount(%d)",
                                __FUNCTION__,iRecSize,oMemRsp.curcount(),oMemRsp.totalcount());
            }
        }
        m_status = eAiEngineWordsLoadQuestionStatus_load_questions_ok;
        return Emit(ERR_OK);
    }
    LOG4_TRACE("%s() invalid status(%u)",__FUNCTION__,m_status);
    return (net::STATUS_CMD_RUNNING);
}

net::E_CMD_STATUS StepLoadAiEngineQuestions::Timeout()
{
	LOG4_TRACE("%s()", __FUNCTION__);
    if (m_timeout < 3)
    {
        ++m_timeout;
        LOG4_TRACE("%s() StepLoadAiEngineQuestions::Timeout(%u)",
                        __FUNCTION__,m_timeout);
        return (net::STATUS_CMD_RUNNING);
    }
    else
    {
        LOG4_WARN("%s() StepLoadAiEngineQuestions::Timeout(%u)",
                        __FUNCTION__,m_timeout);
        return (net::STATUS_CMD_FAULT);
    }
}

} /* namespace im */
