
#ifndef SRC_StepNode_HPP_
#define SRC_StepNode_HPP_
#include "step/Step.hpp"
#include "step/HttpStep.hpp"
#include "session/Session.hpp"
#include "NetError.hpp"
#include "NetDefine.hpp"
#include "storage/RedisOperator.hpp"
#include "storage/DbOperator.hpp"
#include "storage/MemOperator.hpp"

namespace net
{

class StepNode: public net::Step
{
public:
    StepNode(const DataMem::MemOperate* pMemOper);
    StepNode(const std::string &strBody);
    virtual ~StepNode();
    void Init();
    virtual net::E_CMD_STATUS Callback(
        const net::tagMsgShell& stMsgShell,
        const MsgHead& oInMsgHead,
        const MsgBody& oInMsgBody,
        void* data = NULL);
    virtual net::E_CMD_STATUS Timeout();
    virtual net::E_CMD_STATUS Emit(int iErrno = 0, const std::string& strErrMsg = "", const std::string& strErrShow = "");
    //Session对象异步访问存储回调
    void SetProxyCallBack(StorageCallbackSession callback,net::Session* pSession,
    		bool boPermanentSession,const std::string &nodeType = "PROXY",uint32 uiCmd = net::CMD_REQ_STORATE,int uiModFactor=-1)
    {
    	m_storageCallbackSession = callback;
        if (boPermanentSession)
        {
            m_pSession = pSession;
        }
        else
        {
            m_strUpperSessionId = pSession->GetSessionId();
            m_strUpperSessionClassName = pSession->GetSessionClass();
        }
        m_strNodeType = nodeType;
        m_uiCmd = uiCmd;
        m_uiModFactor = uiModFactor;
    }
    //Step对象异步访问存储回调
    void SetProxyCallBack(StorageCallbackStep callback,net::Step* pUpperStep,
    		const std::string &nodeType = "PROXY",uint32 uiCmd = net::CMD_REQ_STORATE,int uiModFactor=-1)
	{
    	m_storageCallbackStep = callback;
    	m_pUpperStep = pUpperStep;
    	m_strNodeType = nodeType;
    	m_uiCmd = uiCmd;
    	m_uiModFactor = uiModFactor;
	}
    //Session对象异步访问一般节点回调
    void SetCallBack(StandardCallbackSession callback,net::Session* pSession,
    		bool boPermanentSession,const std::string &nodeType,uint32 uiCmd,int uiModFactor=-1)
    {
    	m_standardCallbackSession = callback;
		if (boPermanentSession)
		{
			m_pSession = pSession;
		}
		else
		{
			m_strUpperSessionId = pSession->GetSessionId();
			m_strUpperSessionClassName = pSession->GetSessionClass();
		}
		m_strNodeType = nodeType;
		m_uiCmd = uiCmd;
		m_uiModFactor = uiModFactor;
    }
    //Session对象异步访问一般节点回调
	void SetCallBack(StandardCallbackStep callback,net::Step* pUpperStep,
			const std::string &nodeType,uint32 uiCmd,int uiModFactor=-1)
	{
		m_standardCallbackStep = callback;
		m_pUpperStep = pUpperStep;
		m_strNodeType = nodeType;
		m_uiCmd = uiCmd;
		m_uiModFactor = uiModFactor;
	}
	void SetTimeOutMax(uint32 uiTimeOut)
	{
	    m_uiTimeOutMax = uiTimeOut;
	}
	//1:超时重发 0：超时不重发
	void SetRetrySend(uint32 uiRetrySend=1)
	{
	    m_uiRetrySend = uiRetrySend;
	}
	void SetModFactor(int uiModFactor){m_uiModFactor = uiModFactor;}
private:
	bool DecodeMemRsp(DataMem::MemRsp &oRsp,const MsgBody& oInMsgBody);

	net::Session* GetSession()
	{
		if (NULL == m_pSession)
		{
			if (m_strUpperSessionId.size() > 0 && m_strUpperSessionClassName.size() > 0)
			{
				m_pSession = GetLabor()->GetSession(m_strUpperSessionId,m_strUpperSessionClassName);
			}
		}
		return m_pSession;
	}
    uint32 m_uiTimeOut;
    uint32 m_uiTimeOutMax;
    uint32 m_uiRetrySend;
    int m_uiModFactor;

    std::string m_strNodeType;

    uint32 m_uiCmd;
    std::string m_strMsgSerial;//消息体序列化

    //回调处理
    StorageCallbackSession m_storageCallbackSession;
    StorageCallbackStep m_storageCallbackStep;

    StandardCallbackSession m_standardCallbackSession;
    StandardCallbackStep m_standardCallbackStep;

    std::string m_strUpperSessionId;
    std::string m_strUpperSessionClassName;
    net::Session* m_pSession;
    net::Step* m_pUpperStep;

    uint32 m_uiUpperStepSeq;
};
//参数基类（自定义参数类则继承参数基类）
struct DataStepParam
{
    DataStepParam(){};
    virtual ~DataStepParam(){};
};
//只是为了存储连接数据的步骤
class DataStep: public net::HttpStep
{
public:
    DataStep(DataStepParam *data=NULL):m_data(data){}
	DataStep(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,DataStepParam *data=NULL)
        :stInMsgShell(stMsgShell),m_oInHttpMsg(oInHttpMsg),m_data(data){}
	DataStep(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,DataStepParam *data=NULL)
	:stInMsgShell(stMsgShell),m_oInMsgHead(oInMsgHead),m_data(data){}

    virtual ~DataStep(){if(m_data){delete m_data;m_data = NULL;}}
    virtual E_CMD_STATUS Emit(int iErrno = 0, const std::string& strErrMsg = "", const std::string& strErrShow = ""){return net::STATUS_CMD_COMPLETED;}
    virtual E_CMD_STATUS Callback(const tagMsgShell& stMsgShell,const HttpMsg& oHttpMsg,void* data = NULL){return net::STATUS_CMD_COMPLETED;}
	virtual E_CMD_STATUS Timeout(){return net::STATUS_CMD_COMPLETED;}
	void SendBack(const std::string &data,int nCode = 200)
	{
	    if (m_oInMsgHead.cmd() == 0)
	    {
	        HttpMsg oHttpMsg;
            oHttpMsg.set_type(HTTP_RESPONSE);
            oHttpMsg.set_status_code(nCode);
            oHttpMsg.set_http_major(m_oInHttpMsg.http_major());
            oHttpMsg.set_http_minor(m_oInHttpMsg.http_minor());
            oHttpMsg.set_body(data);
            if (!GetLabor()->SendTo(stInMsgShell, oHttpMsg))
            {
                LOG4_ERROR("send to tagMsgShell(fd %d, seq %u) error!", stInMsgShell.iFd, stInMsgShell.ulSeq);
            }
	    }
	    else
	    {
	        MsgBody oMsgBody;
	        MsgHead oInMsgHead;
	        oMsgBody.set_body(data);
	        oInMsgHead.set_cmd(m_oInMsgHead.cmd()+1);
	        oInMsgHead.set_seq(m_oInMsgHead.seq());
	        oInMsgHead.set_msgbody_len(oMsgBody.ByteSize());
            if (!GetLabor()->SendTo(stInMsgShell, oInMsgHead,oMsgBody))
            {
                LOG4_ERROR("send to tagMsgShell(fd %d, seq %u) error!", stInMsgShell.iFd, stInMsgShell.ulSeq);
            }
	    }
	}
	DataStepParam * GetData(){return m_data;}
protected:
    net::tagMsgShell stInMsgShell;
    HttpMsg m_oInHttpMsg;
    MsgHead m_oInMsgHead;
    DataStepParam *m_data;
};

}


#endif
