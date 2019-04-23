/*
 * CoreSeekSession.h
 *
 *  Created on: 2015年11月24日
 *      Author: chen
 */
#ifndef CODE_CORESEEK_SESSION_H_
#define CODE_CORESEEK_SESSION_H_
#include <string>
#include <vector>
#include "Comm.hpp"

namespace robot
{

enum SeparateStatus
{
    eSeparateStatus_Separate = 1,
    eSeparateStatus_Total = 2,
    eSeparateStatus_All = 3,
};

class CoreSeekSession
{
public:
    CoreSeekSession(): boInit(false), m_currenttime(0),m_separate(0),m_deleteOldWordsFile(0),m_convertCode(0)
    {
    }
    ~CoreSeekSession()
    {
    }
    bool Init(const std::string &configPath, const std::string &strWorkPath,const log4cplus::Logger& oLogger);
    void Routine();
    void SetLogger(const log4cplus::Logger& oLogger){m_oLogger = oLogger;}
    const log4cplus::Logger& GetLogger(){return m_oLogger;}
private:
    bool CheckDataFiles();
    bool TransferFiles();
    log4cplus::Logger   m_oLogger;
    std::string         m_strConfigPath;
    std::string         m_strWorkPath;

    bool                boInit;
    uint64              m_currenttime; //当前时间
    lnet::CJsonObject   m_oCoreSeekCmdConf; //加载配置
    std::string         m_readWordsFilePath;//词库目录
    int                 m_separate;//是否写入分开的文件
    int                 m_deleteOldWordsFile;//删除老词库文件
    int                 m_convertCode;//转换编码(从gbk->utf8)
    std::vector<std::string> m_filesNameVec;
    std::vector<std::string> m_deletefilesNameVec;
};

}              //namespace robot

#endif /* CODE_CORESEEK_SESSION_H_ */
