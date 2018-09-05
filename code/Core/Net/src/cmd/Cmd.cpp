/*******************************************************************************
 * Project:  PluginServer
 * @file     Cmd.cpp
 * @brief 
 * @author   bwarliao
 * @date:    2017年3月6日
 * @note
 * Modify history:
 ******************************************************************************/
#include "step/Step.hpp"
#include "step/HttpStep.hpp"
#include "step/StepNode.hpp"
#include "Cmd.hpp"

namespace net
{

Cmd::Cmd()
    : m_pErrBuff(NULL), m_pLabor(0), m_pLogger(0)
{
    m_pErrBuff = new char[gc_iErrBuffLen];
    m_uiCmd = 0;
}

Cmd::~Cmd()
{
    if (m_pErrBuff != NULL)
    {
        delete[] m_pErrBuff;
        m_pErrBuff = NULL;
    }
}

const std::string& Cmd::GetWorkPath() const
{
    return(m_pLabor->GetWorkPath());
}

uint32 Cmd::GetNodeId()const
{
    return(m_pLabor->GetNodeId());
}

uint32 Cmd::GetWorkerIndex()const
{
    return(m_pLabor->GetWorkerIndex());
}

const std::string& Cmd::GetWorkerIdentify()
{
    if (m_strWorkerIdentify.size() < 5) // IP + port + worker_index长度一定会大于这个数即可，不在乎数值是什么
    {
        char szWorkerIdentify[64] = {0};
        snprintf(szWorkerIdentify, 64, "%s:%d.%d", m_pLabor->GetHostForServer().c_str(),
                        m_pLabor->GetPortForServer(), m_pLabor->GetWorkerIndex());
        m_strWorkerIdentify = szWorkerIdentify;
    }
    return(m_strWorkerIdentify);
}

const std::string& Cmd::GetNodeType() const
{
    return(m_pLabor->GetNodeType());
}

const util::CJsonObject& Cmd::GetCustomConf() const
{
    return(m_pLabor->GetCustomConf());
}

time_t Cmd::GetNowTime() const
{
    return(m_pLabor->GetNowTime());
}

bool Cmd::RegisterCallback(Step* pStep, ev_tstamp dTimeout)
{
    return(m_pLabor->RegisterCallback(pStep, dTimeout));
}

void Cmd::DeleteCallback(Step* pStep)
{
    m_pLabor->DeleteCallback(pStep);
}

bool Cmd::Pretreat(Step* pStep)
{
    return(m_pLabor->Pretreat(pStep));
}

bool Cmd::RegisterCallback(Session* pSession)
{
    return(m_pLabor->RegisterCallback(pSession));
}

void Cmd::DeleteCallback(Session* pSession)
{
    return(m_pLabor->DeleteCallback(pSession));
}

Session* Cmd::GetSession(uint64 uiSessionId, const std::string& strSessionClass)
{
    return(m_pLabor->GetSession(uiSessionId, strSessionClass));
}

Session* Cmd::GetSession(const std::string& strSessionId, const std::string& strSessionClass)
{
    return(m_pLabor->GetSession(strSessionId, strSessionClass));
}

bool Cmd::RegisterCallback(const std::string& strRedisNodeType, RedisStep* pRedisStep)
{
    return(GetLabor()->RegisterCallback(strRedisNodeType, pRedisStep));
}

bool Cmd::RegisterCallback(const std::string& strHost, int iPort, RedisStep* pRedisStep)
{
    return(GetLabor()->RegisterCallback(strHost, iPort, pRedisStep));
}
bool Cmd::SendBack(const net::tagMsgShell& oInMsgShell,const std::string &strBody,int iCode,bool gzip)
{
	HttpMsg oHttpMsg;
	if (gzip)//"Content-Encoding" "gzip"
	{
		::HttpMsg_Header* header = oHttpMsg.add_headers();
		header->set_header_name("Content-Encoding");
		header->set_header_value("gzip");
	}
	oHttpMsg.set_type(HTTP_RESPONSE);
	oHttpMsg.set_status_code(iCode);
	oHttpMsg.set_http_major(1);
	oHttpMsg.set_http_minor(1);
	oHttpMsg.set_body(strBody);
	if (!GetLabor()->SendTo(oInMsgShell, oHttpMsg))
	{
		LOG4_ERROR("send to tagMsgShell(fd %d, seq %u) error!", oInMsgShell.iFd, oInMsgShell.ulSeq);
		return false;
	}
	return true;
}
bool Cmd::SendBack(const net::tagMsgShell& oInMsgShell,const HttpMsg& oInHttpMsg,const std::string &strBody,int iCode)
{
	HttpMsg oHttpMsg;
	oHttpMsg.set_type(HTTP_RESPONSE);
	oHttpMsg.set_status_code(iCode);
	oHttpMsg.set_http_major(oInHttpMsg.http_major());
	oHttpMsg.set_http_minor(oInHttpMsg.http_minor());
	oHttpMsg.set_body(strBody);
	if (!GetLabor()->SendTo(oInMsgShell, oHttpMsg))
	{
		LOG4_ERROR("send to tagMsgShell(fd %d, seq %u) error!", oInMsgShell.iFd, oInMsgShell.ulSeq);
		return false;
	}
	return true;
}
bool Cmd::SendBack(const net::tagMsgShell& oInMsgShell,const MsgHead &oInMsgHead,const std::string &strBody)
{
	MsgHead oOutMsgHead;
	MsgBody oOutMsgBody;
	oOutMsgBody.set_body(strBody);
	oOutMsgHead.set_seq(oInMsgHead.seq());
	oOutMsgHead.set_cmd(oInMsgHead.cmd() + 1);
	oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
	if (!GetLabor()->SendTo(oInMsgShell, oOutMsgHead, oOutMsgBody))
	{
		LOG4_ERROR("send to oInMsgShell(fd %d, seq %u) error!", oInMsgShell.iFd, oInMsgShell.ulSeq);
		return false;
	}
	return true;
}
bool Cmd::AsyncStep(Step* pStep,ev_tstamp dTimeout)
{
    if (pStep == NULL)
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(),"pStep == NULL!");
        return(false);
    }
    if (!RegisterCallback(pStep,dTimeout))
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(),"RegisterCallback(pStep) error!");
        delete pStep;
        pStep = NULL;
        return(false);
    }
    if (STATUS_CMD_RUNNING != pStep->Emit(ERR_OK))
    {
        DeleteCallback(pStep);
        return(false);
    }
    return true;
}
//发送回调
bool Cmd::SendToProxyCallBack(Session* pSession,const DataMem::MemOperate* pMemOper,
        StorageCallbackSession callback,bool boPermanentSession,
        const std::string &nodeType,uint32 uiCmd)
{
    return GetLabor()->SendToProxyCallBack(pSession,pMemOper,callback,boPermanentSession,nodeType,uiCmd,-1);
}

bool Cmd::SendToProxyModCallBack(Session* pSession,const DataMem::MemOperate* pMemOper,StorageCallbackSession callback,bool boPermanentSession,int uiModFactor,
                const std::string &nodeType,uint32 uiCmd)
{
    return GetLabor()->SendToProxyCallBack(pSession,pMemOper,callback,boPermanentSession,nodeType,uiCmd,uiModFactor);
}

bool Cmd::SendToCallBack(Session* pSession,uint32 uiCmd,const std::string &strBody,
        StandardCallbackSession callback,bool boPermanentSession,
        const std::string &nodeType)
{
    return GetLabor()->SendToCallBack(pSession,uiCmd,strBody,callback,boPermanentSession,nodeType,-1);
}

bool Cmd::SendToModCallBack(Session* pSession,uint32 uiCmd,const std::string &strBody,
                StandardCallbackSession callback,bool boPermanentSession,int uiModFactor,
                const std::string &nodeType)
{
    return GetLabor()->SendToCallBack(pSession,uiCmd,strBody,callback,boPermanentSession,nodeType,uiModFactor);
}

bool Cmd::SendToProxyCallBack(net::Step* pUpperStep,const DataMem::MemOperate* pMemOper,StorageCallbackStep callback,
                const std::string &nodeType,uint32 uiCmd)
{
    return GetLabor()->SendToProxyCallBack(pUpperStep,pMemOper,callback,nodeType,uiCmd,-1);
}
bool Cmd::SendToProxyModCallBack(net::Step* pUpperStep,const DataMem::MemOperate* pMemOper,StorageCallbackStep callback,int uiModFactor,
                const std::string &nodeType,uint32 uiCmd)
{
    return GetLabor()->SendToProxyCallBack(pUpperStep,pMemOper,callback,nodeType,uiCmd,uiModFactor);
}

bool Cmd::SendToCallBack(net::Step* pUpperStep,uint32 uiCmd,const std::string &strBody,StandardCallbackStep callback,
                const std::string &nodeType)
{
    return GetLabor()->SendToCallBack(pUpperStep,uiCmd,strBody,callback,nodeType,-1);
}
bool Cmd::SendToModCallBack(net::Step* pUpperStep,uint32 uiCmd,const std::string &strBody,StandardCallbackStep callback,int uiModFactor,
                const std::string &nodeType)
{
    return GetLabor()->SendToCallBack(pUpperStep,uiCmd,strBody,callback,nodeType,uiModFactor);
}



} /* namespace net */
