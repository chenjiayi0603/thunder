/*******************************************************************************
 * Project:  LogicServer
 * @file     SessionSensitive.hpp
 * @brief 
 * @author   cjy
 * @date:    2016年6月16日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_SESSION_AI_ENGINE_WORDS_HPP_
#define SRC_SESSION_AI_ENGINE_WORDS_HPP_
#include "DFA.h"

//一页的个数
#define PAGE_NUM (10)

namespace robot
{

#define SESSION_AI_ENGINE_ID     (30000)

#define SESSION_AI_ENGINE_TIMER     (5)
//ulSessionId为appid
class SessionAiEngine: public oss::Timer
{
public:
    SessionAiEngine(uint32 ulAppid,ev_tstamp dSessionTimeout = SESSION_AI_ENGINE_TIMER);
    virtual ~SessionAiEngine();
    virtual oss::E_CMD_STATUS Timeout()
    {
        CheckRoutine();
        return(oss::STATUS_CMD_RUNNING);
    }
    /*
     * 初始化引擎单词库
     * @param logger 日志对象
     * */
    bool init(log4cplus::Logger logger);

    bool LoadAiEngineWords(bool boForce = false);
    /*
     * 建立状态机
     * */
    bool Build();

    bool BuildWordsFfa();

    bool BuildStopWordsFfa();

    bool BuildQuestionsMap();

	 /**
     * 获取语句中的引擎单词
     *  @param 需要检查的语句
     *  @param 返回的匹配的引擎单词列表
     *  return true:含引擎单词  false:不含引擎单词
     */
    bool SearchWords(const std::string& question,std::vector<std::string>& words)
    {
        std::string tmpQuestion = question;
        SkipIgnoreLetters(tmpQuestion);
        return m_AiEngineWordsDfa.SearchKeys(tmpQuestion,words);
    }


    bool IsLoadAiEngineWords()const{return boLoadAiEngineWords;}
    bool IsLoadAiEngineQuestions()const{return boLoadAiEngineQuestions;}
    //去掉无意义字符
    void SkipIgnoreLetters(std::string& word)const
    {
        if (word.size() > 0)
        {
            for(uint32 i = 0 ;i < m_ignoreCharsVec.size(); ++i)
            {
                RemoveFlag(word,m_ignoreCharsVec[i]);
            }
        }
    }
    void SkipFormatLetters(std::string& word)const
    {
        if (word.size() > 0)
        {
            RemoveFlag(word,'\"');
            RemoveFlag(word,' ');
        }
    }
    bool CheckLetters(const std::string& word)const
    {
        if(std::string::npos != word.find(','))
        {
            return false;
        }
        return true;
    }
    void CheckRoutine()
    {
//        m_CheckRoutineTime = ::time(NULL);
//        LOG4CPLUS_TRACE_FMT(GetLogger(),"CheckRoutine m_CheckRoutineTime(%llu)",
//                        m_CheckRoutineTime);
    }
private:
    //去掉符号
    inline void RemoveFlag(std::string &str, char flag)const
    {
        std::string::iterator it = std::remove(str.begin(), str.end(), flag);
        str.erase(it, str.end());
    }
public:
    bool AddAiEngineWords(const AppendAiEngineWords& oWord)
    {
        m_AiEngineWordsSet.insert(oWord.word);
        if (oWord.dir.size() > 0)
        {
            m_AiEngineWordsTypeMap.insert(std::make_pair(loss::HashStrToUint64(oWord.word.c_str(),oWord.word.size()),oWord.dir));
        }
//        LOG4CPLUS_TRACE_FMT(GetLogger(),"AddAiEngineKey ok,"
//                        "m_AiEngineKeysSet size(%u),word(%s)",m_AiEngineWordsSet.size(),
//                        word.c_str());
        return true;
    }
    bool DelAiEngineWords(const AppendAiEngineWords& oWord)
    {
        m_AiEngineWordsSet.erase(oWord.word);
        if (oWord.dir.size() > 0)
        {
            m_AiEngineWordsTypeMap.erase(loss::HashStrToUint64(oWord.word.c_str(),oWord.word.size()));
        }
//        LOG4CPLUS_TRACE_FMT(GetLogger(),"DelAiEngineTipsKey ok"
//                        "m_AiEngineKeysTipsFraudSet size(%u),word(%s)",
//                        m_AiEngineWordsSet.size(),word.c_str());
        return true;
    }
    bool AddAiEngineStopWords(const std::string& word)
    {
        m_AiEngineStopWordsSet.insert(word);
//        LOG4CPLUS_TRACE_FMT(GetLogger(),"AddAiEngineKey ok,"
//                        "m_AiEngineKeysSet size(%u),word(%s)",m_AiEngineWordsSet.size(),
//                        word.c_str());
        return true;
    }
    bool DelAiEngineStopWords(const std::string& word)
    {
        m_AiEngineStopWordsSet.erase(word);
//        LOG4CPLUS_TRACE_FMT(GetLogger(),"DelAiEngineTipsKey ok"
//                        "m_AiEngineKeysTipsFraudSet size(%u),word(%s)",
//                        m_AiEngineWordsSet.size(),word.c_str());
        return true;
    }
       /*
     * 获取引擎单词
     * @param keywordsList返回的引擎单词
     * @param page指定页数（从1开始）
     * @param type指定引擎单词类型(如果是eAiEngineType_default则两个都查)
     * @param secondaryType指定引擎单词二级类型
     * @param nCount指定数量，但不能超过一页个数PAGE_NUM（10）
     * */
    bool GetAiEngineKeys(std::vector<std::string>& keywordsList,int page,int nCount=PAGE_NUM)
    {
        keywordsList.clear();
        //提示引擎单词
        int nStart = (page -1) * PAGE_NUM;
        return GetWordsFromAiEngineSet(keywordsList,nStart,nCount,m_AiEngineWordsSet);;
    }
    int GetAiEngineWordsTotalNum()
    {
        return m_AiEngineWordsSet.size();
    }
private:
    bool GetWordsFromAiEngineSet(std::vector<std::string>& wordsList,int nStart,int nCount,
                    const std::set<std::string>& aiEngineWordsSet)
    {
        bool boGet(false);
        if((int)wordsList.size() < nCount)
        {
            std::set<std::string>::const_iterator cit = aiEngineWordsSet.begin();
            std::set<std::string>::const_iterator citEnd = aiEngineWordsSet.end();
            for(int n = 0;cit != citEnd;++cit,++n)
            {
                if(n >= nStart)
                {
                    wordsList.push_back(*cit);
                    boGet = true;
                    if((int)wordsList.size() >= nCount)
                    {
                        break;
                    }
                }
            }
        }
        return boGet;
    }
public:
    //判断是否指定类型引擎单词
    //引擎单词
    bool IsAiEngineWords(const std::string& word)const
    {
        if(m_AiEngineWordsDfa.IsKey(word))
        {
            LOG4CPLUS_TRACE_FMT(m_Logger,"IsAiEngineWords word(%s) yes",word.c_str());
            return true;
        }
        LOG4CPLUS_TRACE_FMT(m_Logger,"IsAiEngineWords word(%s) no",word.c_str());
        return false;
    }
    //引擎停用单词
    bool IsAiEngineStopWords(const std::string& word)const
    {
        if(m_AiEngineStopWordsDfa.IsKey(word))
        {
            return true;
        }
        return false;
    }
    //是否是所有的引擎单词中的一个(包括正在追加的)
    bool IsAiEngineAllWords(const std::string& word)const
    {
        if(IsAiEngineWords(word))
        {
            LOG4CPLUS_TRACE_FMT(m_Logger,"%s() %s is already AiEngine,in tree",
                                                __FUNCTION__,word.c_str());
            return true;
        }
        if(IsAddAiEngineWordsAppend(word))
        {
            LOG4CPLUS_TRACE_FMT(m_Logger,"%s() %s is already add_AiEngine,in appending list",
                            __FUNCTION__,word.c_str());
            return true;
        }
        return false;
    }
    bool CanDelAiEngineWords(const std::string& word)const
    {
        if(!IsAiEngineWords(word))//不是引擎单词的不能删除
        {
            LOG4CPLUS_WARN_FMT(m_Logger,"%s() %s is not a AiEngine,in tree",
                            __FUNCTION__,word.c_str());
            return false;
        }
        if(IsDelAiEngineWordsAppend(word))//已是追加删除引擎单词的不能再删除
        {
            LOG4CPLUS_WARN_FMT(m_Logger,"%s() %s is already del_AiEngine,in appending list",
                            __FUNCTION__,word.c_str());
            return false;
        }
        return true;
    }
    /*
     * 引擎单词
     * */
    //检查单词文件
    bool CheckWordsFiles();
    //读取单词文件
    bool ReadWordsFiles();

    //判断是否是增加引擎单词追加
    bool IsAddAiEngineWordsAppend(const std::string& word)const;
    //判断是否是减少引擎单词追加
    bool IsDelAiEngineWordsAppend(const std::string& word)const;
    //添加引擎单词追加
    void AddAiEngineWordsAppend(const std::string& word,const std::string& dir);
    //删除引擎单词追加
    void DelAiEngineWordsAppend(const std::string& word,const std::string& dir);
    //添加引擎单词追加
    void AddAiEngineStopWordsAppend(const std::string& word);
    //删除引擎单词追加
    void DelAiEngineStopWordsAppend(const std::string& word);
    //处理追加引擎单词
    bool HandleAppendAiEngineWords();
    //获取单词数量
    int GetAiEngineWordsSize()const{return m_AiEngineWordsSet.size();}
    //重置引擎单词
    void ResetEngineWords()
    {
        boLoadAiEngineWords = false;
        boBuildAiEngineWords = false;
        boBuildAiEngineStopWords = false;
        m_AiEngineWordsSet.clear();
        m_AiEngineWordsTypeMap.clear();
        m_AddWordsAppend.clear();
        m_DelWordsAppend.clear();
        m_AddStopWordsAppend.clear();
        m_DelStopWordsAppend.clear();
    }
    /*
     * 引擎问题
     * */
    //添加引擎问题追加
    void AddAiEngineQuestionAppend(const ai_engine_question& question);
    //删除引擎问题追加
    void DelAiEngineQuestionAppend(const ai_engine_question& question);

    //处理追加引擎问题
    bool HandleAppendAiEngineQuestion();
    //添加引擎问题处理
    bool AddAiEngineQuestion(const ai_engine_question& question);
    //删除引擎问题处理
    bool DelAiEngineQuestion(const ai_engine_question& question);

    //根据问题获取ai问题id列表，匹配ai问题的单词最多的ai问题排最前面
    bool GetAiQuestionListByReqQuestion(const std::string& strReqQuestion,uint32 appid,
                    std::vector<ai_engine_question>& aiQuestionVec);

    bool GetAiQuestionByAiQuestionID(uint64 aiQuestionID,ai_engine_question &question)const;

    bool GetBestAiQuestionByQuestion(const std::string& strReqQuestion,uint32 appid,ai_engine_question &question);

    //请求问题进行文本归类
    bool GetClassifyListByReqQuestion(const std::string& strReqQuestion,std::vector<classify_question_type> &oClassifyQuestionVec);

    //会话消息日志排序分类请求
    //limit 限制结果数 （0则不限制）
    bool SortSessionMessagesLog(std::vector<session_messages_log> &messageLogs,uint32 limit = 0);
    //测试
    bool test();
private:
    bool boInit;
    bool boLoadAiEngineWords;
    bool boLoadAiEngineQuestions;
    bool boBuildAiEngineWords;
    bool boBuildAiEngineStopWords;
    bool boBuildAiEngineQuestions;
    bool boTest;
    log4cplus::Logger m_Logger;
    oss::uint64 m_CheckRoutineTime;
    SessionAiEngineQuestionsStatus m_status;
    //忽略字符
    std::vector<unsigned char> m_ignoreCharsVec;

    /*
             引擎单词
     * */
    std::vector<std::string> m_WordsfilesNameVec;
    std::string m_ReadWordsFilePath;
    std::string m_strWordsFileExt;
    std::string m_StopWordsFileName;//停用词文件名
    int m_convertCode;
    std::string m_strSearchLine;
    uint32 m_nSearchLineCount;

    cppjieba::Jieba jieba;

    //词库集合数组
    std::set<std::string> m_AiEngineWordsSet;

    //分词类型
    typedef std::map<uint64,std::string> AiEngineWordsTypeMap;//wordid ->wordType(dir)
    typedef AiEngineWordsTypeMap::const_iterator AiEngineWordsTypeMapCIt;
    typedef AiEngineWordsTypeMap::iterator AiEngineWordsTypeMapIt;
    AiEngineWordsTypeMap m_AiEngineWordsTypeMap;//wordid ->wordType(dir)
    //词库停用词集合数组
    std::set<std::string> m_AiEngineStopWordsSet;
    //词库状态机
    DFA m_AiEngineWordsDfa;
    //词库停用词状态机
    DFA m_AiEngineStopWordsDfa;
    //增加引擎单词追加
    std::vector<AppendAiEngineWords> m_AddWordsAppend;
    //减少引擎单词追加
    std::vector<AppendAiEngineWords> m_DelWordsAppend;

    //增加引擎停用词追加
    std::vector<AppendAiEngineWords> m_AddStopWordsAppend;
    //减少引擎停用词追加
    std::vector<AppendAiEngineWords> m_DelStopWordsAppend;

    /*
                 引擎问题
     * */
    //引擎问题集合
    typedef std::map<uint64,ai_engine_question> AiEngineQuestionsMap;
    typedef std::map<uint64,ai_engine_question>::const_iterator AiEngineQuestionsMapCIt;
    typedef std::map<uint64,ai_engine_question>::iterator AiEngineQuestionsMapIt;

    AiEngineQuestionsMap m_AiEngineQuestionsMap;//ai question id -> ai_engine_question
    //增加引擎问题追加
    std::vector<AppendAiEngineQuestion> m_AddQuestionsAppend;
    //减少引擎问题追加
    std::vector<AppendAiEngineQuestion> m_DelQuestionsAppend;
    /*
                  问题检索 最大匹配方案
                    每个请求的问题搜索出所有的分词，每个分词获取其对应的一个或者多个问题的文档，并把所有的结果进行合并，匹配文档id最多的则选择为相关度最大的文档id，匹配数量相同的文档则进行编辑距离比较排序，最后返回文档列表。
     * */
    typedef std::map<uint64,std::set<ai_engine_question> > WordID2AIQuestionsMap;
    typedef WordID2AIQuestionsMap::iterator WordID2AIquestionsMapIter;
    typedef WordID2AIQuestionsMap::const_iterator WordID2AIquestionsMapCIter;
    typedef WordID2AIQuestionsMap::value_type WordID2AIquestionsMapValue;
    WordID2AIQuestionsMap m_wordID2questionsMap;//wordid -> set (ai_engine_question) 分词对应含该分词的问题集合

    //统计计算
    typedef std::map<uint64,uint32> QuestionIDCounterMap;
    typedef QuestionIDCounterMap::iterator QuestionIDCounterMapIt;
    typedef QuestionIDCounterMap::const_iterator QuestionIDCounterMapCIt;
    typedef QuestionIDCounterMap::value_type QuestionIDCounterMapValue;
    QuestionIDCounterMap m_questionIDCounterMap;//questionid -> counter

    typedef std::map<std::string,word_type_match> WordTypeCounterMap;
    typedef WordTypeCounterMap::iterator WordTypeCounterMapIt;
    typedef WordTypeCounterMap::const_iterator WordTypeCounterMapCIt;
    typedef WordTypeCounterMap::value_type WordTypeCounterMapValue;
    WordTypeCounterMap m_wordTypeCounterMap;//wordType -> counter
};

SessionAiEngine* GetSessionAiEngine(oss::OssLabor* pLabor);


//搜索引擎在词库 354507个， ai问题 43个 时，占用实际内存为3.6g
//ps 指令 RSZ 真实内存使用（KB）  VSZ 虚拟内存量（KB）
//[imdev@node3 RobotServer]$ ps -e -o "pid,comm,args,pcpu,rsz,vsz"|grep robot | sort -nr --key=5
//23039 Robot_robot_W0  Robot_robot_W0               0.4 3725884 3902228
//12972 fdfs_storaged   /app/robot/fastdfs/FastDFS/  0.0 2541212 2985408
//26508 fdfs_trackerd   /app/robot/fastdfs/FastDFS/  0.0 83552 430596

//top指令
//[imdev@node3 RobotServer]$ top -p 23039
//top - 14:01:58 up 208 days,  4:29,  4 users,  load average: 0.00, 0.02, 0.00
//Tasks:   1 total,   0 running,   1 sleeping,   0 stopped,   0 zombie
//Cpu(s):  0.1%us,  0.3%sy,  0.0%ni, 99.6%id,  0.0%wa,  0.0%hi,  0.0%si,  0.0%st
//Mem:  16207408k total, 15891692k used,   315716k free,   270420k buffers
//Swap:  4194296k total,   119168k used,  4075128k free,  4679012k cached
//
//  PID USER      PR  NI  VIRT  RES  SHR S %CPU %MEM    TIME+  COMMAND
//23039 imdev     20   0 3810m 3.6g 6104 S  0.0 23.0   0:07.02 Robot_robot_W0


//搜索引擎在词库 620579个， ai问题 43个 时，占用实际内存为6.7g
//ps -e -o "pid,comm,args,pcpu,rsz,vsz"|grep robot | sort -nr --key=5
//8346 Robot_robot_W0  Robot_robot_W0               3.3 7020252 7200716

//top -p 8346
//top - 15:22:39 up 215 days,  5:49,  6 users,  load average: 0.08, 0.72, 0.79
//Tasks:   1 total,   0 running,   1 sleeping,   0 stopped,   0 zombie
//Cpu(s):  2.4%us,  0.4%sy,  0.0%ni, 97.2%id,  0.0%wa,  0.0%hi,  0.0%si,  0.0%st
//Mem:  16207408k total, 14375812k used,  1831596k free,    35612k buffers
//Swap:  4194296k total,  3158660k used,  1035636k free,   288376k cached
//
//  PID USER      PR  NI  VIRT  RES  SHR S %CPU %MEM    TIME+  COMMAND
// 8346 imdev     20   0 7031m 6.7g 6272 S  0.0 43.3   0:16.17 Robot_robot_W0


} /* namespace robot */

#endif /* SRC_SESSIONAIENGINE_HPP_ */
