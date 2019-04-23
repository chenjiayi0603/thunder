/*
 * RobotSession.h
 *
 *  Created on: 2015年10月21日
 *      Author: chen
 */
#ifndef CODE_WEBSERVER_SRC_WEBSESSION_H_
#define CODE_WEBSERVER_SRC_WEBSESSION_H_
#include <string>
#include <map>
#include <sys/time.h>
#include "sphinxc/sphinxclient.h"
#include "RobotErrorMapping.h"
#include "dbi/MysqlDbi.hpp"
#include "session/Session.hpp"
#include "OssDefine.hpp"
#include "RobotError.h"
#include "OssError.hpp"
#include "step/Step.hpp"
#include "cmd/Cmd.hpp"
#include "StepLoadAiEngineQuestions.hpp"
#include "Define.h"

#define ROBOT_SESSIN_ID (1000)

namespace robot
{

class RobotSession: public oss::Session
{
public:
    RobotSession(double session_timeout = 5.0)
                    : oss::Session(ROBOT_SESSIN_ID, session_timeout,"robot::RobotSession"),
                      boInit(false),boTest(false),boLoadAiEngineQuestions(false),m_LastBuildAiEngineQuestionsIndexTime(0),
                      m_LastLoadAiEngineQuestionsTime(0),m_currenttime(0),m_nSphinxPort(0),m_nSphinxAnswerMatchMode(0),
                      m_nSphinxAnswerRankMode(0),m_Client(NULL),m_uiBuildQuestionIndexInterval(0),m_uiLoadBulkQuestionInterval(0)
    {
    }
    virtual ~RobotSession()
    {
    }
    bool Init(const loss::CJsonObject& conf);
    oss::E_CMD_STATUS Timeout()
    {
        setCurrentTime();
        LoadAiEngine();
        BuildAiQuestionIndex();
        return oss::STATUS_CMD_RUNNING;
    }
    void SetConfigPath(const std::string &configpath)
    {
        m_strConfigPath = configpath;
    }
    void setCurrentTime()
    {
        m_currenttime = ::time(NULL);
    }
    uint32 getCurrentTime()
    {
        return m_currenttime;
    }
    bool TestSphinx(bool boForce = false);

    bool BuildAiQuestionIndex();

    bool LoadAiEngine();

    sphinx_client * GetSphinxClient();

    bool QuerySphinxAnswer(const std::string& strQuery,const char* rankfield,const char * filterAttr,
                    int filterValue,uint64 &nIndexid,std::string &strAnswer);
    const std::string& GetDefaultAnswer()const{return m_strDefaultAnswer;}
    const std::string& GetAiQuestionGuide()const{return m_strAiQuestionGuide;}
    void SkipNonsenseLetters(std::string& word)const
    {
        for(uint32 i = 0 ;i < m_ignoreCharsVec.size(); ++i)
        {
            RemoveFlag(word,m_ignoreCharsVec[i]);
        }
    }
    void ResetAiEngineQuestions(){boLoadAiEngineQuestions = false;}
private:
    sphinx_result * SphinxQuery(const char * query,const char *index,uint32 mode,const char* rankfield,const char * filterAttr,int filterValue);
    sphinx_result * SphinxModeQuery(const char * query, const char *index,uint32 matchMode,
                    uint32 sortMode,uint32 rankMode,const char* rankfield,const char * filterAttr,int filterValue);
//    bool GetAiAnswer(std::string &answer,int answer_id);
    void RemoveFlag(std::string &str, char flag)const;
    //去掉无意义字符
    bool boInit;
    bool boTest;
    bool boLoadAiEngineQuestions;
    uint32 m_LastBuildAiEngineQuestionsIndexTime; //最后重建AI问题索引时间
    uint32 m_LastLoadAiEngineQuestionsTime; //最后加载AI问题时间
    uint32 m_currenttime; //当前时间
    std::string m_strConfigPath;
    std::string m_strSphinxAnswerMainIndex;
    std::string m_strSphinxHost;
    int m_nSphinxPort;
    //智能答案匹配模式 1:短语匹配 2:所有词匹配   4:任意匹配 8:拓展匹配模式
    enum eSphinxAnswerMatchMode
    {
        eSphinxAnswerMatchMode_Phrase   = 0x0001,//SPH_MATCH_PHRASE
        eSphinxAnswerMatchMode_All      = 0x0002,//SPH_MATCH_ALL
        eSphinxAnswerMatchMode_Any      = 0x0004,//SPH_MATCH_ANY
        eSphinxAnswerMatchMode_Extend2  = 0x0008,//SPH_MATCH_EXTENDED2
    };
    uint32 m_nSphinxAnswerMatchMode;
    //智能答案Rank模式  SPH_RANK_PROXIMITY_BM25(0),SPH_RANK_BM25(1),
    //SPH_RANK_NONE(2),SPH_RANK_WORDCOUNT(3),SPH_RANK_PROXIMITY(4),SPH_RANK_MATCHANY(5),SPH_RANK_FIELDMASK(6),SPH_RANK_SPH04(7)
    uint32 m_nSphinxAnswerRankMode;
    sphinx_client * m_Client;
    std::vector<unsigned char> m_ignoreCharsVec;

    std::string m_strDefaultAnswer;

    std::string m_strSphinxAnswerTestQuestion;

    std::string m_strAiQuestionGuide;

    uint32 m_uiBuildQuestionIndexInterval;//建立问题索引时间间隔（有更新才会重建索引）
    uint32 m_uiLoadBulkQuestionInterval;//全量获取所有问题时间间隔
};

RobotSession* GetRobotSession(oss::OssLabor* pLabor,const std::string &configPath);

}
;

#endif /* CODE_WEBSERVER_SRC_WEBSESSION_H_ */
