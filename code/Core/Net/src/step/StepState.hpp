
#ifndef SRC_StepState_HPP_
#define SRC_StepState_HPP_
#include <map>
#include <algorithm>
#include <functional>
#include "step/Step.hpp"
#include "step/HttpStep.hpp"
#include "session/Session.hpp"
#include "NetError.hpp"
#include "storage/RedisOperator.hpp"
#include "storage/DbOperator.hpp"
#include "storage/MemOperator.hpp"

namespace net
{
#define StepStateVecSize (20)

//函数运行时间计算类
class StageClock
{
public:
	StageClock()
    {
        boInit = boStart = false;
    }
    void Init(const log4cplus::Logger &logger)
    {
    	if (!boInit)
    	{
    		m_logger = logger;
			gettimeofday(&m_clockBeginClock,NULL);
			boInit = true;
    	}
    }
    void Start(int nStage)
    {
        if(boInit && !boStart)
        {
            snprintf(m_desc,sizeof(m_desc),"stage:%d",nStage);
            StartClock();
            boStart = true;
        }
    }
    ~StageClock()
    {
        if (boInit)
        {
        	gettimeofday(&m_clockEndClock,NULL);
			float useTime=1000000*(m_clockEndClock.tv_sec-m_stageBeginClock.tv_sec)+
					m_clockEndClock.tv_usec-m_stageBeginClock.tv_usec;
			useTime/=1000;
			LOG4CPLUS_INFO_FMT(m_logger,"%s() StageClock use time(%lf) ms",__FUNCTION__,useTime);
        }
    }
    void StartClock()
    {
        gettimeofday(&m_stageBeginClock,NULL);
    }
    void EndClock()
    {
        if (boInit && boStart)
        {
            gettimeofday(&m_stageEndClock,NULL);
            float useTime=1000000*(m_stageEndClock.tv_sec-m_stageBeginClock.tv_sec)+
            		m_stageEndClock.tv_usec-m_stageBeginClock.tv_usec;
            useTime/=1000;
            LOG4CPLUS_INFO_FMT(m_logger,"%s() %s use time(%lf) ms",__FUNCTION__,m_desc,useTime);
            boStart = false;
        }
    }
    void AlarmClock()
    {
        if (boInit)
        {
            gettimeofday(&m_clockEndClock,NULL);
            float useTime=1000000*(m_clockEndClock.tv_sec-m_stageBeginClock.tv_sec)+
                    m_clockEndClock.tv_usec-m_stageBeginClock.tv_usec;
            useTime/=1000;
            LOG4CPLUS_INFO_FMT(m_logger,"%s() StageClock use time(%lf) ms",__FUNCTION__,useTime);
        }
    }
    bool boInit;
    bool boStart;
    timeval m_stageBeginClock;
    timeval m_stageEndClock;

    timeval m_clockBeginClock;
	timeval m_clockEndClock;
    char m_desc[32];
    log4cplus::Logger m_logger;
};
//参数基类（自定义参数类则继承参数基类）
struct StepStateParam
{
	StepStateParam(){};
	virtual ~StepStateParam(){};
};
//通用参数类（可根据需求自定义参数类）
struct SendToMsgShellParam:public StepStateParam
{
	SendToMsgShellParam(const tagMsgShell &stMsgShell,const std::string &strBody,uint32 cmd):
		m_stMsgShell(stMsgShell),m_strBody(strBody),m_cmd(cmd){}
	tagMsgShell m_stMsgShell;
	std::string m_strBody;
	uint32 m_cmd;
};
struct SendToIdentifyParam:public StepStateParam
{
	SendToIdentifyParam(const std::string &strToIdentify,const std::string &strBody,uint32 cmd):
		m_strToIdentify(strToIdentify),m_strBody(strBody),m_cmd(cmd){}
	std::string m_strToIdentify;
	std::string m_strBody;
	uint32 m_cmd;
};
//pb
struct SendPbToMsgShellParam:public StepStateParam
{
	SendPbToMsgShellParam(const tagMsgShell &stMsgShell,const MsgHead& oMsgHead, const MsgBody& oMsgBody):
		m_stMsgShell(stMsgShell),m_oMsgHead(oMsgHead),m_oMsgBody(oMsgBody){}
	tagMsgShell m_stMsgShell;
	MsgHead m_oMsgHead;
	MsgBody m_oMsgBody;
};
struct SendPbToIdentifyParam:public StepStateParam
{
	SendPbToIdentifyParam(const std::string &strToIdentify,const MsgHead& oMsgHead, const MsgBody& oMsgBody):
		m_strToIdentify(strToIdentify),m_oMsgHead(oMsgHead),m_oMsgBody(oMsgBody){}
	std::string m_strToIdentify;
	MsgHead m_oMsgHead;
	MsgBody m_oMsgBody;
};
class StepState;
//通用注册函数（可根据需求自定义函数）
bool StateSendToMsgShell(StepState* state);
bool StateSendToMsgShellCallback(StepState* state);

bool StateSendToIdentify(StepState* state);
bool StateSendToIdentifyCallback(StepState* state);

bool StateSendPbToMsgShell(StepState* state);
bool StateSendPbToMsgShellCallback(StepState* state);

bool StateSendPbToIdentify(StepState* state);
bool StateSendPbToIdentifyCallback(StepState* state);
//状态访问步骤，在不同状态下可访问网络接口，在网路接口结果到达时，会进入下一个状态。可根据需求设置状态运行失败钩子函数和状态运行成功钩子函数。
class StepState: public HttpStep
{
public:
	typedef bool (*StateFunc)(StepState*);
	typedef void (*FinalFunc)(StepState*);
	//开始状态步骤
	//uiTimeOutMax 超时次数
	//uiToRetry 是否超时重发 1：是 0 否
	//dTimeout 超时时间（默认配置时间）
	static bool Launch(Labor* pLabor,StepState *step,uint32 uiTimeOutMax=3,uint8 uiToRetry = 1,double dTimeout = 0.0);
	static bool Register(Labor* pLabor,StepState *step,uint32 uiTimeOutMax=3,uint8 uiToRetry = 1,double dTimeout = 0.0);
	StepState();
	StepState(const tagMsgShell& stReqMsgShell, const MsgHead& oReqMsgHead, const MsgBody& oReqMsgBody);
	StepState(const tagMsgShell& stReqMsgShell, const MsgHead& oReqMsgHead);
	StepState(const tagMsgShell& stReqMsgShell, const HttpMsg& oInHttpMsg);
    virtual ~StepState();
    void Init();
    virtual void SetStepDesc(const std::string &s){m_strStepDesc = s;}
    void AddStateFunc(StateFunc func);
    void SetSuccFunc(FinalFunc func){m_SuccFunc = func;}
    void SetFailFunc(FinalFunc func){m_FailFunc = func;}
    virtual net::E_CMD_STATUS Callback(
        const net::tagMsgShell& stMsgShell,
        const MsgHead& oInMsgHead,
        const MsgBody& oInMsgBody,
        void* data = NULL);
    virtual E_CMD_STATUS Callback(
                        const tagMsgShell& stMsgShell,
                        const HttpMsg& oHttpMsg,
                        void* data = NULL);
    virtual E_CMD_STATUS Timeout();
    virtual E_CMD_STATUS Emit(int iErrno = 0, const std::string& strErrMsg = "", const std::string& strErrShow = "");
    bool SetNextState(uint32 s)
    {
        if (s < m_uiStateVecNum && (m_StateVec[s])){m_uiNextState = s;return true;}
        return false;
    }//设置本状态的下一个状态过程
    void Finish(){m_uiState = m_uiStateVecNum;}//本状态结束后结束步骤（不需要等待回调）

    uint32 GetCurrentState()const{return m_uiState;}//当前状态
    uint32 GetLastState()const{return m_uiLastState;}//上一个状态
    uint32 GetStageNum()const{return m_uiStateVecNum;}//总状态数

	void SetTimeOutMax(uint32 uiTimeOutMax){m_uiTimeOutMax = uiTimeOutMax;}
	void SetTimeOutRetry(){m_uiTimeOutRetry = 1;}//设置重新尝试过程
	void InitClock(){m_StageClock.Init(GetLogger());}
	virtual void OnSucc(){if (m_SuccFunc) m_SuccFunc(this);}
	virtual void OnFail(){if (m_FailFunc) m_FailFunc(this);}
	log4cplus::Logger GetLogger(){return Step::GetLogger();}
	Labor* GetLabor(){return Step::GetLabor();}
	void SetData(StepStateParam* data){if(m_data)delete m_data;m_data = data;}
	void* GetData()const{return m_data;}
	//网络接口(公开接口)
	bool SendTo(const tagMsgShell& stMsgShell);
	bool SendTo(const tagMsgShell& stMsgShell, const MsgHead& oMsgHead, const MsgBody& oMsgBody);
	bool SendTo(const std::string& strIdentify, const MsgHead& oMsgHead, const MsgBody& oMsgBody);
	bool SendTo(const tagMsgShell& stMsgShell, const HttpMsg& oHttpMsg);
	bool SendToNext(const std::string& strNodeType, const MsgHead& oMsgHead, const MsgBody& oMsgBody);
	bool SendToWithMod(const std::string& strNodeType, unsigned int uiModFactor, const MsgHead& oMsgHead, const MsgBody& oMsgBody);
	bool SendToProxy(const DataMem::MemOperate* pMemOper,std::string strNodeType="PROXY");
	bool RecvFromProxy(DataMem::MemRsp &oMemRsp,bool &boMoreData);
	bool RecvFromProxy(DataMem::MemRsp &oMemRsp);
	bool SendBack(const std::string &body,int iCode=200);//构造时需传入stReqMsgShell和消息头
	//异步发送，使用默认参数类型，自定义参数类型则继承StepStateParam并自定义状态函数，参考StateSendToMsgShell，StateSendToMsgShellCallback
	void AsyncSend(const net::tagMsgShell& stMsgShell,const std::string& strBody,int iCmd,StateFunc callback = StateSendToMsgShellCallback);//使用参数SendToMsgShellParam
	void AsyncSend(const std::string &strToIdentify,const std::string& strBody,int iCmd,StateFunc callback = StateSendToIdentifyCallback);//使用参数SendToIdentifyParam
	void AsyncSend(const net::tagMsgShell& stMsgShell,const MsgHead& oMsgHead, const MsgBody& oMsgBody,StateFunc callback = StateSendPbToMsgShellCallback);//使用参数SendPbToMsgShellParam
	void AsyncSend(const std::string &strToIdentify,const MsgHead& oMsgHead, const MsgBody& oMsgBody,StateFunc callback = StateSendPbToIdentifyCallback);//使用参数SendPbToIdentifyParam

	const tagMsgShell&  GetReqMsgShell()const {return m_stReqMsgShell;}
	const MsgHead&  GetReqMsgHead()const {return m_oReqMsgHead;}
	const MsgBody&  GetReqMsgBody()const {return m_oReqMsgBody;}
	int GetErrno()const{return m_iErrno;}
	const std::string& GetError()const{return m_strErrMsg;}
	HttpMsg m_oInHttpMsg;// 请求数据
	tagMsgShell m_oResMsgShell;//响应数据
	HttpMsg m_oResHttpMsg;
	MsgHead m_oResMsgHead;
	MsgBody m_oResMsgBody;
protected:
	uint32 m_uiTimeOutCounter;//已超时次数
	uint32 m_uiTimeOutMax;//最多超时次数
	uint32 m_uiTimeOutRetry;//超时重新尝试过程

	uint32 m_uiStateVecNum;//状态过程计数
    uint32 m_uiState;//状态机
    uint32 m_uiLastState; //上一个状态机
    int m_uiNextState;//指定下一个状态机

    StepStateParam * m_data;
    StageClock m_StageClock;
    int m_iErrno;
    std::string m_strErrMsg;
    std::string m_strStepDesc;
private:
	StateFunc m_StateVec[StepStateVecSize];//状态过程函数
    FinalFunc m_SuccFunc;
    FinalFunc m_FailFunc;
};

}
typedef net::StepState::StateFunc StepStateFunc;
typedef net::StepState::FinalFunc StepFinalFunc;

//StepState回调函数中处理日志的宏
#define LOG4_FATAL_S(stage,args...) LOG4CPLUS_FATAL_FMT(stage->GetLogger(), ##args)
#define LOG4_ERROR_S(stage,args...) LOG4CPLUS_ERROR_FMT(stage->GetLogger(), ##args)
#define LOG4_WARN_S(stage,args...) LOG4CPLUS_WARN_FMT(stage->GetLogger(), ##args)
#define LOG4_INFO_S(stage,args...) LOG4CPLUS_INFO_FMT(stage->GetLogger(), ##args)
#define LOG4_DEBUG_S(stage,args...) LOG4CPLUS_DEBUG_FMT(stage->GetLogger(), ##args)
#define LOG4_TRACE_S(stage,args...) LOG4CPLUS_TRACE_FMT(stage->GetLogger(), ##args)
//StepState回调函数中处理参数的宏，使用参数对象pStageParam
#define STAGE_TEST_PARAM(ParamType,state) \
ParamType* pStageParam = (ParamType*)state->GetData();if (!pStageParam){LOG4_WARN_S(state,"pParam null");return false;}

#define STAGE_TEST_PARAM_RETURN_NULL(ParamType,state) \
ParamType* pStageParam = (ParamType*)state->GetData();if (!pStageParam){LOG4_WARN_S(state,"pParam null");return;}

#define STAGE_TEST_PARAM_LOG(ParamType,state,args...) \
ParamType* pStageParam = (ParamType*)state->GetData();if (!pStageParam){LOG4_WARN_S(state,"pParam null");return false;};\
LOG4CPLUS_TRACE_FMT(state->GetLogger(), ##args);

#define STAGE_TEST_PARAM_LOG_RETURN_NULL(ParamType,state,args...) \
ParamType* pStageParam = (ParamType*)state->GetData();if (!pStageParam){LOG4_WARN_S(state,"pParam null");return;};\
LOG4CPLUS_TRACE_FMT(state->GetLogger(), ##args);

#endif
