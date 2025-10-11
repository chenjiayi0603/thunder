/*******************************************************************************
 * Project:  Nebula
 * @file     FileLogger.hpp
 * @brief 
 * @author   Tommy
 * @date:    2020年5月29日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef LOGGER_FILELOGGER_HPP_
#define LOGGER_FILELOGGER_HPP_

#include <cstdio>
#include <string>
#include "Logger.hpp"

namespace net
{

class FileLogger: public Logger
{
public:
    explicit FileLogger(
            const std::string& strLogFile,
            int iLogLev = Logger::INFO,
            unsigned int uiMaxFileSize = net::gc_uiMaxLogFileSize,
            unsigned int uiMaxRollFileIndex = net::gc_uiMaxRollLogFileIndex);
    virtual ~FileLogger()
    {
#if __GNUC__ < 5
        free(m_szTime);
#endif
        fclose(m_fp);
    }

    static FileLogger* Instance(const std::string& strLogFile = "../log/default.log",
                    int iLogLev = Logger::INFO,
                    unsigned int uiMaxFileSize = net::gc_uiMaxLogFileSize,
                    unsigned int uiMaxRollFileIndex = net::gc_uiMaxRollLogFileIndex)
    {
        if (s_pInstance == nullptr)
        {
            s_pInstance = new FileLogger(strLogFile, iLogLev, uiMaxFileSize, uiMaxRollFileIndex);
        }
        return(s_pInstance);
    }

    void SetLogLevel(int iLev)
    {
        m_iLogLevel = iLev;
    }

    virtual int WriteLog(int iLev, const char* szFileName, unsigned int uiFileLine, const char* szFunction, const char* szLogStr = "info", ...);
    virtual int WriteLog(const std::string& strTraceId, int iLev, const char* szFileName, unsigned int uiFileLine, const char* szFunction, const char* szLogStr = "info", ...);

private:
    int OpenLogFile(const std::string strLogFile);
    void ReOpen();
    void RollOver();
    int Vappend(int iLev, const char* szFileName, unsigned int uiFileLine, const char* szFunction, const char* szLogStr, va_list ap);
    int Vappend(const std::string& strTraceId, int iLev, const char* szFileName, unsigned int uiFileLine, const char* szFunction, const char* szLogStr, va_list ap);

    static FileLogger* s_pInstance;

#if __GNUC__ < 5
    char* m_szTime;
#endif
    FILE* m_fp;
    int m_iLogLevel;
    unsigned int m_uiLogNum;
    unsigned int m_uiMaxFileSize;       // 日志文件大小
    unsigned int m_uiMaxRollFileIndex;  // 滚动日志文件数量
    std::string m_strLogFileBase;       // 日志文件基本名（如 log/program_name.log）
};

} /* namespace net */


#define DATA_LOG4_FATAL(...) GetLabor()->GetDataLogger()->WriteLog(net::Logger::FATAL, __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define DATA_LOG4_ERROR(...) GetLabor()->GetDataLogger()->WriteLog(net::Logger::ERROR, __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define DATA_LOG4_WARNING(...) GetLabor()->GetDataLogger()->WriteLog(net::Logger::WARNING, __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define DATA_LOG4_NOTICE(...) GetLabor()->GetDataLogger()->WriteLog(net::Logger::NOTICE, __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define DATA_LOG4_INFO(...) GetLabor()->GetDataLogger()->WriteLog(net::Logger::INFO, __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define DATA_LOG4_CRITICAL(...) GetLabor()->GetDataLogger()->WriteLog(net::Logger::CRITICAL, __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define DATA_LOG4_DEBUG(...) GetLabor()->GetDataLogger()->WriteLog(net::Logger::DEBUG, __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define DATA_LOG4_TRACE(...) GetLabor()->GetDataLogger()->WriteLog(net::Logger::TRACE, __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__)


#endif /* LOGGER_FILELOGGER_HPP_ */
