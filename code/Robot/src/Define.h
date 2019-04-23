/*******************************************************************************
* Project:  LogicServer
* @file     define.h
* @brief 
* @author   cjy
* @date:    2015年10月23日
* @note
* Modify history:
******************************************************************************/
#ifndef SRC_ROBOTSERVER_DEFINE_H_
#define SRC_ROBOTSERVER_DEFINE_H_
#include <time.h>
#include <sys/time.h>
#include <string>
#include <string.h>
#include <list>
#include "enum.pb.h"
#include "util/json/CJsonObject.hpp"
#include "storage/MemOperator.hpp"
#include "storage/dataproxy.pb.h"
#include "behaviour_common.pb.h"
#include "OssError.hpp"
#include "RobotError.h"
#include "RobotErrorMapping.h"
#include "RobotTableName.h"
#include "RobotRedisProto.h"
#include "algorithm/Levenshtein.hpp"

namespace robot
{
typedef net::uint8 uint8;
typedef net::uint16 uint16;
typedef net::uint32 uint32;
typedef net::uint64 uint64;

#define SAFE_DELETE(x) { if (x) delete x; x = NULL; }
#define SAFE_ARRAY_DELETE(x) { if (x) delete [] x; x = NULL; }

enum RobotMsgType
{
    eRobotMsgType_text = 1,
};

struct ai_engine_question
{
    ai_engine_question()
    {
        index_id = 0;
        standard_question_id = 0;
        appid = 0;
        question_attribute = 0;
        question_type = 0;
        create_date = 0;
        update_date = 0;

        uiMatchWordsCounter = 0;
        nLevenshtein = 0;
    }
    ai_engine_question(const ai_engine_question& oQuestion)
    {
        index_id = oQuestion.index_id;
        standard_question_id = oQuestion.standard_question_id;
        appid = oQuestion.appid;
        question_attribute = oQuestion.question_attribute;
        question_type = oQuestion.question_type;
        question = oQuestion.question;
        answer = oQuestion.answer;
        create_date = oQuestion.create_date;
        update_date = oQuestion.update_date;

        strReqQuestion = oQuestion.strReqQuestion;
        uiMatchWordsCounter = oQuestion.uiMatchWordsCounter;
        nLevenshtein = oQuestion.nLevenshtein;
    }
    const ai_engine_question& operator = (const ai_engine_question& oQuestion)
    {
        index_id = oQuestion.index_id;
        standard_question_id = oQuestion.standard_question_id;
        appid = oQuestion.appid;
        question_attribute = oQuestion.question_attribute;
        question_type = oQuestion.question_type;
        question = oQuestion.question;
        answer = oQuestion.answer;
        create_date = oQuestion.create_date;
        update_date = oQuestion.update_date;

        strReqQuestion = oQuestion.strReqQuestion;
        uiMatchWordsCounter = oQuestion.uiMatchWordsCounter;
        nLevenshtein = oQuestion.nLevenshtein;
        return *this;
    }
    bool operator < (const ai_engine_question& oQuestion) const
    {
        if (uiMatchWordsCounter == oQuestion.uiMatchWordsCounter)//相同匹配单词数的
        {
            //距离小的排前面
            return nLevenshtein < oQuestion.nLevenshtein;
        }
        else
        {
            //匹配单词多的排前面
            return uiMatchWordsCounter > oQuestion.uiMatchWordsCounter;
        }
    }
    void CalLevenshtein()
    {
        nLevenshtein = lnet::Levenshtein(question,strReqQuestion);
    }
    uint32 GetLevenshtein()const
    {
        return nLevenshtein;
    }
    uint64 index_id;
    uint64 standard_question_id;
    uint32 appid;
    uint32 question_attribute;
    uint32 question_type;
    std::string question;
    std::string answer;
    uint32 create_date;
    uint32 update_date;

    //在计算相关度时才有
    std::string strReqQuestion;//请求的问题，作为相关度计算依据
    uint32 uiMatchWordsCounter;//匹配单词数
    uint32 nLevenshtein;//与请求问题编辑距离
};

struct word_type_match
{
    std::vector<std::string> words;//匹配分词
    word_type_match()
    {
    }
    word_type_match(const word_type_match& oWordType)
    {
        words = oWordType.words;
    }
    const word_type_match& operator = (const word_type_match& oWordType)
    {
        words = oWordType.words;
        return *this;
    }
    bool operator < (const word_type_match& oWordType) const
    {
        return words.size() < oWordType.words.size();
    }
};

struct classify_question_type
{
    std::string strReqQuestionType;//问题分类
    std::vector<std::string> words;//匹配分词
    classify_question_type()
    {
    }
    classify_question_type(const classify_question_type& oQuestion)
    {
        strReqQuestionType = oQuestion.strReqQuestionType;
        words = oQuestion.words;
    }
    const classify_question_type& operator = (const classify_question_type& oQuestion)
    {
        strReqQuestionType = oQuestion.strReqQuestionType;
        words = oQuestion.words;
        return *this;
    }
    bool operator < (const classify_question_type& oQuestion) const
    {
        return words.size() > oQuestion.words.size();//匹配词数大的排前面
    }
};

struct session_messages_log
{
    behaviour_common::tb_session_messages_log message;//最小编辑距离在ESAGENTS 计算，不在ROBOT中计算
    session_messages_log()
    {
    }
    session_messages_log(const session_messages_log& msg):message(msg.message)
    {
    }
    session_messages_log(const behaviour_common::tb_session_messages_log& msg):message(msg)
    {
    }
    const session_messages_log& operator = (const session_messages_log& msg)
    {
        message = msg.message;
        return *this;
    }
    bool operator < (const session_messages_log& messages_log) const
    {
        if (message.max_matchwordcounter() == messages_log.message.max_matchwordcounter())//问题匹配词一样的距离小的排前面
        {
            return message.min_levenshtein() < messages_log.message.min_levenshtein();
        }
        return message.max_matchwordcounter() > messages_log.message.max_matchwordcounter();//单词最大匹配数多的排前面
    }
    uint32 GetLevenshtein()const
    {
        return message.min_levenshtein();
    }
};

//函数运行时间计算类
class CustomClock
{
public:
    CustomClock(const char* desc,log4cplus::Logger logger)
    {
        if (!boStart)
        {
            m_desc = desc;
            m_logger = logger;
            StartClock();
        }
    }
    ~CustomClock()
    {
        EndClock();
    }
    void StartClock()
    {
        gettimeofday(&m_cBeginClock,NULL);
    }
    void EndClock()
    {
        if (boStart)
        {
            boStart = false;
            gettimeofday(&m_cEndClock,NULL);
            float useTime=1000000*(m_cEndClock.tv_sec-m_cBeginClock.tv_sec)+
                            m_cEndClock.tv_usec-m_cBeginClock.tv_usec;
            useTime/=1000;
            LOG4CPLUS_INFO_FMT(m_logger,"%s() CustomClock %s use time(%lf) ms",__FUNCTION__,m_desc,useTime);
        }
    }
    bool boStart;
    timeval m_cBeginClock;
    timeval m_cEndClock;
    const char* m_desc;
    log4cplus::Logger m_logger;
};

}
#endif /* SRC_LOGICSERVER_DEFINE_H_ */
