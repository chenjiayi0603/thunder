#ifndef SRC_STEPNODE_HPP_
#define SRC_STEPNODE_HPP_
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
/**
 * @brief 节点间通信异步回调处理步骤(不在逻辑层使用)
 * @note 不在逻辑层使用，由api函数自动生成和管理
 */
class StepNode: public net::Step
{
public:
	explicit StepNode(const DataMem::MemOperate* pMemOper,StepParam* pStepParam=nullptr);
	explicit StepNode(const std::string &strBody,StepParam* pStepParam=nullptr);
    virtual ~StepNode() = default;
    virtual net::E_CMD_STATUS Callback(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,const MsgBody& oInMsgBody,void* data = nullptr);
    virtual net::E_CMD_STATUS Timeout();
    virtual net::E_CMD_STATUS Emit(int iErrno = 0, const std::string& strErrMsg = "", const std::string& strErrShow = "");

    //回调设置
    void SetCallBack(SessionCallbackMem callback,net::Session* pSession,const std::string &nodeType = REDISAGENT_NODE,const std::string& strModFactor="",uint32 uiCmd = net::CMD_REQ_STORATE);
    void SetCallBack(StepCallbackMem callback,net::Step* pUpperStep,const std::string &nodeType = REDISAGENT_NODE,const std::string& strModFactor="",uint32 uiCmd = net::CMD_REQ_STORATE);
    void SetCallBack(SessionCallback callback,net::Session* pSession,const std::string &nodeType,uint32 uiCmd,const std::string& strModFactor="");
	void SetCallBack(StepCallback callback,net::Step* pUpperStep,const std::string &nodeType,uint32 uiCmd,const std::string& strModFactor="");

	void SetCallBack(SessionCallback callback,net::Session* pSession,const tagMsgShell &stMsgShell,uint32 uiCmd,const std::string& strModFactor="");
	void SetCallBack(StepCallback callback,net::Step* pUpperStep,const tagMsgShell &stMsgShell,uint32 uiCmd,const std::string& strModFactor="");


	void SetTimeOutMax(uint32 uiTimeOut){m_uiTimeOutMax = uiTimeOut;}
	//1:超时重发 0：超时不重发
	void SetRetrySend(uint8 uiRetrySend=1){m_uiRetrySend = uiRetrySend;}
	void SetModFactor(const std::string& strModFactor){m_strModFactor = strModFactor;}
private:
	bool DecodeMemRsp(DataMem::MemRsp &oRsp,const MsgBody& oInMsgBody);
	net::Session* GetSession();

    std::string m_strNodeType;//节点类型 如SESSION, 或者 连接描述符 如192.168.11.66:27032.0
    tagMsgShell m_stMsgShell;
    std::string m_strMsgSerial;//消息体序列化
    uint32 m_uiCmd = 0;

	std::string m_strModFactor;

	uint32 m_uiTimeOut = 0;
	uint32 m_uiTimeOutMax = 3;
	uint8 m_uiRetrySend = 1;

    //回调处理
    SessionCallbackMem m_storageCallbackSession = nullptr;
    StepCallbackMem m_storageCallbackStep = nullptr;

    SessionCallback m_standardCallbackSession = nullptr;
    StepCallback m_standardCallbackStep = nullptr;

    std::string m_strUpperSessionId;
    std::string m_strUpperSessionClassName;
    net::Session* m_pSession = nullptr;
    uint32 m_uiUpperStepSeq = 0;
};

//存储数据步骤,用来步骤状态
class DataStep: public net::HttpStep
{
public:
    DataStep(StepParam *data=nullptr){SetData(data);}
	DataStep(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,StepParam *data=nullptr):net::HttpStep(stMsgShell,oInHttpMsg){SetData(data);}
	DataStep(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,StepParam *data=nullptr):net::HttpStep(stMsgShell,oInMsgHead){SetData(data);}
    virtual ~DataStep() = default;
    virtual E_CMD_STATUS Emit(int iErrno = 0, const std::string& strErrMsg = "", const std::string& strErrShow = ""){return Timeout();}
	virtual E_CMD_STATUS Timeout()
	{
		if (m_boDelayDel)
		{
			m_boDelayDel = false;
			return net::STATUS_CMD_RUNNING;
		}
		return net::STATUS_CMD_COMPLETED;
	}
};

}


#endif
