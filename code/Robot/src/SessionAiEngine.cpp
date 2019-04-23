/*******************************************************************************
 * Project:  LogicServer
 * @file     SessionSensitive.cpp
 * @brief 
 * @author   cjy
 * @date:    2016年6月16日
 * @note
 * Modify history:
 ******************************************************************************/
#include "SessionAiEngine.hpp"
#include "StepLoadAiEngineQuestions.hpp"

namespace robot
{


const char* const DICT_PATH = "./dict/jieba.dict.utf8";
const char* const HMM_PATH = "./dict/hmm_model.utf8";
const char* const USER_DICT_PATH = "./dict/user.dict.utf8";
const char* const IDF_PATH = "./dict/idf.utf8";
const char* const STOP_WORD_PATH = "./dict/stop_words.utf8";

SessionAiEngine::SessionAiEngine(uint32 ulid, ev_tstamp dSessionTimeout)
    : net::Timer(ulid, dSessionTimeout, "robot::SessionAiEngine"),
      boInit(false),boLoadAiEngineWords(false),boLoadAiEngineQuestions(false),boBuildAiEngineWords(false),
      boBuildAiEngineStopWords(false),boBuildAiEngineQuestions(false),
      boTest(false),m_CheckRoutineTime(0),m_status(eSessionAiEngineQuestions_start),m_convertCode(0),m_nSearchLineCount(0),
	  jieba(DICT_PATH,
		HMM_PATH,
		USER_DICT_PATH,
		IDF_PATH,
		STOP_WORD_PATH)
{
}

SessionAiEngine::~SessionAiEngine()
{
}

bool SessionAiEngine::test()
{
    if(boTest)
    {
        return true;
    }
    boTest = true;
    if (m_strSearchLine.size() == 0)
    {
        LOG4CPLUS_WARN_FMT(m_Logger,"m_strSearchLine.size() == 0");
        return false;
    }
    LOG4CPLUS_INFO_FMT(m_Logger,"SessionAiEngine::test begin");
    uint32 appid(1);
    //词库测试
    {//测试分词
        {//测试获取词
            //SearchKeys(toSearchLine,words):toSearchLine(你我金融是什么) use time(60.000000)ms,try num(10000) words size(4)
            // toSearchLine(你我金融是什么) SearchWords word(你我) wordNo(1)
            // toSearchLine(你我金融是什么) SearchWords word(金) wordNo(2)
            // toSearchLine(你我金融是什么) SearchWords word(融) wordNo(3)
            // toSearchLine(你我金融是什么) SearchWords word(是什) wordNo(4)
            //约160000qps

            //SearchKeys(toSearchLine,words):toSearchLine(你我贷，国内领先的在线P2P信用投融资平台（是互联网金融 ITFIN 产品的一种），金融改革践行者，专注中小微企业、个体户及农户的资金发展需求，长期致力于全民信用体系的构建与完善。)
            //use time(70.000000)ms,try num(1000) words size(40)
            //约14000qps
            std::vector<std::string> words;
            clock_t b = clock();
            int n = m_nSearchLineCount;
            for(int i = 0;i < n;++i)
            {
                this->SearchWords(m_strSearchLine,words);
            }
            clock_t last = clock();
            double useTime = ((last - b)* 1000) /CLOCKS_PER_SEC ;
            LOG4CPLUS_INFO_FMT(m_Logger,"SearchKeys(toSearchLine,words):"
                            "toSearchLine(%s) use time(%lf)ms,try num(%d) words size(%u)",
                            m_strSearchLine.c_str(),useTime,n,words.size());
            std::vector<std::string>::const_iterator it = words.begin();
            std::vector<std::string>::const_iterator itEnd = words.end();
            int wordNo(1);
            for(;it != itEnd;++it,++wordNo)
            {
                LOG4CPLUS_INFO_FMT(m_Logger,"%s() toSearchLine(%s) SearchWords word(%s) wordNo(%d)",
                                __FUNCTION__,m_strSearchLine.c_str(),it->c_str(),wordNo);
            }
        }
    }
    {//测试匹配问题id
        {
            std::vector<ai_engine_question> aiQuestionVec;
            clock_t b = clock();
            int n = m_nSearchLineCount;
            for(int i = 0;i < n;++i)
            {
                this->GetAiQuestionListByReqQuestion(m_strSearchLine,appid,aiQuestionVec);
            }
            clock_t last = clock();
            double useTime = ((last - b)* 1000) /CLOCKS_PER_SEC ;
            //GetAiQuestionListByReqQuestion(toSearchLine,aiQuestionIDVec):toSearchLine(你我金融是什么) use time(320.000000)ms,try num(10000) aiQuestionVec size(2)
            //约31250qps,约是sphinx同步接口查询相同问题的31250qps/190qps = 164倍
            //GetAiQuestionListByReqQuestion(toSearchLine,aiQuestionIDVec):toSearchLine(你我贷，国内领先的在线P2P信用投融资平台（是互联网金融 ITFIN 产品的一种），金融改革践行者，专注中小微企业、个体户及农户的资金发展需求，长期致力于全民信用体系的构建与完善。)
            //use time(630.000000)ms,try num(1000) aiQuestionVec size(8)
            //约1587qps
            LOG4CPLUS_INFO_FMT(m_Logger,"GetAiQuestionListByReqQuestion(toSearchLine,aiQuestionIDVec):"
                            "toSearchLine(%s) use time(%lf)ms,try num(%d) aiQuestionVec size(%u)",
                            m_strSearchLine.c_str(),useTime,n,aiQuestionVec.size());

            if (aiQuestionVec.size() > 0)
            {
                // toSearchLine(你我金融是什么),best match ai question(你我金融) questionid(9223372036854775807)
                // toSearchLine(你我金融是什么) match ai question(你我金融) questionid(9223372036854775807) questionNo(1) uiMatchWordsCounter(1) nLevenshtein(9)
                // toSearchLine(你我金融是什么) match ai question(你我的企业标) questionid(11) questionNo(2) uiMatchWordsCounter(1) nLevenshtein(14)
                ai_engine_question question = *aiQuestionVec.begin();
                LOG4CPLUS_INFO_FMT(m_Logger,"%s() toSearchLine(%s),best match ai question(%s) questionid(%llu)",
                                            __FUNCTION__,m_strSearchLine.c_str(),question.question.c_str(),
                                            question.index_id);
                std::vector<ai_engine_question>::const_iterator it = aiQuestionVec.begin();
                std::vector<ai_engine_question>::const_iterator itEnd = aiQuestionVec.end();
                int questionNo(1);
                for(;it != itEnd;++it,++questionNo)
                {
                    LOG4CPLUS_INFO_FMT(m_Logger,"%s() toSearchLine(%s) match ai question(%s) questionid(%llu) questionNo(%d) uiMatchWordsCounter(%u) nLevenshtein(%u)",
                                    __FUNCTION__,m_strSearchLine.c_str(),it->question.c_str(),it->index_id,questionNo,it->uiMatchWordsCounter,it->nLevenshtein);
                }
            }
        }
    }
    {//测试匹配问题
        {
            ai_engine_question question;
            clock_t b = clock();
            int n = m_nSearchLineCount;
            for(int i = 0;i < n;++i)
            {
                this->GetBestAiQuestionByQuestion(m_strSearchLine,appid,question);
            }
            clock_t last = clock();
            double useTime = ((last - b)* 1000) /CLOCKS_PER_SEC ;
            if (question.question.size() > 0)
            {
                //use time(230.000000)ms,try num(10000)  （30w 词库）
                //GetBestAiQuestionByQuestion(toSearchLine,question):toSearchLine(你我金融是什么) use time(330.000000)ms,try num(10000) ai question(你我金融)

                //GetBestAiQuestionByQuestion(toSearchLine,question):toSearchLine(你我贷，国内领先的在线P2P信用投融资平台（是互联网金融 ITFIN 产品的一种），金融改革践行者，专注中小微企业、个体户及农户的资金发展需求，长期致力于全民信用体系的构建与完善。)
                //use time(640.000000)ms,try num(1000) ai question(投资人) uiMatchWordsCounter(3) nLevenshtein(241)
                //约1562qps （62w 词库）
                LOG4CPLUS_INFO_FMT(m_Logger,"GetBestAiQuestionByQuestion(toSearchLine,question):"
                        "toSearchLine(%s) use time(%lf)ms,try num(%d) ai question(%s) uiMatchWordsCounter(%u) nLevenshtein(%u)",
                        m_strSearchLine.c_str(),useTime,n,question.question.c_str(),question.uiMatchWordsCounter,question.nLevenshtein);

            }
            else
            {
                LOG4CPLUS_INFO_FMT(m_Logger,"GetBestAiQuestionByQuestion(toSearchLine,question):"
                        "toSearchLine(%s) use time(%lf)ms,try num(%d) no match ai question",
                        m_strSearchLine.c_str(),useTime,n);
            }
        }
    }
    {//测试提问分类
        clock_t b = clock();
        std::vector<classify_question_type> oClassifyQuestionVec;
        int n = m_nSearchLineCount;
        for(int i = 0;i < n;++i)
        {
            GetClassifyListByReqQuestion(m_strSearchLine,oClassifyQuestionVec);
        }
        clock_t last = clock();
        double useTime = ((last - b)* 1000) /CLOCKS_PER_SEC ;
        if (oClassifyQuestionVec.size() > 0)
        {
            /*
             * toSearchLine(你我金融是什么) use time(180.000000)ms,try num(10000) oClassifyQuestionVec.size(3)
                uiMatchCounter(2) strReqQuestionType(生活百科) no(1)
                strReqQuestionType(生活百科) match word(你我)
                strReqQuestionType(生活百科) match word(金)
                uiMatchCounter(1) strReqQuestionType(人文科学) no(2)
                strReqQuestionType(人文科学) match word(融)
                uiMatchCounter(1) strReqQuestionType(社会科学) no(3)
                strReqQuestionType(社会科学) match word(是什)
             * */
            /*
             toSearchLine(你我贷，国内领先的在线P2P信用投融资平台（是互联网金融 ITFIN 产品的一种），金融改革践行者，专注中小微企业、个体户及农户的资金发展需求，长期致力于全民信用体系的构建与完善。)
             use time(140.000000)ms,try num(1000) oClassifyQuestionVec.size(5)
            oClassifyQuestionVec uiMatchCounter(18) strReqQuestionType(生活百科) no(1)
            strReqQuestionType(生活百科) match word(你我)
            strReqQuestionType(生活百科) match word(国)
            strReqQuestionType(生活百科) match word(信)
            strReqQuestionType(生活百科) match word(投)
            strReqQuestionType(生活百科) match word(资)
            strReqQuestionType(生活百科) match word(金)
            strReqQuestionType(生活百科) match word(的一)
            strReqQuestionType(生活百科) match word(金)
            strReqQuestionType(生活百科) match word(行者)
            strReqQuestionType(生活百科) match word(微)
            strReqQuestionType(生活百科) match word(资)
            strReqQuestionType(生活百科) match word(金)
            strReqQuestionType(生活百科) match word(发)
            strReqQuestionType(生活百科) match word(期)
            strReqQuestionType(生活百科) match word(力)
            strReqQuestionType(生活百科) match word(信)
            strReqQuestionType(生活百科) match word(系)
            strReqQuestionType(生活百科) match word(建)
            oClassifyQuestionVec uiMatchCounter(10) strReqQuestionType(社会科学) no(2)
            strReqQuestionType(社会科学) match word(内领)
            strReqQuestionType(社会科学) match word(平台)
            strReqQuestionType(社会科学) match word(改)
            strReqQuestionType(社会科学) match word(革)
            strReqQuestionType(社会科学) match word(企业)
            strReqQuestionType(社会科学) match word(个体)
            strReqQuestionType(社会科学) match word(展)
            strReqQuestionType(社会科学) match word(需求)
            strReqQuestionType(社会科学) match word(民)
            strReqQuestionType(社会科学) match word(体)
            oClassifyQuestionVec uiMatchCounter(4) strReqQuestionType(人文科学) no(3)
            strReqQuestionType(人文科学) match word(融)
            strReqQuestionType(人文科学) match word(融)
            strReqQuestionType(人文科学) match word(融)
            strReqQuestionType(人文科学) match word(善)
            oClassifyQuestionVec uiMatchCounter(2) strReqQuestionType(工程应用) no(4)
            test() strReqQuestionType(工程应用) match word(在线)
            strReqQuestionType(工程应用) match word(互联网)
            oClassifyQuestionVec uiMatchCounter(1) strReqQuestionType(农林鱼畜) no(5)
            strReqQuestionType(农林鱼畜) match word(产品)
             * */
            LOG4CPLUS_INFO_FMT(m_Logger,"GetClassifyListByReqQuestion(toSearchLine,oClassifyQuestionVec):"
                            "toSearchLine(%s) use time(%lf)ms,try num(%d) oClassifyQuestionVec.size(%u)",
                            m_strSearchLine.c_str(),useTime,n,oClassifyQuestionVec.size());
            std::vector<classify_question_type>::const_iterator cit = oClassifyQuestionVec.begin();
            std::vector<classify_question_type>::const_iterator citend = oClassifyQuestionVec.end();
            int counter(1);
            for(;cit != citend;++cit,++counter)
            {
                LOG4CPLUS_INFO_FMT(m_Logger,"oClassifyQuestionVec words.size(%u) strReqQuestionType(%s) no(%d)",
                                cit->words.size(),cit->strReqQuestionType.c_str(),counter);
                std::vector<std::string>::const_iterator wordscit = cit->words.begin();
                std::vector<std::string>::const_iterator wordscitEnd = cit->words.end();
                for(;wordscit != wordscitEnd;++wordscit)
                {
                    LOG4CPLUS_INFO_FMT(m_Logger,"%s() strReqQuestionType(%s) match word(%s)",
                                    __FUNCTION__,cit->strReqQuestionType.c_str(),wordscit->c_str());
                }
            }
        }

    }
    LOG4CPLUS_INFO_FMT(m_Logger,"SessionAiEngine::test finish");
    return true;
}

bool SessionAiEngine::init(log4cplus::Logger logger)
{
    if(boInit)
    {
        return true;
    }
    m_Logger = logger;
    {//初始化状态机日志
        m_AiEngineWordsDfa.SetLogger(m_Logger);
        m_AiEngineStopWordsDfa.SetLogger(m_Logger);
    }
    std::string ignore_chars;
    lnet::CJsonObject customConf = GetCustomConf();
    if(customConf.Get("ignore_chars",ignore_chars))
    {
        RemoveFlag(ignore_chars,' ');
        int s = ignore_chars.length();
        char *tmpChars = new char[s + 1];
        snprintf(tmpChars,s + 1,ignore_chars.c_str());
        LOG4CPLUS_TRACE_FMT(m_Logger,"ignore letters:%s",tmpChars);
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
                LOG4CPLUS_TRACE_FMT(m_Logger,"ignore letter:%u,%c",(unsigned char)ascii,(unsigned char)ascii);
                m_ignoreCharsVec.push_back((unsigned char)ascii);
            }
            else if(0 == tmpChars[i])
            {
                ascii = atoi(&tmpChars[j]);
                LOG4CPLUS_TRACE_FMT(m_Logger,"ignore letter:%u,%c",(unsigned char)ascii,(unsigned char)ascii);
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
    if (!customConf.Get("words_file_ext",m_strWordsFileExt))
    {
        m_strWordsFileExt = ".txt";
    }
    LOG4CPLUS_TRACE_FMT(m_Logger, "%s() m_strWordsFileExt(%s)",__FUNCTION__,m_strWordsFileExt.c_str());
    if (!customConf.Get("convert_code",m_convertCode))
    {
        m_convertCode = 0;
    }
    if (!customConf.Get("search_line",m_strSearchLine))
    {
        m_strSearchLine = "你我金融是什么";
    }
    if (!customConf.Get("search_line_count",m_nSearchLineCount))
    {
        m_nSearchLineCount = 1000;
    }

    LOG4CPLUS_TRACE_FMT(m_Logger, "%s() m_convertCode(%d)",__FUNCTION__,m_convertCode);
    if (!customConf.Get("words_dir",m_ReadWordsFilePath))
    {
        m_ReadWordsFilePath = "./words";
    }
    if (!customConf.Get("stop_words",m_StopWordsFileName))
    {
        m_StopWordsFileName = "stop";
    }
    LOG4CPLUS_TRACE_FMT(m_Logger, "%s() m_ReadWordsFilePath(%s)",__FUNCTION__,m_ReadWordsFilePath.c_str());
    boInit = true;
    return true;
}

bool SessionAiEngine::LoadAiEngineWords(bool boForce)
{
    LOG4CPLUS_TRACE_FMT(m_Logger, "%s() LoadAiEngineWords",__FUNCTION__);
    if (!boLoadAiEngineWords || boForce)
    {
        //CustomClock LoadAiEngineWords use time(1508.917969) ms (30w words)
        //CustomClock LoadAiEngineWords use time(3017.746094) ms (62w words)
        CustomClock clock("LoadAiEngineWords",m_Logger);
        if (!CheckWordsFiles())
        {
            LOG4CPLUS_ERROR_FMT(m_Logger, "%s() CheckDataFiles failed",__FUNCTION__);
            return false;
        }
        if (!ReadWordsFiles())
        {
            LOG4CPLUS_ERROR_FMT(m_Logger, "%s() ReadWordsFiles failed",__FUNCTION__);
            return false;
        }
        boLoadAiEngineWords = true;
        LOG4CPLUS_TRACE_FMT(m_Logger,"%s() succ to LoadAiEngineWords",__FUNCTION__);
        return true;
    }
    LOG4CPLUS_TRACE_FMT(m_Logger,"%s() already LoadAiEngineWords",__FUNCTION__);
    return true;
}

bool SessionAiEngine::CheckWordsFiles()
{
    LOG4CPLUS_TRACE_FMT(m_Logger,"SessionAiEngine::CheckDataFiles");
    m_WordsfilesNameVec.clear();
    if (!lnet::IsDirectory(m_ReadWordsFilePath.c_str()))
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(), "%s() m_ReadWordsFilePath(%s) not exist",__FUNCTION__,m_ReadWordsFilePath.c_str());
        return false;
    }
    const char* sFileExt = m_strWordsFileExt.c_str();//".txt";
    if (lnet::GetDirCommonFilesByExt(m_ReadWordsFilePath.c_str(), sFileExt, 3,
                    m_WordsfilesNameVec,true) == -1)
    {
        LOG4CPLUS_ERROR_FMT(m_Logger, "can't get readfile_path(%s) file for input files,errno(%d),strerror(%s)",
                        m_ReadWordsFilePath.c_str(), errno, strerror(errno));
        return (false);
    }
    if (m_WordsfilesNameVec.empty())//没有文件
    {
        LOG4CPLUS_TRACE_FMT(m_Logger, "readfile_path(%s) don't has files to read",
                        m_ReadWordsFilePath.c_str());
        return (false);
    }
    return true;
}

bool SessionAiEngine::ReadWordsFiles()
{
    LOG4CPLUS_TRACE_FMT(m_Logger,"%s()",__FUNCTION__);
    std::vector<std::string>::iterator it = m_WordsfilesNameVec.begin();
    std::vector<std::string>::iterator itEnd = m_WordsfilesNameVec.end();
    std::string strWriteTotalFile;
    int nReadTotalCounter(0);
    int nFileReadCounter(0);
    for(;it != itEnd;++it)
    {
        nFileReadCounter = 0;
        std::ifstream readfin(it->c_str(), std::ios::in);//读取一个数据文件
        if(!readfin.good())
        {
            //配置信息流读取失败
            LOG4_ERROR("Open conf (%s) error!",it->c_str());
            return false;
        }
        char sfileName[32];
        lnet::ExtractFileNameOnly(it->c_str(),sfileName,sizeof(sfileName));
        bool boStopWords(false);
        char sDirsName[64];
        memset(sDirsName,0,sizeof(sDirsName));
        if (0 == strcmp(sfileName,m_StopWordsFileName.c_str()))
        {
            boStopWords = true;
            LOG4CPLUS_TRACE_FMT(m_Logger,"stop words file(%s)",sfileName);
        }
        else
        {
            if (lnet::ExtractFileNearDirectoryName(it->c_str(),sDirsName,sizeof(sDirsName)) > 0)
            {
                LOG4CPLUS_INFO_FMT(m_Logger,"ExtractFileNearDirectoryName file(%s) sDirsName(%s)",it->c_str(),sDirsName);
            }
            else
            {
                LOG4CPLUS_WARN_FMT(m_Logger,"ExtractFileNearDirectoryName failed file(%s)",it->c_str());
                continue;
            }
        }
        {
            std::string line;
            while(std::getline(readfin,line))
            {
                if(line.size() > 0)
                {
                    if(m_convertCode)
                    {
                        std::string convertedCode;
                        int ret = lnet::gbk2utf8(convertedCode,line.c_str());
                        if (ret < 0)
                        {
                            LOG4CPLUS_WARN_FMT(m_Logger,"failed to gbk2utf8,code:%d",ret);
                            readfin.close();
                            return false;
                        }
                        line.assign(convertedCode);
                    }
                    if (boStopWords)
                    {
                        AddAiEngineStopWordsAppend(line);
                    }
                    else
                    {
                        AddAiEngineWordsAppend(line,std::string(sDirsName));
                    }
                    ++nReadTotalCounter;
                    ++nFileReadCounter;
                    //LOG4CPLUS_TRACE_FMT(m_Logger,"line size(%u):%s",line.size(),line.c_str());
                }
                else
                {
                    LOG4_TRACE("empty line");
                }
            }
            readfin.close();
        }
        LOG4CPLUS_TRACE_FMT(m_Logger,"read file(%s) ok nFileReadCounter(%d) nReadTotalCounter(%d)",
                        it->c_str(),nFileReadCounter,nReadTotalCounter);
    }
    return true;
}

bool SessionAiEngine::IsAddAiEngineWordsAppend(const std::string& word)const
{
    std::vector<AppendAiEngineWords>::const_iterator it = m_AddWordsAppend.begin();
    std::vector<AppendAiEngineWords>::const_iterator itEnd = m_AddWordsAppend.end();
    for(;it != itEnd;++it)
    {
        if (word == it->word)
        {
            return true;
        }
    }
    return false;
}

bool SessionAiEngine::IsDelAiEngineWordsAppend(const std::string& word)const
{
    std::vector<AppendAiEngineWords>::const_iterator it = m_DelWordsAppend.begin();
    std::vector<AppendAiEngineWords>::const_iterator itEnd = m_DelWordsAppend.end();
    for(;it != itEnd;++it)
    {
        if (word == it->word)
        {
            return true;
        }
    }
    return false;
}

void SessionAiEngine::AddAiEngineWordsAppend(const std::string& ai_engine_word,const std::string& dir)
{
//    LOG4CPLUS_TRACE_FMT(m_Logger,"add add_ai_engine_word(%s) appending",ai_engine_word.c_str());
    AppendAiEngineWords appendAiEngine;
    appendAiEngine.word = ai_engine_word;
    appendAiEngine.dir = dir;
    m_AddWordsAppend.push_back(appendAiEngine);
}

void SessionAiEngine::DelAiEngineWordsAppend(const std::string& ai_engine_word,const std::string& dir)
{
//    LOG4CPLUS_TRACE_FMT(m_Logger,"del_ai_engine_word(%s) appending",del_ai_engine_word.c_str());
    AppendAiEngineWords appendAiEngine;
    appendAiEngine.word = ai_engine_word;
    appendAiEngine.dir = dir;
    m_DelWordsAppend.push_back(appendAiEngine);
}

void SessionAiEngine::AddAiEngineStopWordsAppend(const std::string& ai_engine_word)
{
//    LOG4CPLUS_TRACE_FMT(m_Logger,"add add_ai_engine_word(%s) appending",ai_engine_word.c_str());
    AppendAiEngineWords appendAiEngine;
    appendAiEngine.word = ai_engine_word;
    m_AddStopWordsAppend.push_back(appendAiEngine);
}

void SessionAiEngine::DelAiEngineStopWordsAppend(const std::string& ai_engine_word)
{
//    LOG4CPLUS_TRACE_FMT(m_Logger,"del_ai_engine_word(%s) appending",del_ai_engine_word.c_str());
    AppendAiEngineWords appendAiEngine;
    appendAiEngine.word = ai_engine_word;
    m_DelStopWordsAppend.push_back(appendAiEngine);
}

bool SessionAiEngine::HandleAppendAiEngineWords()
{
    {//停用词
        LOG4CPLUS_INFO_FMT(m_Logger,"%s() m_AddStopWordsAppend.size(%u) m_DelStopWordsAppend.size(%u)",
                            __FUNCTION__,m_AddStopWordsAppend.size(),m_DelStopWordsAppend.size());
        if(m_AddStopWordsAppend.size() > 0 || m_DelStopWordsAppend.size() > 0)
        {
            if(m_AddStopWordsAppend.size() > 0)
            {//增加停用词处理
                LOG4CPLUS_TRACE_FMT(m_Logger,"HandleAppendWords m_AddStopWordsAppend size(%u)",m_AddStopWordsAppend.size());
                std::vector<AppendAiEngineWords>::const_iterator it = m_AddStopWordsAppend.begin();
                std::vector<AppendAiEngineWords>::const_iterator itEnd = m_AddStopWordsAppend.end();
                for(;it != itEnd;++it)
                {
                    AddAiEngineStopWords(it->word);
                }
            }
            if(m_DelStopWordsAppend.size() > 0)
            {//减少停用词处理
                LOG4CPLUS_TRACE_FMT(m_Logger,"HandleAppendWords m_DelStopWordsAppend size(%u)",m_DelStopWordsAppend.size());
                std::vector<AppendAiEngineWords>::const_iterator it = m_DelStopWordsAppend.begin();
                std::vector<AppendAiEngineWords>::const_iterator itEnd = m_DelStopWordsAppend.end();
                for(;it != itEnd;++it)
                {
                    DelAiEngineStopWords(it->word);
                }
            }
            m_AddStopWordsAppend.clear();
            m_DelStopWordsAppend.clear();
            if (!BuildStopWordsFfa())
            {
                LOG4CPLUS_WARN_FMT(m_Logger,"BuildStopWordsFfa error");
                return false;
            }
        }
    }
    {//实用词
        LOG4CPLUS_INFO_FMT(m_Logger,"%s() m_AddWordsAppend.size(%u) m_DelWordsAppend.size(%u)",
                            __FUNCTION__,m_AddWordsAppend.size(),m_DelWordsAppend.size());
        if(m_AddWordsAppend.size() > 0 || m_DelWordsAppend.size() > 0)
        {
            if(m_AddWordsAppend.size() > 0)
            {//增加词处理
                LOG4CPLUS_TRACE_FMT(m_Logger,"HandleAppendWords m_AddWordsAppend size(%u)",m_AddWordsAppend.size());
                std::vector<AppendAiEngineWords>::const_iterator it = m_AddWordsAppend.begin();
                std::vector<AppendAiEngineWords>::const_iterator itEnd = m_AddWordsAppend.end();
                for(;it != itEnd;++it)
                {
                    if (!IsAiEngineStopWords(it->word))//不加入停用词
                    {
                        AddAiEngineWords(*it);
                    }
                    else
                    {
                        LOG4CPLUS_TRACE_FMT(m_Logger,"HandleAppendWords ignore stop word(%s)",it->word.c_str());
                    }
                }
            }
            if(m_DelWordsAppend.size() > 0)
            {//减少词处理
                LOG4CPLUS_TRACE_FMT(m_Logger,"HandleAppendWords m_DelWordsAppend size(%u)",m_DelWordsAppend.size());
                std::vector<AppendAiEngineWords>::const_iterator it = m_DelWordsAppend.begin();
                std::vector<AppendAiEngineWords>::const_iterator itEnd = m_DelWordsAppend.end();
                for(;it != itEnd;++it)
                {
                    DelAiEngineWords(*it);
                }
            }
            m_AddWordsAppend.clear();
            m_DelWordsAppend.clear();
            LOG4CPLUS_TRACE_FMT(m_Logger,"HandleAppendWords m_AiEngineWordsTypeMap.size(%u)",m_AiEngineWordsTypeMap.size());
            if(!BuildWordsFfa())
            {
                LOG4CPLUS_WARN_FMT(m_Logger,"BuildWordsFfa error");
                return false;
            }
        }
    }
    return true;
}

void SessionAiEngine::AddAiEngineQuestionAppend(const ai_engine_question& question)
{
    LOG4CPLUS_TRACE_FMT(m_Logger,"%s() add question(%s) index_id(%llu) appid(%llu) appending",__FUNCTION__,
                    question.question.c_str(),question.index_id,question.appid);
    AppendAiEngineQuestion appendAiEngineQuestion;
    appendAiEngineQuestion.question = question;
    SkipIgnoreLetters(appendAiEngineQuestion.question.question);
    if (appendAiEngineQuestion.question.question.size() > 0 && question.index_id > 0 && question.appid > 0)
    {
        m_AddQuestionsAppend.push_back(appendAiEngineQuestion);
    }
    else
    {
        LOG4CPLUS_WARN_FMT(m_Logger,"%s() invalid question(%s) question.size(%u) index_id(%llu) appid(%u)",
                        __FUNCTION__,question.question.c_str(),appendAiEngineQuestion.question.question.size(),
                        question.index_id,question.appid);
    }
}

void SessionAiEngine::DelAiEngineQuestionAppend(const ai_engine_question& question)
{
    LOG4CPLUS_TRACE_FMT(m_Logger,"del question(%s) appending",__FUNCTION__,question.question.c_str());
    AppendAiEngineQuestion appendAiEngineQuestion;
    appendAiEngineQuestion.question = question;
    SkipIgnoreLetters(appendAiEngineQuestion.question.question);
    if (appendAiEngineQuestion.question.question.size() > 0)
    {
        m_DelQuestionsAppend.push_back(appendAiEngineQuestion);
    }
    else
    {
        LOG4CPLUS_WARN_FMT(m_Logger,"%s() del question(%s) appending failed.appendAiEngineQuestion.question.question.size() == 0",
                                    __FUNCTION__,question.question.c_str());
    }
}

bool SessionAiEngine::AddAiEngineQuestion(const ai_engine_question& question)
{
    if (question.question.size() == 0 || 0 == question.index_id)
    {
        LOG4CPLUS_WARN_FMT(GetLogger(),"%s() question.question.size(%u) == 0 question.index_id(%llu)",
                        __FUNCTION__,question.question.size(),question.index_id);
        return false;
    }
    //question.index_id = lnet::HashStrToUint64(question.question.c_str(),question.question.size());
    m_AiEngineQuestionsMap.insert(std::make_pair(question.index_id,question));
    LOG4CPLUS_TRACE_FMT(GetLogger(),"%s() AddAiEngineQuestion ok,m_AiEngineQuestionsMap size(%u),question(%s) questionid(%llu)",
                    __FUNCTION__,m_AiEngineQuestionsMap.size(),question.question.c_str(),question.index_id);
    return true;
}

bool SessionAiEngine::DelAiEngineQuestion(const ai_engine_question& question)
{
    if (question.question.size() == 0)
    {
        LOG4CPLUS_WARN_FMT(GetLogger(),"%s() question.question.size() == 0",__FUNCTION__);
        return false;
    }
    uint64 questionid = lnet::HashStrToUint64(question.question.c_str(),question.question.size());
    m_AiEngineQuestionsMap.erase(questionid);
    LOG4CPLUS_TRACE_FMT(GetLogger(),"%s() DelAiEngineQuestion ok m_AiEngineQuestionsMap size(%u),question(%s) questionid(%llu)",
                    __FUNCTION__,m_AiEngineQuestionsMap.size(),question.question.c_str(),questionid);
    return true;
}

bool SessionAiEngine::HandleAppendAiEngineQuestion()
{
    LOG4CPLUS_INFO_FMT(m_Logger,"%s() m_AddQuestionsAppend.size(%u) m_DelQuestionsAppend.size(%u)",
                    __FUNCTION__,m_AddQuestionsAppend.size(),m_DelQuestionsAppend.size());
    if(m_AddQuestionsAppend.size() > 0 || m_DelQuestionsAppend.size() > 0)
    {
        if(m_AddQuestionsAppend.size() > 0)
        {//增加问题处理
            LOG4CPLUS_TRACE_FMT(m_Logger,"HandleAppendAiEngineQuestion m_AddQuestionsAppend size(%u)",m_AddQuestionsAppend.size());
            std::vector<AppendAiEngineQuestion>::iterator it = m_AddQuestionsAppend.begin();
            std::vector<AppendAiEngineQuestion>::iterator itEnd = m_AddQuestionsAppend.end();
            for(;it != itEnd;++it)
            {
                AddAiEngineQuestion(it->question);
            }
        }
        if(m_DelQuestionsAppend.size() > 0)
        {//减少问题处理
            LOG4CPLUS_TRACE_FMT(m_Logger,"HandleAppendAiEngineQuestion m_DelQuestionsAppend size(%u)",m_DelQuestionsAppend.size());
            std::vector<AppendAiEngineQuestion>::iterator it = m_DelQuestionsAppend.begin();
            std::vector<AppendAiEngineQuestion>::iterator itEnd = m_DelQuestionsAppend.end();
            for(;it != itEnd;++it)
            {
                DelAiEngineQuestion(it->question);
            }
        }
        m_AddQuestionsAppend.clear();
        m_DelQuestionsAppend.clear();
        if (!BuildQuestionsMap())
        {
            LOG4CPLUS_WARN_FMT(m_Logger,"BuildQuestionsMap error");
            return false;
        }
    }
    return true;
}

bool SessionAiEngine::Build()
{
    LOG4CPLUS_INFO_FMT(m_Logger,"%s()",__FUNCTION__);
    HandleAppendAiEngineWords();
    HandleAppendAiEngineQuestion();
    m_status = eSessionAiEngineQuestions_loaded;
    test();
    return true;
}

bool SessionAiEngine::BuildWordsFfa()
{
    if (!boBuildAiEngineWords)
    {
        if(m_AiEngineWordsSet.size() == 0)
        {
            LOG4CPLUS_TRACE_FMT(m_Logger,"%s() m_AiEngineWordsSet empty",__FUNCTION__);
        }
        if(!m_AiEngineWordsDfa.CreateKeysTree(m_AiEngineWordsSet))
        {
            LOG4CPLUS_ERROR_FMT(m_Logger,"%s() m_AiEngineWordsDfa CreateKeysTree failed",__FUNCTION__);
            return false;
        }
        //BuildWordsFfa() m_AiEngineWordsSet init ok,m_AiEngineWordsSet size(620648)
        LOG4CPLUS_INFO_FMT(m_Logger,"%s() m_AiEngineWordsSet init ok,m_AiEngineWordsSet size(%u)",
                        __FUNCTION__,m_AiEngineWordsSet.size());
        boBuildAiEngineWords = true;
    }
    else
    {
        LOG4CPLUS_INFO_FMT(m_Logger,"%s() m_AiEngineKeysSet already init",__FUNCTION__);
    }
    return true;
}

bool SessionAiEngine::BuildStopWordsFfa()
{
    if (!boBuildAiEngineStopWords)
    {
        if(m_AiEngineStopWordsSet.size() == 0)
        {
            LOG4CPLUS_TRACE_FMT(m_Logger,"%s() m_AiEngineStopWordsSet empty",__FUNCTION__);
        }
        if(!m_AiEngineStopWordsDfa.CreateKeysTree(m_AiEngineStopWordsSet))
        {
            LOG4CPLUS_ERROR_FMT(m_Logger,"%s() m_AiEngineStopWordsDfa CreateKeysTree failed",__FUNCTION__);
            return false;
        }
        LOG4CPLUS_TRACE_FMT(m_Logger,"%s() m_AiEngineStopWordsSet init ok,m_AiEngineStopWordsSet size(%u)",
                        __FUNCTION__,m_AiEngineStopWordsSet.size());
        boBuildAiEngineStopWords = true;
    }
    else
    {
        LOG4CPLUS_TRACE_FMT(m_Logger,"%s() m_AiEngineStopWordsSet already init",__FUNCTION__);
    }
    return true;
}

bool SessionAiEngine::BuildQuestionsMap()
{
    int counter(0);
    LOG4CPLUS_INFO_FMT(m_Logger,"%s() m_AiEngineQuestionsMap size(%u)",__FUNCTION__,m_AiEngineQuestionsMap.size());
    //O(N) * (O(L) + O(K * ln2K))
    // 约等于 O(N) * O(K * ln2K)
    //N 为问题数,k为问题的分词数，L为问题长度
    for (AiEngineQuestionsMapCIt questionsMapit = m_AiEngineQuestionsMap.begin();
                    questionsMapit != m_AiEngineQuestionsMap.end();++questionsMapit,++counter)//ai 问题列表，O(N) N 为问题数
    {
        std::vector<std::string> words;//分词
        SearchWords(questionsMapit->second.question,words);//O(L),L为strQuestion长度，匹配单词数（包括多个同样单词）
        LOG4CPLUS_INFO_FMT(m_Logger,"%s() question counter(%d) SearchWords words size(%u)",__FUNCTION__,counter,words.size());
        if (words.size() > 0)
        {
            for(unsigned int i = 0;i < words.size();++i)//k为问题的分词数
            {
                const std::string& word = words[i];
                if (word.size() > 0)
                {
                    uint64 wordid = lnet::HashStrToUint64(word.c_str(),word.size());
                    {//该分词对应的问题
                        WordID2AIquestionsMapIter wordID2questionIDMapiter = m_wordID2questionsMap.find(wordid);//分词对应含该分词的问题集合ln2K,k为问题的分词数
                        if (wordID2questionIDMapiter == m_wordID2questionsMap.end())
                        {
                            std::set<ai_engine_question> set;
                            set.insert(questionsMapit->second);
                            m_wordID2questionsMap.insert(WordID2AIquestionsMapValue(wordid,set));//分词对应含该分词的问题集合ln2K,k为问题的分词数
                        }
                        else
                        {
                            wordID2questionIDMapiter->second.insert(questionsMapit->second);
                        }
                    }
                }
            }
        }
    }
    LOG4CPLUS_INFO_FMT(m_Logger,"SessionAiEngine::BuildQuestionsMap ok");
    boBuildAiEngineQuestions = true;
    return true;
}

bool SessionAiEngine::GetAiQuestionListByReqQuestion(const std::string& strReqQuestion,uint32 appid,
                std::vector<ai_engine_question>& aiQuestionVec)
{
    //总复杂度　　O(L) + O(J * lnN * lnC *B) + O(lnK * A) + O(lnA)
    //约等于= O(J * lnN * lnC *B) +O(lnK * A)
    //L请求的长度，K为问题数,J为请求问题的分词数，B为某分词对应问题个数,A最终匹配的问题数,词库分词N，C为某分词中的不同问题个数
    aiQuestionVec.clear();
    std::vector<std::string> words;//分词
    SearchWords(strReqQuestion,words);//O(L),L为strReqQuestion长度   ,请求的问题匹配的分词（重复的分词也会计算，因为计算词频）
    m_questionIDCounterMap.clear();
    if (words.size() > 0)
    {
        //大致 复杂度 O(J * ln2N * ln2K *B)
        //K为问题数,J为问题的分词数
        for(uint32 i = 0;i < words.size();++i)//请求问题的分词数J
        {
            const std::string& word = words[i];
            if (word.size() > 0)
            {
                uint64 wordid = lnet::HashStrToUint64(word.c_str(),word.size());
                {//该分词对应的问题
                    WordID2AIquestionsMapIter iter = m_wordID2questionsMap.find(wordid);//分词获取问题lnN,J为请求问题的分词数
                    if (iter != m_wordID2questionsMap.end())
                    {
                        const std::set<ai_engine_question>& questionIDSet = iter->second;//匹配分词对应的ai问题集合
                        std::set<ai_engine_question>::const_iterator questionIDSetIt = questionIDSet.begin();
                        std::set<ai_engine_question>::const_iterator questionIDSetItEnd = questionIDSet.end();
                        for(;questionIDSetIt != questionIDSetItEnd;++questionIDSetIt)
                        {
                            if (appid == questionIDSetIt->appid)//只匹配同一appid问题
                            {
                                QuestionIDCounterMapIt questionIDCounterMapIt =
                                                m_questionIDCounterMap.find(questionIDSetIt->index_id);
                                if (questionIDCounterMapIt == m_questionIDCounterMap.end())
                                {
                                    m_questionIDCounterMap.insert(std::make_pair(questionIDSetIt->index_id,1));//问题ID出现的次数计数lnC(C为某分词中的不同问题个数) * B（B为某分词对应问题个数）
                                }
                                else
                                {
                                    ++questionIDCounterMapIt->second;
                                }
                            }
                            else
                            {
                                LOG4CPLUS_TRACE_FMT(m_Logger,"pass question appid(%u) question(%llu,%s)",
                                                questionIDSetIt->appid,questionIDSetIt->index_id,questionIDSetIt->question.c_str());
                            }
                        }

                    }
                }
            }
        }
    }
    {
        //O(ln2K * A)
        //k问题数，A匹配的问题数
        QuestionIDCounterMapCIt cit = m_questionIDCounterMap.begin();//问题id出现次数（也就是该问题包含关键词的个数）
        QuestionIDCounterMapCIt citEnd = m_questionIDCounterMap.end();
        for(;cit != citEnd;++cit)
        {
            ai_engine_question aiQuestion;
            if (GetAiQuestionByAiQuestionID(cit->first,aiQuestion))
            {
                aiQuestion.uiMatchWordsCounter = cit->second;
                aiQuestion.strReqQuestion = strReqQuestion;
                aiQuestion.CalLevenshtein();
                aiQuestionVec.push_back(aiQuestion);
            }
        }
        //O(lnA)
        //匹配的问题数
        if (aiQuestionVec.size() > 0)
        {
            std::sort(aiQuestionVec.begin(),aiQuestionVec.end());
        }
    }
    return aiQuestionVec.size() > 0 ? true:false;
}

bool SessionAiEngine::GetAiQuestionByAiQuestionID(uint64 aiQuestionID,ai_engine_question &question)const
{
    AiEngineQuestionsMapCIt cit = m_AiEngineQuestionsMap.find(aiQuestionID);
    if (cit != m_AiEngineQuestionsMap.end())
    {
        question = cit->second;
        return true;
    }
    return false;
}

bool SessionAiEngine::GetBestAiQuestionByQuestion(const std::string& strReqQuestion,uint32 appid,ai_engine_question &question)
{
    std::vector<ai_engine_question> aiQuestionVec;
    GetAiQuestionListByReqQuestion(strReqQuestion,appid,aiQuestionVec);
    if (aiQuestionVec.size() > 0)
    {
        question = *aiQuestionVec.begin();
        return true;
    }
    else
    {
//        LOG4CPLUS_TRACE_FMT(m_Logger,"%s() strQuestion(%s),no match question",__FUNCTION__,strQuestion.c_str());
    }
    return false;
}

bool SessionAiEngine::GetClassifyListByReqQuestion(const std::string& strReqQuestion,
                std::vector<classify_question_type> &oClassifyQuestionVec)
{
    oClassifyQuestionVec.clear();
    m_wordTypeCounterMap.clear();
    std::vector<std::string> words;//分词
    SearchWords(strReqQuestion,words);//O(L),L为strReqQuestion长度   ,请求的问题匹配的分词
    if (words.size() > 0)
    {
        for(uint32 i = 0;i < words.size();++i)//请求问题的分词数J
        {//分词对应的类型统计
            const std::string& word = words[i];
            if (word.size() > 0)
            {
                uint64 wordid = lnet::HashStrToUint64(word.c_str(),word.size());
                AiEngineWordsTypeMapIt wordsTypeMapIt = m_AiEngineWordsTypeMap.find(wordid);
                if (wordsTypeMapIt != m_AiEngineWordsTypeMap.end())
                {
                    WordTypeCounterMapIt wordTypeCounterMapcit = m_wordTypeCounterMap.find(wordsTypeMapIt->second);//wordType -> words set
                    if (wordTypeCounterMapcit == m_wordTypeCounterMap.end())
                    {
                        word_type_match oWordTypeMatch;
                        oWordTypeMatch.words.push_back(word);
                        m_wordTypeCounterMap.insert(std::make_pair(wordsTypeMapIt->second,oWordTypeMatch));
                    }
                    else
                    {
                        wordTypeCounterMapcit->second.words.push_back(word);
                    }
//                    LOG4CPLUS_TRACE_FMT(m_Logger,"%s() word(%s),wordType(%s)",__FUNCTION__,word.c_str(),wordsTypeMapIt->second.c_str());
                }
            }
        }
    }
    if (m_wordTypeCounterMap.size() > 0)
    {
        WordTypeCounterMapCIt cit = m_wordTypeCounterMap.begin();
        WordTypeCounterMapCIt citend = m_wordTypeCounterMap.end();
        for(;cit != citend;++cit)
        {
            classify_question_type oClassifyQuestion;
            oClassifyQuestion.strReqQuestionType = cit->first;
            oClassifyQuestion.words = cit->second.words;
            oClassifyQuestionVec.push_back(oClassifyQuestion);
        }
        if (oClassifyQuestionVec.size() > 0)
        {
            std::sort(oClassifyQuestionVec.begin(),oClassifyQuestionVec.end());
        }
    }
    return oClassifyQuestionVec.size() > 0 ? true: false;
}

bool SessionAiEngine::SortSessionMessagesLog(std::vector<session_messages_log> &messageLogs,uint32 limit)
{
    LOG4CPLUS_DEBUG_FMT(m_Logger,"%s()",__FUNCTION__);
    /*
     message tb_session_messages_log
    {
        uint64 session_id = 1;//  BIGINT(24) UNSIGNED 会话id
        uint64 msgid = 2;//   BIGINT(24) UNSIGNED 微秒级时间消息戳，非空字段,默认值为0
        uint32 appid = 3;//   INT(11) UNSIGNED    应用id
        uint32 send_userid = 4;// INT(11) UNSIGNED    发送者id，默认值为0
        bytes send_nickname = 5;//   VARCHAR(128)    用户昵称，编码格式为UTF-8;默认值为NULL
        bytes send_avatar = 6;// VARCHAR(1024)   发送者头像url地址，编码格式为UTF-8;默认值为NULL
        uint32 send_type = 7;//   TINYINT(4) UNSIGNED 用户类型 1:游客，2：企业用户，3：客服，4：管理员 5：超级管理员
        uint32 recv_userid = 8;// INT(11) UNSIGNED    接受者id，默认值为0
        bytes recv_nickname = 9;//   VARCHAR(128)    用户昵称，编码格式为UTF-8;默认值为NULL
        bytes recv_avatar = 10;// VARCHAR(1024)   用户头像url地址，编码格式为UTF-8;默认值为NULL
        uint32 recv_type = 11;//   TINYINT(4) UNSIGNED 用户类型 1:游客，2：企业用户，3：客服，4：管理员 5：超级管理员
        uint32 msg_type = 12;//    SMALLINT(6) UNSIGNED    消息指令
        bytes  body = 13;//    BLOB    消息序列化为字符串json
        uint32 record_time = 14;// DATETIME    该条记录的生成时间;默认值为 '1970-01-01 08:00:00'

        bytes category = 15;//类别(只在统计时使用)
    }*/
    std::vector<session_messages_log>::iterator it = messageLogs.begin();
    std::vector<session_messages_log>::iterator itEnd = messageLogs.end();
    for(;it != itEnd;++it)
    {
        behaviour_common::tb_session_messages_log& tb_session_messages_log = it->message;
        std::vector<std::string> words;//分词
        SearchWords(tb_session_messages_log.body(),words);//O(L),L为strReqQuestion长度   ,请求的问题匹配的分词（重复的分词也会计算，因为要计算词频）
        m_questionIDCounterMap.clear();
        if (words.size() > 0)
        {
            //大致 复杂度 O(J * ln2N * ln2K *B)
            //K为问题数,J为问题的分词数
            for(uint32 i = 0;i < words.size();++i)//请求问题的分词数J
            {
                const std::string& word = words[i];
                if (word.size() > 0)
                {
                    uint64 wordid = lnet::HashStrToUint64(word.c_str(),word.size());
                    {//该分词对应的问题
                        WordID2AIquestionsMapIter iter = m_wordID2questionsMap.find(wordid);//分词获取问题lnN,J为请求问题的分词数
                        if (iter != m_wordID2questionsMap.end())
                        {
                            //一个单词匹配的问题越多计数也越多
                            const std::set<ai_engine_question>& questionIDSet = iter->second;//匹配分词对应的ai问题集合
                            std::set<ai_engine_question>::const_iterator questionIDSetIt = questionIDSet.begin();
                            std::set<ai_engine_question>::const_iterator questionIDSetItEnd = questionIDSet.end();
                            for(;questionIDSetIt != questionIDSetItEnd;++questionIDSetIt)//问题ID出现的次数计数lnC(C为某分词中的不同问题个数) * B（B为某分词对应问题个数）
                            {
                                if (tb_session_messages_log.appid() == questionIDSetIt->appid)//只匹配同一appid的问题
                                {
                                    QuestionIDCounterMapIt questionIDCounterMapIt =
                                                    m_questionIDCounterMap.find(questionIDSetIt->index_id);
                                    if (questionIDCounterMapIt == m_questionIDCounterMap.end())
                                    {
                                        m_questionIDCounterMap.insert(std::make_pair(questionIDSetIt->index_id,1));
                                    }
                                    else
                                    {
                                        ++questionIDCounterMapIt->second;
                                    }
                                }
                                else
                                {
                                    LOG4CPLUS_TRACE_FMT(m_Logger,"pass question appid(%u) question(%llu,%s)",
                                                    questionIDSetIt->appid,questionIDSetIt->index_id,questionIDSetIt->question.c_str());
                                }
                            }

                        }
                    }
                }
            }
            //匹配最多单词的问题计数
            //ln2K * n
            //k问题数，A匹配的问题数
            uint32 maxMatchCounter(0);
            QuestionIDCounterMapCIt cit = m_questionIDCounterMap.begin();
            QuestionIDCounterMapCIt citEnd = m_questionIDCounterMap.end();
            for(;cit != citEnd;++cit)
            {
                if (cit->second > maxMatchCounter)
                {
                    maxMatchCounter = cit->second;
                }
            }
            tb_session_messages_log.set_max_matchwordcounter(maxMatchCounter);//匹配单词对应的问题数(包括多个相同单词，每个单词可对应多个问题)
            LOG4CPLUS_TRACE_FMT(m_Logger,"%s() SortSessionMessagesLog body(%s) maxMatchCounter(%u)",
                            __FUNCTION__,tb_session_messages_log.body().c_str(),maxMatchCounter);
        }
        else
        {
            LOG4CPLUS_TRACE_FMT(m_Logger,"%s() SortSessionMessagesLog body(%s) match no words",
                                        __FUNCTION__,tb_session_messages_log.body().c_str());
        }
    }
    {
        //O(lnA)
        //消息排序
        if (messageLogs.size() > 0)
        {
            std::sort(messageLogs.begin(),messageLogs.end());
        }
        std::vector<session_messages_log>::iterator it = messageLogs.begin();
        std::vector<session_messages_log>::iterator itEnd = messageLogs.end();
        uint32 index(0);
        for(;it != itEnd;++it,++index)
        {
            if (0 == it->message.max_matchwordcounter())
            {
                break;//没有匹配单词的消息则不需要
            }
        }
        if (index < messageLogs.size())
        {
            messageLogs.resize(index);
        }
        if (limit > 0 && messageLogs.size() > limit)
        {
            messageLogs.resize(limit);
        }
    }
    {//分类
        std::vector<classify_question_type> oClassifyQuestionVec;
        int s = messageLogs.size();
        for (int i = 0;i < s;++i)
        {
            GetClassifyListByReqQuestion(messageLogs[i].message.body(),oClassifyQuestionVec);
            if (oClassifyQuestionVec.size() > 0)
            {
                messageLogs[i].message.set_category(oClassifyQuestionVec.front().strReqQuestionType);
            }
            else
            {
                messageLogs[i].message.set_category("未分类");
            }
            LOG4CPLUS_TRACE_FMT(m_Logger,"messageLogs i(%d) category(%s)",i,messageLogs[i].message.category().c_str());
        }
    }
    return true;
}



SessionAiEngine* GetSessionAiEngine(net::OssLabor* pLabor)
{
    SessionAiEngine* pSessionAiEngine = (SessionAiEngine*) pLabor->GetSession(SESSION_AI_ENGINE_ID,"robot::SessionAiEngine");
    if (pSessionAiEngine)
    {
        return (pSessionAiEngine);
    }
    pSessionAiEngine = new SessionAiEngine(SESSION_AI_ENGINE_ID);
    if (pSessionAiEngine == NULL)
    {
        LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(), "error %d: new RobotSession() error!",
                        net::ERR_NEW);
        return (NULL);
    }
    if (pLabor->RegisterCallback(pSessionAiEngine))
    {
        if (!pSessionAiEngine->init(pLabor->GetLogger()))
        {
            pLabor->DeleteCallback(pSessionAiEngine);
            LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(), "pSessionAiEngine init error!");
            return (NULL);
        }
        LOG4CPLUS_TRACE_FMT(pLabor->GetLogger(), "register RobotSession ok!");
        return (pSessionAiEngine);
    }
    else
    {
        LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(), "register RobotSession error!");
        delete pSessionAiEngine;
        pSessionAiEngine = NULL;
    }
    return (NULL);
}


} /* namespace robot */
