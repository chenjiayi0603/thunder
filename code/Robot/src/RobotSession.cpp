/*
 * RobotSession.cpp
 *
 *  Created on: 2017年1月19日
 *      Author: chenjiayi
 */
#include "RobotSession.h"
#include "util/json/CJsonObject.hpp"
#include "sphinxc/sphinxTest.h"

namespace robot
{

bool RobotSession::Init(const lnet::CJsonObject& conf)
{
    if(boInit)
    {
        return true;
    }
    if (!conf.Get("sphinx_port", m_nSphinxPort))
    {
        LOG4CPLUS_INFO_FMT(GetLogger(),"failed to load conf for sphinx_port");
    }
    if (!conf.Get("sphinx_host", m_strSphinxHost))
    {
        LOG4CPLUS_INFO_FMT(GetLogger(),"failed to load conf for sphinx_host");
    }
    if (m_nSphinxPort > 0 && m_strSphinxHost.size() > 0)
    {
        if (!conf.Get("sphinx_answer_main_index", m_strSphinxAnswerMainIndex))
        {
            LOG4CPLUS_ERROR_FMT(GetLogger(),"failed to load conf for sphinx_answer_main_index");
            return false;
        }
        if (!conf.Get("sphinx_answer_match_mode", m_nSphinxAnswerMatchMode))
        {
            LOG4CPLUS_ERROR_FMT(GetLogger(),"failed to load conf for sphinx_answer_match_mode");
            return false;
        }
        if (!conf.Get("sphinx_answer_rank_mode", m_nSphinxAnswerRankMode))
        {
            LOG4CPLUS_ERROR_FMT(GetLogger(),"failed to load conf for sphinx_answer_rank_mode");
            return false;
        }
        if (!conf.Get("sphinx_answer_test_question", m_strSphinxAnswerTestQuestion))
        {
            LOG4CPLUS_ERROR_FMT(GetLogger(),"failed to load conf for sphinx_answer_test_question");
            return false;
        }
    }
    else
    {
        LOG4CPLUS_TRACE_FMT(GetLogger(),"%s() ignore sphinx engine",__FUNCTION__);
    }
    if (!conf.Get("default_answer", m_strDefaultAnswer))
    {
        m_strDefaultAnswer = "不好意思，不能理解你的意思。";
    }
    LOG4CPLUS_TRACE_FMT(GetLogger(),"%s() m_strDefaultAnswer(%s)",__FUNCTION__,m_strDefaultAnswer.c_str());
    if (!conf.Get("ai_question_guide", m_strAiQuestionGuide))
    {
        m_strAiQuestionGuide = "请问你问的是";
    }
    conf.Get("build_question_index_interval", m_uiBuildQuestionIndexInterval);
    conf.Get("load_bulk_question_interval", m_uiLoadBulkQuestionInterval);

    LOG4CPLUS_TRACE_FMT(GetLogger(),"%s() m_strAiQuestionGuide(%s)",__FUNCTION__,m_strAiQuestionGuide.c_str());
    {//忽略字符列表
        std::string ignore_chars;
        if(conf.Get("ignore_chars",ignore_chars))
        {
            RemoveFlag(ignore_chars,' ');
            int s = ignore_chars.length();
            char *tmpChars = new char[s + 1];
            snprintf(tmpChars,s + 1,ignore_chars.c_str());
            LOG4CPLUS_TRACE_FMT(GetLogger(),"ignore letters:%s",tmpChars);
            int j(0);
            int ascii(0);
            for(int i = 0;i <= s;)
            {
                if (',' == tmpChars[i])//逗号分隔
                {
                    tmpChars[i] = 0;
                    ascii = atoi(&tmpChars[j]);
                    ++i;
                    j = i;
                    LOG4CPLUS_TRACE_FMT(GetLogger(),"ignore letter:%u,%c",(unsigned char)ascii,(unsigned char)ascii);
                    m_ignoreCharsVec.push_back((unsigned char)ascii);
                }
                else if(0 == tmpChars[i])
                {
                    ascii = atoi(&tmpChars[j]);
                    LOG4CPLUS_TRACE_FMT(GetLogger(),"ignore letter:%u,%c",(unsigned char)ascii,(unsigned char)ascii);
                    m_ignoreCharsVec.push_back((unsigned char)ascii);
                    break;
                }
                else
                {
                    ++i;
                }
            }
            delete [] tmpChars;
        }
        else
        {
            m_ignoreCharsVec.push_back((unsigned char)' ');
        }
    }
    setCurrentTime();
    LOG4CPLUS_TRACE_FMT(GetLogger(),"init RobotSession ok");
    boInit = true;
    return true;
}

bool RobotSession::TestSphinx(bool boForce)
{
    if (!boForce)
    {
        if (!GetSphinxClient())
        {
            LOG4CPLUS_INFO_FMT(GetLogger(), "%s() GetSphinxClient failed",__FUNCTION__);
            return false;
        }
        if(boTest)
        {
            return true;
        }
        boTest = true;
    }
    LOG4CPLUS_INFO_FMT(GetLogger(), "%s() Test QuerySphinxAnswer",__FUNCTION__);
    timeval cBeginClock;
    gettimeofday(&cBeginClock,NULL);
    uint64 nQuestionid(0);
    std::string strAnswer;
    int nSearchCounter(300);
    int nSuccTimes(0);
    for(int i = 0;i < nSearchCounter;++i)
    {
        if(QuerySphinxAnswer(m_strSphinxAnswerTestQuestion,"question","appid",1,nQuestionid,strAnswer))
        {
            ++nSuccTimes;
        }
    }
    timeval cEndClock;
    gettimeofday(&cEndClock,NULL);
    float useTime=1000000*(cEndClock.tv_sec-cBeginClock.tv_sec)+
                    cEndClock.tv_usec-cBeginClock.tv_usec;
    useTime/=1000;
    if (nQuestionid > 0)
    {
        //TestSphinx() QuerySphinxAnswer found nQuestionid(11) for strQuery(你我) useTime(15043.595703)ms try nSearchCounter(300),succ (300) times
        //约50ms/req
        //多客户端测试约190qps（经多工作者测试）
        LOG4CPLUS_INFO_FMT(GetLogger(),"%s() QuerySphinxAnswer found nQuestionid(%llu) for strQuery(%s) useTime(%lf)ms try nSearchCounter(%d),succ (%d) times",
                        __FUNCTION__,nQuestionid,m_strSphinxAnswerTestQuestion.c_str(),useTime,nSearchCounter,nSuccTimes);
    }
    else
    {
        LOG4CPLUS_INFO_FMT(GetLogger(),"%s() QuerySphinxAnswer not found for strQuery(%s) useTime(%lf)ms try nSearchCounter(%d),succ (%d) times",
                            __FUNCTION__,m_strSphinxAnswerTestQuestion.c_str(),useTime,nSearchCounter,nSuccTimes);
    }
    return true;
}

bool RobotSession::BuildAiQuestionIndex()
{
    LOG4CPLUS_TRACE_FMT(GetLogger(), "%s()", __FUNCTION__);
    bool boToBuild(false);
    if (m_uiBuildQuestionIndexInterval > 0)
    {
        if (m_LastBuildAiEngineQuestionsIndexTime + m_uiBuildQuestionIndexInterval < m_currenttime)
        {
            boToBuild = true;
        }
    }
    if (boToBuild)
    {
        SessionAiEngine* pSessionAiEngine = GetSessionAiEngine(GetLabor());
        if (pSessionAiEngine)
        {
            m_LastBuildAiEngineQuestionsIndexTime = m_currenttime;
            pSessionAiEngine->Build();
        }
    }
    return boToBuild;
}

bool RobotSession::LoadAiEngine()
{
    LOG4CPLUS_TRACE_FMT(GetLogger(), "%s()", __FUNCTION__);
    bool boToLoad(false);
    bool boForceLoadWords(false);
    if (!boLoadAiEngineQuestions)
    {
        boToLoad = true;
        boForceLoadWords = true;
    }
    if (m_uiLoadBulkQuestionInterval > 0)
    {
        if (m_LastLoadAiEngineQuestionsTime + m_uiLoadBulkQuestionInterval < m_currenttime)
        {
            boToLoad = true;
        }
    }
    if (!boToLoad)
    {
        return true;
    }
    LOG4CPLUS_TRACE_FMT(GetLogger(), "%s() begin loadding,m_LastLoadAiEngineQuestionsTime(%u) m_currenttime(%u) m_uiLoadBulkQuestionInterval(%u)",
                    __FUNCTION__,m_LastLoadAiEngineQuestionsTime,m_currenttime,m_uiLoadBulkQuestionInterval);
    boLoadAiEngineQuestions = true;
    m_LastLoadAiEngineQuestionsTime = m_currenttime;

    StepLoadAiEngineQuestions* pStepLoadAiEngineQuestions = new StepLoadAiEngineQuestions(boForceLoadWords);
    if (pStepLoadAiEngineQuestions == NULL)
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(), "error %d: new StepLoadAiEngineQuestions() error!", net::ERR_NEW);
        return false;
    }
    if (!RegisterCallback(pStepLoadAiEngineQuestions))
    {
        delete pStepLoadAiEngineQuestions;
        pStepLoadAiEngineQuestions = NULL;
        LOG4CPLUS_ERROR_FMT(GetLogger(), "failed to RegisterCallback(pStepLoadAiEngineQuestions)");
        return false;
    }
    if (net::STATUS_CMD_RUNNING != pStepLoadAiEngineQuestions->Emit(ERR_OK))
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(), "failed to pStepLoadAiEngineQuestions Emit");
        DeleteCallback(pStepLoadAiEngineQuestions);
        return(false);
    }
    LOG4CPLUS_TRACE_FMT(GetLogger(), "%s() pStepLoadAiEngineQuestions emit ok!", __FUNCTION__);
    return true;
}

bool RobotSession::QuerySphinxAnswer(const std::string& strQuery,const char* rankfield,const char * filterAttr,
                int filterValue,uint64 &nIndexid,std::string &strAnswer)
{
    nIndexid = 0;
    std::string strFilterQuery = strQuery;
    SkipNonsenseLetters(strFilterQuery);
    if(strFilterQuery.empty())
    {
        LOG4_WARN("empty str for query");
        return false;
    }
    const char* query = strFilterQuery.c_str();
    const char *index = m_strSphinxAnswerMainIndex.c_str();
    sphinx_result * res = SphinxQuery(query,index,m_nSphinxAnswerMatchMode,rankfield,filterAttr,filterValue);
    if(!res)
    {
        LOG4_WARN("failed to get res for query:%s,index:%s",query,index);
        return false;
    }
    {//组合答案
        LOG4_TRACE( "GetSphinxPhraseClient Matches:%d" ,res->num_matches);
        //  Matches:
        //  ../../src/sphinxTest.h(103):1. doc_id=3, weight=2, res=答案1, req_type=123, create_date=1479780177, update_date=1479780177
        if (res->num_matches <= 0)
        {
            LOG4_TRACE("no matches");
            return false;
        }
        char temp[1024];
        int tempLen = 0;
        int j, k, mva_len;
        unsigned int * mva;
        for (int i=0; i<res->num_matches; i++ )//结果数
        {
            //sphinx_result -> CJsonObject
            lnet::CJsonObject jObjJson;
            jObjJson.Add("id",sphinx_get_id ( res, i ));
            jObjJson.Add("weight",sphinx_get_weight ( res, i));
            LOG4_TRACE("res(id:%s) num_attrs:%d",jObjJson("id").c_str(),res->num_attrs);
            for ( j=0; j<res->num_attrs; j++ )
            {
                snprintf (temp,sizeof(temp)-1,", %s=", res->attr_names[j]);
                switch ( res->attr_types[j] )
                {
                    case SPH_ATTR_MULTI64://多值字段
                    case SPH_ATTR_MULTI:
                    {
                        mva = sphinx_get_mva ( res, i, j );
                        mva_len = *mva++;
                        tempLen = 0;
                        tempLen += snprintf (temp + tempLen,sizeof(temp)-1,"(");
                        for ( k=0; k<mva_len; k++ )
                        {
                            snprintf (temp  + tempLen,sizeof(temp)-1,
                              k ? ",%u" : "%u", ( res->attr_types[j]==SPH_ATTR_MULTI ? mva[k] : (unsigned int)sphinx_get_mva64_value ( mva, k ) ));
                        }
                        snprintf (temp + tempLen,sizeof(temp)-1,")");
                        jObjJson.Add(res->attr_names[j],temp);
                    }
                    break;
                    case SPH_ATTR_FLOAT:
                    {
                        jObjJson.Add(res->attr_names[j],sphinx_get_float ( res, i, j ));
                    }break;
                    case SPH_ATTR_STRING:
                    {
                        jObjJson.Add(res->attr_names[j],sphinx_get_string ( res, i, j ));
                        LOG4_TRACE("%s:%s",res->attr_names[j],sphinx_get_string ( res, i, j ));
                    }break;
                    default:
                    {
                        jObjJson.Add(res->attr_names[j],(unsigned int)sphinx_get_int ( res, i, j ));
                        LOG4_TRACE("%s:%u",res->attr_names[j],(unsigned int)sphinx_get_int ( res, i, j ));
                    }break;
                }
            }
            {
                LOG4_TRACE("jObjJson result:%s",jObjJson.ToFormattedString().c_str());
                if(jObjJson.Get("id",nIndexid))
                {
                    LOG4_TRACE("answer id:%llu",nIndexid);
                    jObjJson.Get("answer",strAnswer);
                    break;//只返回第一个答案
                }
            }
        }
    }
    if(nIndexid)
    {
        LOG4_TRACE("found nIndexid:%llu,strAnswer(%s)",nIndexid,strAnswer.c_str());
        return true;
    }
    LOG4_TRACE("not found nIndexid");
    return false;
}

//bool RobotSession::GetAiAnswer(std::string &answer,int answer_id)
//{
//    //查询数据库
//    char szSql[128];
//    snprintf(szSql, sizeof(szSql) - 1, "select * from tb_answer where id = %d", answer_id);
//    LOG4_TRACE(szSql);
//    lnet::T_vecResultSet vecRes;
//    if (0 != m_pMysqlDbi->ExecSql(szSql, vecRes))
//    {
//        LOG4_ERROR("loadNodeTypes error,%d:%s",
//                        m_pMysqlDbi->GetErrno(),
//                        m_pMysqlDbi->GetError().c_str());
//        return (false);
//    }
//    if (vecRes.empty())
//    {
//        LOG4_ERROR("select * from tb_answer empty,%d:%s",
//                        m_pMysqlDbi->GetErrno(),
//                        m_pMysqlDbi->GetError().c_str());
//        return (false);
//    }
//    int nResLen(0);
//    char tempRes[1024];
//    for (lnet::T_vecResultSet::iterator it = vecRes.begin(); it != vecRes.end();it = vecRes.end())//只需要一个结果
//    {
//        lnet::T_mapRow& valmap = *it;
//        //req
//        lnet::T_mapRow::iterator mapit = valmap.find("req");
//        if (valmap.end() == mapit)
//        {
//            LOG4_ERROR("failed to get segment \"req\"");
//            return false;
//        }
//        nResLen += snprintf(tempRes + nResLen,sizeof(tempRes)-1,"%s",valmap["req"].c_str());
//        mapit = valmap.find("res");
//        if (valmap.end() == mapit)
//        {
//            LOG4_ERROR("failed to get segment \"res\"");
//            return false;
//        }
//        nResLen += snprintf(tempRes + nResLen,sizeof(tempRes)-1,":\n%s",valmap["res"].c_str());
//        LOG4_TRACE("tempRes:%s",tempRes);
//    }
//    answer.assign(tempRes,nResLen);
//    return true;
//}

sphinx_client * RobotSession::GetSphinxClient()
{
    if(!m_Client)
    {
        LOG4_TRACE("%s() sphinx_create SphinxHost(%s) nSphinxPort(%d)",__FUNCTION__,m_strSphinxHost.c_str(), m_nSphinxPort);
        if (m_strSphinxHost.size() > 0 && m_nSphinxPort > 0)
        {
            m_Client = sphinx_create ( SPH_TRUE );
            sphinx_set_server ( m_Client, m_strSphinxHost.c_str(), m_nSphinxPort );
            //默认方式
            //拓展匹配模式--使用排序模式:SPH_MATCH_EXTENDED2
            //排序方式--按相关度降序排列:SPH_SORT_RELEVANCE
            //排序方式：SPH_RANK_PROXIMITY_BM25
            //具体查询时再设置具体匹配模式
        }
    }
    return m_Client;
}

sphinx_result * RobotSession::SphinxQuery(const char * query,const char *index,uint32 matchMode,
                const char* rankfield,const char * filterAttr,int filterValue)
{
    sphinx_result * res(NULL);
    if(matchMode & eSphinxAnswerMatchMode_Phrase)//短语匹配模式
    {
        LOG4_TRACE("eSphinxAnswerMatchMode_Phrase for query:%s,index:%s",query,index);
        res = SphinxModeQuery(query, index,SPH_MATCH_PHRASE,SPH_SORT_RELEVANCE,SPH_RANK_DEFAULT,rankfield,filterAttr,filterValue);
    }
    else if (matchMode & eSphinxAnswerMatchMode_All)//全词匹配模式
    {
        LOG4_TRACE("eSphinxAnswerMatchMode_All for query:%s,index:%s",query,index);
        res = SphinxModeQuery(query, index,SPH_MATCH_ALL,SPH_SORT_RELEVANCE,SPH_RANK_DEFAULT,rankfield,filterAttr,filterValue);
    }
    else if (matchMode & eSphinxAnswerMatchMode_Any)//任意词匹配模式
    {
        LOG4_TRACE("eSphinxAnswerMatchMode_Any for query:%s,index:%s",query,index);
        res = SphinxModeQuery(query, index,SPH_MATCH_ANY,SPH_SORT_RELEVANCE,SPH_RANK_DEFAULT,rankfield,filterAttr,filterValue);
    }
    else if (matchMode & eSphinxAnswerMatchMode_Extend2)//拓展匹配模式
    {
        LOG4_TRACE("eSphinxAnswerMatchMode_Extend2 for query:%s,index:%s",query,index);
        res = SphinxModeQuery(query, index,SPH_MATCH_EXTENDED2,SPH_SORT_RELEVANCE,m_nSphinxAnswerRankMode,rankfield,filterAttr,filterValue);
    }
    if ( !res )
    {
        LOG4_WARN("failed to get res for query:%s,index:%s",query,index);
        return NULL;
    }
    return res;
}

sphinx_result *RobotSession::SphinxModeQuery(const char * query, const char *index,uint32 matchMode,
                uint32 sortMode,uint32 rankMode,const char* rankfield,const char * filterAttr,int filterValue)
{
    LOG4_TRACE( "query(%s),index(%s),matchMode(%u),sortMode(%u),rankMode(%u),rankfield(%s),filterAttr(%s),filterValue(%d)",
                    query ,index,matchMode,sortMode,rankMode,rankfield,filterAttr,filterValue);
    sphinx_client* client = GetSphinxClient();
    if(!client)
    {
        LOG4_ERROR( "failed to GetSphinxClient", sphinx_error(client) );
        return NULL;
    }
    sphinx_set_match_mode ( client, matchMode );
    sphinx_set_sort_mode ( client, sortMode, rankfield );//目前只是按相关度排序
    sphinx_set_ranking_mode ( client, rankMode );
    if(rankfield)//设置排序字段权重
    {
        const char * field_names[1];
        int field_weights[1];
        field_names[0] = rankfield;
        field_weights[0] = 1000;
        sphinx_set_field_weights ( client, 1, field_names, field_weights );
    }
    {//查询索引的所有的属性
        char selectMode[64];
        snprintf(selectMode,sizeof(selectMode),"*");
        sphinx_set_select ( client, selectMode );
    }
    /*
                        设置查询数量
        $offset: 起始偏移量
        $limit: 从$offset开始 获取的数量控制
        $max_matches:控制服务端在当前请求中返回的数据的最大值
        $cutoff: 控制查询的数量限制(当sphinx 的查询超过$cutoff 就停止查询)
     * */
    sphinx_set_limits(client,0,10,10,0);
    if(filterAttr)//设置过滤字段
    {
        sphinx_int64_t filter_group = { filterValue };
        sphinx_add_filter ( client, filterAttr, 1, &filter_group, SPH_FALSE );
    }
    sphinx_result * res = sphinx_query ( client, query, index, NULL );
    if(filterAttr)
    {
        sphinx_reset_filters ( client );
    }
    if ( !res )
    {
        LOG4_ERROR( "query failed.error:%s,sphinx_warning:%s", sphinx_error(client),sphinx_error(client) );
        return NULL;
    }
    if (res->num_matches <= 0)
    {
        LOG4_WARN( "GetSphinxClient Matches none:%d" ,res->num_matches);
        return NULL;
    }
    return res;
}

void RobotSession::RemoveFlag(std::string &str, char flag)const
{
    std::string::iterator it = std::remove(str.begin(), str.end(), flag);
    str.erase(it, str.end());
}

RobotSession* GetRobotSession(net::OssLabor* pLabor,const std::string &configPath)
{
    RobotSession* pSess = (RobotSession*) pLabor->GetSession(ROBOT_SESSIN_ID,"robot::RobotSession");
    if (pSess)
    {
        return (pSess);
    }
    pSess = new RobotSession();
    if (pSess == NULL)
    {
        LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(), "error %d: new RobotSession() error!",
                        net::ERR_NEW);
        return (NULL);
    }
    lnet::CJsonObject   oCurrentConf;       ///< 当前加载的配置
    {
        //配置文件路径查找
        std::string strConfFile = configPath + std::string("/RobotCmd.json");
        LOG4CPLUS_TRACE_FMT(pLabor->GetLogger(), "CONF FILE = %s.", strConfFile.c_str());

        std::ifstream fin(strConfFile.c_str());
        //配置信息输入流
        if (fin.good())
        {
            //解析配置信息 JSON格式
            std::stringstream ssContent;
            ssContent << fin.rdbuf();
            if (!oCurrentConf.Parse(ssContent.str()))
            {
                //配置文件解析失败
                LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(),
                                "Read conf (%s) error,it's maybe not a json file!",
                                strConfFile.c_str());
                ssContent.str("");
                fin.close();
                return NULL;
            }
        }
        else
        {
            //配置信息流读取失败
            LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(), "Open conf (%s) error!",
                            strConfFile.c_str());
            return NULL;
        }
        pSess->SetConfigPath(configPath);
    }
    if (pLabor->RegisterCallback(pSess))
    {
        if (!pSess->Init(oCurrentConf))
        {
            pLabor->DeleteCallback(pSess);
            LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(), "RobotSession init error!");
            return (NULL);
        }
        LOG4CPLUS_TRACE_FMT(pLabor->GetLogger(), "register RobotSession ok!");
        return (pSess);
    }
    else
    {
        LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(), "register RobotSession error!");
        delete pSess;
        pSess = NULL;
    }
    return (NULL);
}


}
;
//name space robot
