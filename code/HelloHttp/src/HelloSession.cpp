/*
 * HelloSession.cpp
 *
 *      author   cjy
 */
#include "HelloSession.h"
#include "log/CustomLogger.hpp"

namespace im
{

bool HelloSession::Init(const util::CJsonObject& conf)
{
    if(boInit)
    {
        return true;
    }
    LoadConfig(conf);
    boInit = true;
    return true;
}

void HelloSession::LoadConfig(const util::CJsonObject& conf)
{
	conf.Get("access_control_allow_origin",m_AccessControlAllowOrigin);
	conf.Get("access_control_allow_headers",m_AccessControlAllowHeaders);
	conf.Get("access_control_allow_methods",m_AccessControlAllowMethods);
	if (!conf.Get("valid_time_delay", m_ValidTimeDelay))
	{
		m_ValidTimeDelay = 60;
	}
	LOG4_INFO("%s valid time delay:%u",__FUNCTION__,m_ValidTimeDelay);
	SetCurrentTime();
}

bool HelloSession::GetConfig(const std::string& strFile,util::CJsonObject& conf)
{
	std::string strConfFile = net::GetConfigPath() + strFile;
	LOG4_TRACE("%s() CONF FILE = %s.",__FUNCTION__, strConfFile.c_str());
	if (!net::GetConfig(conf,strConfFile))
	{
		//配置信息流读取失败
		LOG4_ERROR("Open conf \"%s\" error!", strConfFile.c_str());
		return(false);
	}
	return true;
}

net::E_CMD_STATUS HelloSession::Timeout()
{
	util::CJsonObject conf;
	if (GetConfig("HelloDynamic.json",conf))
	{
		LoadConfig(conf);
	}
	LOG4_TRACE("program_get_rss %zu",program_get_rss());
	return net::STATUS_CMD_RUNNING;
}


void HelloSession::TestCat()
{
	LOG4_INFO("%s()",__FUNCTION__);
}

 

void HelloSession::TestRebootProcess()
{
	LOG4_INFO("%s()",__FUNCTION__);
}


void HelloSession::TestLog3()
{
	static int k = 0;
	for (int i = 0;i < 10;++i)
	{
		DATA_LOG4_INFO("%s() %s %u", __FUNCTION__,"HelloSessionTestLog3",++k);
	}
	LOG4_INFO("%s() TestLog3  done", __FUNCTION__);
}

#define TEST_DATA_LOG4_INFO(...) LOG4CPLUS_INFO_FMT(g_oDataLogger, ##__VA_ARGS__)
#define TEST_DATA_LOG4_TEACE(...) LOG4CPLUS_TRACE_FMT(g_oDataLogger, ##__VA_ARGS__)
void HelloSession::TestLog()
{
	LOG4_TRACE("%s() TestLog",__FUNCTION__);
	static log4cplus::Logger g_oDataLogger;
	if (!m_boTestLogInit)
	{
		m_boTestLogInit = true;
		int64 iMaxLogFileSize = 20480; // 10 MB
		int32 iMaxLogFileNum = 20;
		int32 iMaxHistory = 1;//48
		int32 iLogLevel = log4cplus::TRACE_LOG_LEVEL;
		std::string strDataLogPath = "data/";
		std::string strParttern = "%m%n";
		std::string fileExt = ".data";
		std::string strLogPreName = strDataLogPath + std::string("/") + getproctitle() + ".HelloSessionTestLog.";// + std::string(".data");
		uint32 iLogschedule = static_cast<uint32_t>(log4cplus::DailyRollingFileSchedule::HOURLY);//enum class DailyRollingFileSchedule
		int iStartTryCleanFile = 1, iMaxTryCleanFile = 3;
		bool bCreateDirs = false;
		bool bRollOnClose = false;
		bool bImmediateFlush = true;

		log4cplus::initialize();
		log4cplus::SharedAppenderPtr file_append(new customlog4cplus::FixDailyRollingFileAppender(
				strLogPreName,fileExt,
				(log4cplus::DailyRollingFileSchedule) iLogschedule ,
				iMaxLogFileSize,iMaxLogFileNum,iMaxHistory,
				iStartTryCleanFile,iMaxTryCleanFile,
				bImmediateFlush,bCreateDirs,bRollOnClose,""));
		//std::string strName = "MingingLog";
		file_append->setName(strLogPreName);
		std::unique_ptr<log4cplus::Layout> layout(new log4cplus::PatternLayout(strParttern));
		file_append->setLayout(std::move(layout));
		//log4cplus::Logger::getRoot().addAppender(file_append);
		g_oDataLogger = log4cplus::Logger::getInstance(strLogPreName);
		g_oDataLogger.setLogLevel(iLogLevel);
		g_oDataLogger.addAppender(file_append);
		LOG4_INFO("data_max_log_file_size(%lld) data_log_schedule(%u) data_max_log_file_num(%d) "
				"data_log_level(%d) bCreateDirs(%d) bRollOnClose(%d) "
				"iMaxHistory(%d)",
				iMaxLogFileSize,iLogschedule,iMaxLogFileNum,
				iLogLevel,bCreateDirs,bRollOnClose,
				iMaxHistory);
	}
	static int k = 0;
	for (int i = 0;i < 100;++i)
	{
		TEST_DATA_LOG4_TEACE("%s() %s %u", __FUNCTION__,"TestLog",++k);
	}
	LOG4_INFO("%s() TestLog  done", __FUNCTION__);
}

void HelloSession::TestLog2()
{
	LOG4_TRACE("%s() TestLog",__FUNCTION__);
	static log4cplus::Logger g_oDataLogger;
	if (!m_boTest2LogInit)
	{
		m_boTest2LogInit = true;
		int64 iMaxLogFileSize = 20480; // 10 MB
		int32 iMaxLogFileNum = 20;
		int32 iMaxHistory = 1;//48
		int32 iLogLevel = log4cplus::INFO_LOG_LEVEL;
		std::string strDataLogPath = "data/";
		std::string strParttern = "%m%n";
		std::string fileExt = ".data";
		std::string strLogPreName = strDataLogPath + std::string("/") + getproctitle() + ".CustomDailyRolling.";// + std::string(".data");
		uint32 iLogschedule = static_cast<uint32_t>(log4cplus::DailyRollingFileSchedule::HOURLY);//enum class DailyRollingFileSchedule
		bool bCreateDirs = false;
		bool bRollOnClose = false;
		bool bImmediateFlush = true;

		log4cplus::initialize();
		log4cplus::SharedAppenderPtr file_append(new customlog4cplus::CustomDailyRollingFileAppender(
				strLogPreName,
				(log4cplus::DailyRollingFileSchedule) iLogschedule ,
				iMaxLogFileSize,iMaxLogFileNum,
				bImmediateFlush,bCreateDirs,bRollOnClose,""));
		//std::string strName = "MingingLog";
		file_append->setName(strLogPreName);
		std::unique_ptr<log4cplus::Layout> layout(new log4cplus::PatternLayout(strParttern));
		file_append->setLayout(std::move(layout));
		//log4cplus::Logger::getRoot().addAppender(file_append);
		g_oDataLogger = log4cplus::Logger::getInstance(strLogPreName);
		g_oDataLogger.setLogLevel(iLogLevel);
		g_oDataLogger.addAppender(file_append);
	}

	static int k = 0;
	for (int i = 0;i < 10;++i)
	{
		TEST_DATA_LOG4_INFO("%s() %s %u", __FUNCTION__,"TestLog2",++k);
	}
	LOG4_INFO("%s() TestLog2  done", __FUNCTION__);
}


}
;
//name space robot
