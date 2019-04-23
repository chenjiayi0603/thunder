/*
 * CoreSeekMgr.cpp
 *
 *  Created on: 2015年11月24日
 *      Author: chen
 */
#include "CoreSeekMgr.h"

namespace robot
{

CoreSeekMgr::CoreSeekMgr(const std::string& strConfFile):m_bInitLogger(false),m_strConfFile(strConfFile),m_iLogLevel(0)
{
    if (strConfFile == "")
    {
        std::cerr << "error: no config file!" << std::endl;
        exit(1);
    }
    if(!Init())
    {
        std::cerr << "CoreSeekMgr init failed!" << std::endl;
        exit(-1);
    }
}

bool CoreSeekMgr::SetProcessName(const loss::CJsonObject& oJsonConf)
{
    char szProcessName[64] = {0};
    snprintf(szProcessName, sizeof(szProcessName), "%s", oJsonConf("server_name").c_str());
    ngx_setproctitle(szProcessName);
    return(true);
}

bool CoreSeekMgr::Init()
{
    if (!loadServerConf())//加载配置文件
    {
        std::cerr << "loadServerConf() error!" << std::endl;
        return(false);
    }
    ngx_setproctitle(m_strServerName.c_str());
    daemonize(m_strServerName.c_str());
    if (!InitLogger(m_oCurrentConf))//初始化日志
    {
        std::cerr << "InitLogger error" << std::endl;
        return(false);
    }
    if(!m_CoreseekToolSession.Init(m_strConfigPath,m_strWorkPath,m_oLogger))
    {
       std::cerr << "m_CoreseekToolSession init failed!" << std::endl;
       return(false);
    }
    return(true);
}

bool CoreSeekMgr::loadServerConf()
{
    char szFilePath[256] = {0};
    if (m_strWorkPath.length() == 0)
    {
        if (getcwd(szFilePath, sizeof(szFilePath)))
        {
            m_strWorkPath = szFilePath;
            SetConfigPath();
        }
        else
        {
            std::cout << "get work dir: error"<< std::endl;
            return(false);
        }
    }
    m_oLastConf = m_oCurrentConf;
    std::ifstream fin(m_strConfFile.c_str());
    if (fin.good())
    {
        std::stringstream ssContent;
        ssContent << fin.rdbuf();
        if (!m_oCurrentConf.Parse(ssContent.str()))
        {
            ssContent.str("");
            fin.close();
            return(false);
        }
        ssContent.str("");
        fin.close();
    }
    else
    {
        std::cerr << "parse" << m_strConfFile <<": error"<< std::endl;
        return(false);
    }
    if (m_oLastConf.ToString() != m_oCurrentConf.ToString())
    {
    	// {"FATAL":50000, "ERROR":40000, "WARN":30000, "INFO":20000, "DEBUG":10000, "TRACE":0}
    	m_iLogLevel = ::atoi(m_oCurrentConf("log_level").c_str());
    }
    if(m_oCurrentConf("server_name").empty())
    {
        m_strServerName = "CoreseekTool";
    }
    else
    {
        m_strServerName = m_oCurrentConf("server_name");
    }
    return(true);
}

bool CoreSeekMgr::InitLogger(const loss::CJsonObject& oJsonConf)
{
    if (m_bInitLogger)  // 已经被初始化过，只修改日志级别
    {
        int32 iLogLevel = 0;
        oJsonConf.Get("log_level", iLogLevel);
        m_oLogger.setLogLevel(iLogLevel);
        return(true);
    }
    else
    {
        char szLogName[256] = {0};
        snprintf(szLogName, sizeof(szLogName), "%s/log/%s.log", m_strWorkPath.c_str(), getproctitle());
        std::string strParttern = "[%D,%d{%q}][%p] [%l] %m%n";
        log4cplus::initialize();
        //配置程序日志文件大小和数量
        log4cplus::SharedAppenderPtr append(new log4cplus::RollingFileAppender(
                        szLogName, atol(m_oCurrentConf("max_log_file_size").c_str()),
                        atoi(m_oCurrentConf("max_log_file_num").c_str())));
        append->setName(szLogName);
        std::auto_ptr<log4cplus::Layout> layout(new log4cplus::PatternLayout(strParttern));
        append->setLayout(layout);
        m_oLogger = log4cplus::Logger::getInstance(szLogName);
        m_oLogger.addAppender(append);
        int32 iLogLevel = 0;
        oJsonConf.Get("log_level", iLogLevel);//日志等级
        m_oLogger.setLogLevel(iLogLevel);
        LOG4CPLUS_DEBUG_FMT(m_oLogger,
                        "%s begin, and work path %s...", m_oCurrentConf("server_name").c_str(), m_strWorkPath.c_str());
        m_bInitLogger = true;
        return(true);
    }
}

void CoreSeekMgr::SetLogger(const log4cplus::Logger& oLogger)
{
	m_oLogger = oLogger;
}
const log4cplus::Logger& CoreSeekMgr::GetLogger()
{
	return m_oLogger;
}
void CoreSeekMgr::SetConfigPath()
{
	if (m_strConfigPath == "")
	{
		m_strConfigPath = m_strWorkPath + std::string("/conf");
	}
}

void CoreSeekMgr::Run()
{
    m_CoreseekToolSession.Routine();
}

}//namespace robot
