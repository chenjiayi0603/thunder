
#include "Loader.hpp"

namespace net
{

Loader::Loader(const std::string& strWorkPath,const std::string& strConfFile,util::CJsonObject& oJsonConf,
		LoaderConfigVersionData::LoaderConfigVersionMM *pLoaderConfigVersionMM,bool boRestart)
{
	m_strWorkPath = strWorkPath;
	m_strConfFile = strConfFile;
	{
		auto pos = m_strConfFile.rfind("/");
		if (pos != std::string::npos)
		{
			m_strServerConfFileName = m_strConfFile.substr(pos + 1);
		}
		else
		{
			m_strServerConfFileName = m_strConfFile;
		}
	}

	if (!Init(oJsonConf))
	{
		exit(-1);
	}

	if (!CreateEvents())
	{
		exit(-2);
	}

	 
}

void Loader::GetConfig(util::CJsonObject& oJsonConf,bool boRestart)
{
	//todo 获取配置文件
}

void Loader::Run()
{
	SAFE_LOG4_INFO("%s()", __FUNCTION__);
    ev_run (m_loop, 0);
}

bool Loader::CreateEvents()
{
	m_loop = ev_loop_new(EVBACKEND_EPOLL);
	if (m_loop == nullptr)
	{
		return(false);
	}

	signal(SIGPIPE, SIG_IGN);
	// 注册信号事件
	AddSignal(SIGINT,TerminatedCallback);
	AddSignal(SIGKILL,TerminatedCallback);
	AddSignal(SIGTERM,TerminatedCallback);

	AddPeriodicTaskEvent();
	return(true);
}

bool Loader::AddPeriodicTaskEvent()
{
	SAFE_LOG4_INFO("%s()", __FUNCTION__);
    ev_timer* timeout_watcher = new ev_timer();
    timeout_watcher->data = (void*)this;
    AddEvent(1.0,timeout_watcher,PeriodicTaskCallback);//NODE_BEAT
    return(true);
}

void Loader::PeriodicTaskCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents)
{
    if (watcher->data != nullptr)
    {
    	Loader* pWorker = (Loader*)(watcher->data);
        pWorker->CheckParent();
        pWorker->RefreshEvent(1.0,watcher);//NODE_BEAT
    }
}

bool Loader::CheckParent()// 向父进程上报或者检查
{
    //LOG4_TRACE("%s()", __FUNCTION__);
    pid_t iParentPid = getppid();
    if (iParentPid == 1)    // manager进程已不存在
    {
    	SAFE_LOG4_INFO("no manager duty exist, loader exit.");
        //Destroy();
        exit(0);
    }
    return(true);
}

bool Loader::Init(util::CJsonObject& oJsonConf)
{
	SetProcessName(oJsonConf);
	m_oLastConf = m_oCurrentConf;
	m_oCurrentConf = oJsonConf;

	if (m_oLastConf.ToString() != m_oCurrentConf.ToString())
	{
		m_uiWorkerNum = strtoul(m_oCurrentConf("process_num").c_str(), NULL, 10);
		m_oCurrentConf.Get("inner_port", m_iPortForServer);

		oJsonConf.Get("io_timeout", m_dIoTimeout);
		oJsonConf.Get("step_timeout", m_dStepTimeout);

		oJsonConf.Get("node_type", m_strNodeType);

		m_oCustomConf = oJsonConf["custom"];
		m_oCustomConf.Get("cat_log_system", m_bCatLogSystem);

		InitLogger(oJsonConf);
	}
	if (nPid <= 0)
	{
		nPid = getpid();
		SAFE_LOG4_INFO("%s program begin, and work path %s. pid(%d) ppid(%d) iWorkerNum(%u) iPortForServer(%d)",
				m_strServerName.c_str(), m_strWorkPath.c_str(),nPid,getppid(),m_uiWorkerNum,m_iPortForServer);
	}
	return(true);
}

void Loader::SetProcessName(const util::CJsonObject& oJsonConf)
{
	std::string strServerName = std::move(oJsonConf("server_name"));
	if (strServerName.size() > 0 && m_strServerName != strServerName)
	{
		char szProcessName[64] = {0};
		snprintf(szProcessName, sizeof(szProcessName), "%s_Loader", strServerName.c_str());
		ngx_setproctitle(szProcessName);
		m_strServerName = strServerName;
	}
}


}
