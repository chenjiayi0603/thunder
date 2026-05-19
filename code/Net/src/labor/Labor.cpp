/*******************************************************************************
 * Project:  Net
 * @file     Labor.cpp
 * @brief 
 * @author   cjy
 * @date:    2017年9月6日
 * @note
 * Modify history:
 ******************************************************************************/
#include <memory>
#include <cctype>
#include "../NetDefine.hpp"
#include "../NetError.hpp"
#include "labor/Labor.hpp"
#include "step/Step.hpp"
#include "logger/CustomLogger.hpp"

#include "util/IpUtil.hpp"
#include "util/json/CJsonObject.hpp"
#include "EvIoBackend.hpp"
#include "NativeUringIoBackend.hpp"
#ifdef THUNDER_IO_ASIO_URING
#include "AsioUringIoBackend.hpp"
#endif
#ifdef THUNDER_IO_DPDK
#include "DpdkIoBackend.hpp"
#endif

//每个进程只有一个labor，使用单例模式
net::Labor* g_pLabor = nullptr;
net::Labor* GetLabor() {return g_pLabor;}
const net::Labor* GetCLabor() {return g_pLabor;}

namespace net
{
namespace
{
void TrimAscii(std::string& s)
{
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
    {
        ++i;
    }
    s.erase(0, i);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
    {
        s.pop_back();
    }
}

void ToUpperAscii(std::string& s)
{
    for (char& c : s)
    {
        if (c >= 'a' && c <= 'z')
        {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
}

int32 SanitizeLogLevelInt(int32 v)
{
    switch (v)
    {
        case log4cplus::TRACE_LOG_LEVEL:
        case log4cplus::DEBUG_LOG_LEVEL:
        case log4cplus::INFO_LOG_LEVEL:
        case log4cplus::WARN_LOG_LEVEL:
        case log4cplus::ERROR_LOG_LEVEL:
        case log4cplus::FATAL_LOG_LEVEL:
            return v;
        default:
            return log4cplus::INFO_LOG_LEVEL;
    }
}

bool BuiltinNameToLog4cplusLevel(const std::string& keyUpper, int32& out)
{
    if (keyUpper == "TRACE")
    {
        out = log4cplus::TRACE_LOG_LEVEL;
        return true;
    }
    if (keyUpper == "DEBUG")
    {
        out = log4cplus::DEBUG_LOG_LEVEL;
        return true;
    }
    if (keyUpper == "INFO")
    {
        out = log4cplus::INFO_LOG_LEVEL;
        return true;
    }
    if (keyUpper == "WARN")
    {
        out = log4cplus::WARN_LOG_LEVEL;
        return true;
    }
    if (keyUpper == "ERROR")
    {
        out = log4cplus::ERROR_LOG_LEVEL;
        return true;
    }
    if (keyUpper == "FATAL")
    {
        out = log4cplus::FATAL_LOG_LEVEL;
        return true;
    }
    return false;
}
} // namespace

bool ResolveLogLevelFromConf(const util::CJsonObject& oJsonConf, int32& ioLogLevel)
{
    int32 v = 0;
    if (oJsonConf.Get("log_level", v))
    {
        ioLogLevel = SanitizeLogLevelInt(v);
        return true;
    }
    std::string s;
    if (!oJsonConf.Get("log_level", s))
    {
        return false;
    }
    TrimAscii(s);
    if (s.empty())
    {
        return false;
    }
    ToUpperAscii(s);
    if (BuiltinNameToLog4cplusLevel(s, v))
    {
        ioLogLevel = v;
        return true;
    }
    return false;
}

Labor::Labor()
{
	g_pLabor = this;
}

Labor::~Labor()
{
	delete m_pCatClientConnent; m_pCatClientConnent = nullptr;
}

void  Labor::SetSocketAttr(int iFd,bool boNeedKeepInterval)
{
	 /* tcp连接检测 与发送设置*/
	int iKeepAlive = 1;// 非0值，开启keepalive属性
	int iKeepIdle = 60;// 如该连接在60秒内没有任何数据往来,则进行此TCP层的探测
	int iKeepInterval = 5;// 探测发包间隔为5秒
	int iKeepCount = 3; // 尝试探测的次数.如果第1次探测包就收到响应了,则后2次的不再发
	int iTcpNoDelay = 1;
	if (setsockopt(iFd, SOL_SOCKET, SO_KEEPALIVE, static_cast<const void*>(&iKeepAlive), sizeof(iKeepAlive)) < 0)
	{
		LOG4_WARN("fail to set SO_KEEPALIVE");
	}
	if (boNeedKeepInterval)
	{
		if (setsockopt(iFd, SOL_TCP, TCP_KEEPIDLE, static_cast<const void*>(&iKeepIdle), sizeof(iKeepIdle)) < 0)
		{
			LOG4_WARN("fail to set SO_KEEPIDLE");
		}
		if (setsockopt(iFd, SOL_TCP, TCP_KEEPINTVL, static_cast<const void*>(&iKeepInterval), sizeof(iKeepInterval)) < 0)
		{
			LOG4_WARN("fail to set SO_KEEPINTVL");
		}
		if (setsockopt(iFd, SOL_TCP, TCP_KEEPCNT, static_cast<const void*>(&iKeepCount), sizeof (iKeepCount)) < 0)
		{
			LOG4_WARN("fail to set SO_KEEPALIVE");
		}
	}
#ifndef ENABLE_NAGLE
	if (setsockopt(iFd, IPPROTO_TCP, TCP_NODELAY, static_cast<const void*>(&iTcpNoDelay), sizeof(iTcpNoDelay)) < 0)
	{
		LOG4_WARN("fail to set TCP_NODELAY");
	}
#endif
}

bool Labor::Fd2Address(int iAiFamily,int iAcceptFd,std::string &strClientAddr)
{
	if (AF_INET == iAiFamily)
	{
		char szClientAddr[64] = {0};
		int z;                          /* status return code */
		struct sockaddr_in stClientAddr;
		socklen_t iClientAddrSize = sizeof(stClientAddr);
		z = getpeername(iAcceptFd, (struct sockaddr *)&stClientAddr, &iClientAddrSize);
		if (z == 0)
		{
			inet_ntop(AF_INET, &stClientAddr.sin_addr, szClientAddr, sizeof(szClientAddr));
			LOG4_TRACE("set fd %d's remote addr \"%s\"", iAcceptFd, szClientAddr);
			strClientAddr = std::move(std::string(szClientAddr));
			return true;
		}
		else
		{
			LOG4_ERROR("getpeername error %d", errno);
		}
	}
	else if (AF_INET6 == iAiFamily)  // AF_INET6
	{
		char szClientAddr[64] = {0};
		int z;                          /* status return code */
		struct sockaddr_in6 stClientAddr;
		socklen_t iClientAddrSize = sizeof(stClientAddr);
		z = getpeername(iAcceptFd, (struct sockaddr *)&stClientAddr, &iClientAddrSize);
		if (z == 0)
		{
			inet_ntop(AF_INET6, &stClientAddr.sin6_addr, szClientAddr, sizeof(szClientAddr));
			LOG4_TRACE("set fd %d's remote addr \"%s\"", iAcceptFd, szClientAddr);
			strClientAddr = std::move(std::string(szClientAddr));
			return true;
		}
		else
		{
			LOG4_ERROR("getpeername error %d", errno);
		}
	}
	return false;
}


bool Labor::HostPort2SockAddr(const std::string& strHost, int iPort,struct sockaddr &addr,int &iFd,bool boPassive)
{
	struct addrinfo stAddrHints;
	struct addrinfo* pAddrResult;
	struct addrinfo* pAddrCurrent;
	memset(&stAddrHints, 0, sizeof(struct addrinfo));
	stAddrHints.ai_family = AF_UNSPEC; /* Allow IPv4 or IPv6 */
	stAddrHints.ai_socktype = SOCK_STREAM;
	stAddrHints.ai_protocol = IPPROTO_IP;
	if (boPassive)
	{
		stAddrHints.ai_flags = AI_PASSIVE;              /* For wildcard IP address */
	}
	int iCode = getaddrinfo(strHost.c_str(), std::to_string(iPort).c_str(), &stAddrHints, &pAddrResult);
	if (0 != iCode)
	{
		LOG4_ERROR("getaddrinfo(\"%s\", \"%d\") error %d: %s",strHost.c_str(), iPort, iCode, gai_strerror(iCode));
		return(false);
	}
	for (pAddrCurrent = pAddrResult;pAddrCurrent != nullptr; pAddrCurrent = pAddrCurrent->ai_next)
	{
		iFd = socket(pAddrCurrent->ai_family,pAddrCurrent->ai_socktype, pAddrCurrent->ai_protocol);//socket(AF_INET, SOCK_STREAM, 0);
		if (iFd == -1)
		{
			continue;
		}
		break;
	}
	if (pAddrCurrent == nullptr)/* No address succeeded */
	{
		LOG4_ERROR("Could not connect to \"%s:%d\"", strHost.c_str(), iPort);
		freeaddrinfo(pAddrResult);
		return(false);
	}
	addr.sa_family = pAddrCurrent->ai_addr->sa_family;
	memcpy(addr.sa_data,pAddrCurrent->ai_addr->sa_data,sizeof(addr.sa_data));
	freeaddrinfo(pAddrResult);

	x_sock_set_block(iFd, 0);
	int reuse = 1;
	int iTcpNoDelay = 1;
	if (::setsockopt(iFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
	{
		LOG4_WARN("fail to set SO_REUSEADDR");
	}
#ifndef ENABLE_NAGLE
	if (setsockopt(iFd, IPPROTO_TCP, TCP_NODELAY, static_cast<const void*>(&iTcpNoDelay), sizeof(iTcpNoDelay)) < 0)
	{
		LOG4_WARN("fail to set TCP_NODELAY");
	}
#endif
	return(true);
}

const std::string& Labor::GetHostName()
{
	if (m_strHostName.size() == 0)
	{
		 if (!util::GetHostName(m_strHostName))
		 {
			 strerror_r(errno, m_pErrBuff, gc_iErrBuffLen);
		 }
	}
	return m_strHostName;
}

const std::string& Labor::GetHostForServer()
{
	if (m_strHostForServer.size() == 0 && GetHostName().size() > 0)
	{
		std::string strError;
		if (!util::DomainToIP(m_strHostName,nullptr,m_strHostForServer,strError))
		{
			snprintf(m_pErrBuff,sizeof(m_pErrBuff),"%s",strError.c_str());
	//		std::cerr << "DomainToIP "<< strError <<" error!" << std::endl;
		}
	}
	return m_strHostForServer;
}

const std::string& Labor::GetHostForClient()
{
	if (m_strHostForClient.size() == 0 && GetHostName().size() > 0)
	{
		std::string strError;
		if (!util::DomainToIP(m_strHostName,nullptr,m_strHostForClient,strError))
		{
			snprintf(m_pErrBuff,sizeof(m_pErrBuff),"%s",strError.c_str());
		}
	}
	return m_strHostForClient;
}



void Labor::ResetLogLevel(log4cplus::LogLevel iLogLevel)
{
    m_oLogger.setLogLevel(iLogLevel);
}

bool Labor::InitLogger(const util::CJsonObject& oJsonConf)
{
    if (m_bInitLogger)  // 已经被初始化过，只修改日志级别
    {
        int32 iLogLevel = log4cplus::INFO_LOG_LEVEL;
        if (ResolveLogLevelFromConf(oJsonConf, iLogLevel))
        {
        	m_oLogger.setLogLevel(iLogLevel);
        }
        return(true);
    }
    else
    {
        int32 iMaxLogFileSize = 0;
        int32 iMaxLogFileNum = 0;
        int32 iLogLevel = log4cplus::INFO_LOG_LEVEL;
        int32 iLoggingPort = 9000;
        std::string strLoggingHost;
        std::string strLogname = oJsonConf("log_path") + std::string("/") + getproctitle() + std::string(".log");
        std::string strParttern = "[%D,%d{%q}][%p] [%l] %m%n";

		(void)ResolveLogLevelFromConf(oJsonConf, iLogLevel);
        oJsonConf.Get("max_log_file_size", iMaxLogFileSize);
        oJsonConf.Get("max_log_file_num", iMaxLogFileNum);
        log4cplus::initialize();
        log4cplus::SharedAppenderPtr file_append(new log4cplus::RollingFileAppender(strLogname, iMaxLogFileSize, iMaxLogFileNum));
        file_append->setName(strLogname);
        std::unique_ptr<log4cplus::Layout> layout(new log4cplus::PatternLayout(strParttern));
        file_append->setLayout(std::move(layout));
        //log4cplus::Logger::getRoot().addAppender(file_append);
        m_oLogger = log4cplus::Logger::getInstance(strLogname);
        m_oLogger.setLogLevel(iLogLevel);
        m_oLogger.addAppender(file_append);
        if (oJsonConf.Get("socket_logging_host", strLoggingHost) && oJsonConf.Get("socket_logging_port", iLoggingPort))
        {
        	std::string strServerName = std::string(getproctitle())  + " " +  m_strHostForServer + ":" + std::to_string(m_iPortForServer);
        	std::ostringstream ssServerName;
			ssServerName << strServerName;
            log4cplus::SharedAppenderPtr socket_append(new log4cplus::SocketAppender(strLoggingHost, iLoggingPort, ssServerName.str()));
            socket_append->setName(ssServerName.str());
            socket_append->setLayout(std::unique_ptr<log4cplus::Layout>(new log4cplus::PatternLayout(strParttern)));
            socket_append->setThreshold(log4cplus::INFO_LOG_LEVEL);
            m_oLogger.addAppender(socket_append);
        }
        m_bInitLogger = true;
        return(true);
    }
}

bool Labor::InitDataLogger(const util::CJsonObject& oJsonConf)
{
	if (m_bDataInitLogger)  // 已经被初始化过，只修改日志级别
	{
		util::CJsonObject oDataLog;
		if (oJsonConf.Get("data_log",oDataLog))
		{
			int32 iLogLevel = 0;
			if (oDataLog.Get("data_log_level", iLogLevel))
			{
				m_oDataLogger.setLogLevel(iLogLevel);
			}
		}
		return(true);
	}
	else
	{
		util::CJsonObject oDataLog;
		if (oJsonConf.Get("data_log",oDataLog))
		{
			int64 iMaxLogFileSize = 10*1024*1024; // 10 MB
			int32 iMaxLogFileNum = 10;
			int32 iMaxHistory = 24 * 2;//48
			int32 iLogLevel = log4cplus::INFO_LOG_LEVEL;
			uint32 iLogschedule = static_cast<uint32>(log4cplus::DailyRollingFileSchedule::HOURLY);//enum class DailyRollingFileSchedule
			std::string strDataLogPath = "data/";
			std::string strParttern = "%m%n";
			std::string fileExt = ".data";
			int iStartTryCleanFile = 1, iMaxTryCleanFile = 3;
			bool bCreateDirs = false;
			bool bRollOnClose = false;
			bool bImmediateFlush = true;
			oDataLog.Get("data_log_path",strDataLogPath);
			oDataLog.Get("data_log_parttern",strParttern);
			oDataLog.Get("data_max_log_file_size", iMaxLogFileSize);
			oDataLog.Get("data_max_log_file_num", iMaxLogFileNum);
			oDataLog.Get("data_max_history", iMaxHistory);
			oDataLog.Get("data_log_level", iLogLevel);
			oDataLog.Get("data_create_dirs",bCreateDirs);
			oDataLog.Get("data_roll_on_close",bRollOnClose);
			oDataLog.Get("data_immediate_flush",bImmediateFlush);
			oDataLog.Get("data_start_try_clean_file",iStartTryCleanFile);
			oDataLog.Get("data_max_try_clean_file",iMaxTryCleanFile);

			std::string strLogPreName = strDataLogPath + std::string("/") + getproctitle() + ".";// + std::string(".data");

			log4cplus::initialize();
			log4cplus::SharedAppenderPtr file_append(new netcustomlog4cplus::FixDailyRollingFileAppender(
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
			m_oDataLogger = log4cplus::Logger::getInstance(strLogPreName);
			m_oDataLogger.setLogLevel(iLogLevel);
			m_oDataLogger.addAppender(file_append);
			LOG4_INFO("max_log_file_size(%lld) log_schedule(%u) max_log_file_num(%d)",(long long)iMaxLogFileSize,iLogschedule,iMaxLogFileNum);
			LOG4_INFO("log_level(%d) bCreateDirs(%d) bRollOnClose(%d) iStartTryCleanFile(%d) iMaxTryCleanFile(%d)",
							iLogLevel,bCreateDirs,bRollOnClose,iStartTryCleanFile,iMaxTryCleanFile);
			m_bDataInitLogger = true;
		}
		return(true);
	}
}

bool Labor::InitIoBackend(const util::CJsonObject& oJsonConf, IoCompletionCallback callback)
{
    // 先清理旧后端
    if (m_pIoBackend)
    {
        m_pIoBackend->Destroy();
        delete m_pIoBackend;
        m_pIoBackend = nullptr;
    }

    std::string strBackend;
    oJsonConf.Get("io_backend", strBackend);

    if (strBackend == "asio_uring")
    {
#ifdef THUNDER_IO_ASIO_URING
        AsioUringIoBackend* pBackend = new AsioUringIoBackend();
        if (pBackend && pBackend->Init(m_loop, callback, static_cast<void*>(this)))
        {
            m_pIoBackend = pBackend;
            LOG4_INFO("IoBackend: asio_uring initialized successfully");
            return true;
        }
        delete pBackend;
        LOG4_WARN("IoBackend: asio_uring init failed, falling back to ev");
#else
        LOG4_WARN("IoBackend: asio_uring requested but THUNDER_IO_ASIO_URING not compiled, falling back to ev");
#endif
    }

    if (strBackend == "native_uring")
    {
        NativeUringIoBackend* pBackend = new NativeUringIoBackend();
        if (pBackend && pBackend->Init(m_loop, callback, static_cast<void*>(this)))
        {
            m_pIoBackend = pBackend;
            LOG4_INFO("IoBackend: native_uring initialized successfully");
            return true;
        }
        delete pBackend;
        LOG4_WARN("IoBackend: native_uring init failed, falling back to ev");
    }

    if (strBackend == "uring")
    {
        LOG4_WARN("IoBackend: \"uring\" backend has been removed, falling back to ev");
    }

    if (strBackend == "dpdk")
    {
#ifdef THUNDER_IO_DPDK
        DpdkIoBackend* pBackend = new DpdkIoBackend();
        if (pBackend && pBackend->Init(m_loop, callback, static_cast<void*>(this)))
        {
            m_pIoBackend = pBackend;
            LOG4_INFO("IoBackend: dpdk initialized successfully");
            return true;
        }
        delete pBackend;
        LOG4_WARN("IoBackend: dpdk init failed, falling back to ev");
#else
        LOG4_WARN("IoBackend: dpdk requested but THUNDER_IO_DPDK not compiled, falling back to ev");
#endif
    }

    // 默认使用 ev 后端
    EvIoBackend* pBackend = new EvIoBackend();
    if (pBackend && pBackend->Init(m_loop, callback, static_cast<void*>(this)))
    {
        m_pIoBackend = pBackend;
        LOG4_INFO("IoBackend: ev initialized successfully");
        return true;
    }
    delete pBackend;
    LOG4_ERROR("IoBackend: failed to initialize any backend");
    return false;
}

void Labor::SkipNonsenseLetters(std::string& word)
{
	m_IgnoreChars.SkipNonsenseLetters(word);
}

void Labor::SkipFormatLetters(std::string& word)
{
	m_IgnoreChars.SkipFormatLetters(word);
}

void Labor::SkipCRLetters(std::string& word)
{
	m_IgnoreChars.SkipCRLetters(word);
}

const std::string& Labor::GetNodeIdentify()
{
	if (m_strNodeIdentify.size() < 5) // IP + port长度一定会大于这个数即可，不在乎数值是什么
	{
		char szNodeIdentify[64] = {0};
		snprintf(szNodeIdentify, 64, "%s:%d", GetHostForServer().c_str(),GetPortForServer());
		m_strNodeIdentify = szNodeIdentify;
	}
	return(m_strNodeIdentify);
}

void Labor::AddEvent(int events,ev_io* io_watcher, io_callback pFunc,int iFd)
{
	if (io_watcher)
	{
		ev_io_init (io_watcher, pFunc, iFd, events);
		ev_io_start (m_loop, io_watcher);
	}
}


void Labor::AddEvent(ev_signal* signal_watcher, signal_callback pFunc, int iSignum)
{
    if (signal_watcher)
    {
    	ev_signal_init (signal_watcher, pFunc, iSignum);
		ev_signal_start (m_loop, signal_watcher);
    }
}

void Labor::AddEvent(ev_tstamp dTimeout,ev_timer* timer_watcher, timer_callback pFunc)
{
    if (timer_watcher)
    {
    	ev_timer_init (timer_watcher, pFunc, dTimeout + ev_time() - ev_now(m_loop), 0.0);
		ev_timer_start (m_loop, timer_watcher);
    }
}

void Labor::RefreshEvent(int events,ev_io* io_watcher)
{
	if (io_watcher)
	{
		ev_io_stop(m_loop, io_watcher);
		ev_io_set(io_watcher, io_watcher->fd, events);
		ev_io_start (m_loop, io_watcher);
	}
}


void Labor::RefreshEvent(ev_tstamp dTimeout,ev_timer* timer_watcher)
{
    if (timer_watcher)
    {
    	ev_timer_stop (m_loop, timer_watcher);
		ev_timer_set (timer_watcher, dTimeout + ev_time() - ev_now(m_loop), 0);
		ev_timer_start (m_loop, timer_watcher);
    }
}

void Labor::DelEvent(ev_io* io_watcher)
{
    if (io_watcher)
    {
        ev_clear_pending(m_loop, io_watcher);
    	ev_io_stop(m_loop, io_watcher);
    }
}

void Labor::DelEvent(ev_timer* timer_watcher)
{
    if (timer_watcher)
    {
        ev_clear_pending(m_loop, timer_watcher);
        ev_timer_stop(m_loop, timer_watcher);
    }
}


void Labor::AddSignal(int iSignum,signal_callback callback)
{
	ev_signal* watcher = new ev_signal();
	watcher->data = static_cast<void*>(this);
	AddEvent(watcher,callback,iSignum);
}

void Labor::AddStep(Step* pStep,ev_tstamp dTimeout,timer_callback callback)
{
	ev_timer* pTimeoutWatcher = new ev_timer();
	if (0.0 == dTimeout)
	{
		pStep->SetTimeout(m_dStepTimeout);
	}
	else
	{
		pStep->SetTimeout(dTimeout);
	}
	pStep->m_pTimeoutWatcher = pTimeoutWatcher;
	pTimeoutWatcher->data = static_cast<void*>(pStep);
	AddEvent(pStep->GetTimeout(),pTimeoutWatcher, callback);
}

void Labor::AddSession(Session* pSession,ev_tstamp dTimeout,timer_callback callback)
{
	if (dTimeout != 0)
	{
		ev_timer* pTimeoutWatcher = new ev_timer();
		pSession->m_pTimeoutWatcher = pTimeoutWatcher;
		pTimeoutWatcher->data = static_cast<void*>(pSession);
		AddEvent(pSession->GetTimeout(),pTimeoutWatcher, callback);
	}
	else
	{
		LOG4_ERROR("%s() dTimeout == 0",__FUNCTION__);
	}
}

void PostToEventLoopAsyncCallback(struct ev_loop* /*loop*/, ev_async* w, int /*revents*/)
{
	if (w == nullptr || w->data == nullptr)
	{
		return;
	}
	Labor* pLabor = static_cast<Labor*>(w->data);
	pLabor->DrainPostToEventLoopQueue();
}

void Labor::InitPostToEventLoop()
{
	if (m_loop == nullptr || m_postToLoopStarted)
	{
		return;
	}
	ev_async_init(&m_evPostToLoop, PostToEventLoopAsyncCallback);
	m_evPostToLoop.data = static_cast<void*>(this);
	ev_async_start(m_loop, &m_evPostToLoop);
	m_postToLoopStarted = true;
}

void Labor::StopPostToEventLoop()
{
	if (!m_postToLoopStarted || m_loop == nullptr)
	{
		return;
	}
	ev_async_stop(m_loop, &m_evPostToLoop);
	m_postToLoopStarted = false;
	std::deque<std::function<void()>> junk;
	{
		std::lock_guard<std::mutex> lock(m_postToLoopMutex);
		junk.swap(m_postToLoopQueue);
	}
}

void Labor::PostToEventLoop(std::function<void()> fn)
{
	if (!fn)
	{
		return;
	}
	if (m_loop == nullptr)
	{
		LOG4_WARN("%s() m_loop is null, drop task", __FUNCTION__);
		return;
	}
	if (!m_postToLoopStarted)
	{
		LOG4_WARN("%s() InitPostToEventLoop not called, drop task", __FUNCTION__);
		return;
	}
	{
		std::lock_guard<std::mutex> lock(m_postToLoopMutex);
		m_postToLoopQueue.push_back(std::move(fn));
	}
	// 通知（唤醒）libev事件循环线程，处理 m_postToLoopQueue 队列中的回调任务
	ev_async_send(m_loop, &m_evPostToLoop);
}

void Labor::DrainPostToEventLoopQueue()
{
	std::deque<std::function<void()>> local;
	for (;;)
	{
		{
			std::lock_guard<std::mutex> lock(m_postToLoopMutex);
			if (m_postToLoopQueue.empty())
			{
				break;
			}
			local.swap(m_postToLoopQueue);
		}
		for (auto& t : local)
		{
			if (t)
			{
				t();
			}
		}
		local.clear();
	}
}

} /* namespace net */
