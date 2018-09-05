#ifndef SRC_StepCo_HPP_
#define SRC_StepCo_HPP_

#include "StepState.hpp"
//协程步骤
namespace net
{

//协程访问步骤，在不同状态下可访问网络接口，在网路接口结果到达时，会进入下一个状态。可根据需求设置状态运行失败钩子函数和状态运行成功钩子函数。
class StepCo: public StepState
{
    typedef StepState super;
public:
	typedef bool (*StateFunc)(StepCo*);
	typedef void (*FinalFunc)(StepCo*);
	//开始状态步骤(参考StepState)
	StepCo();
	StepCo(const tagMsgShell& stReqMsgShell, const MsgHead& oReqMsgHead, const MsgBody& oReqMsgBody);
	StepCo(const tagMsgShell& stReqMsgShell, const MsgHead& oReqMsgHead);
	StepCo(const tagMsgShell& stReqMsgShell, const HttpMsg& oInHttpMsg);
    virtual ~StepCo();
    void Init();
    void AddCoroutinueFunc(FinalFunc func);
    void SetSuccFunc(FinalFunc func){m_SuccFunc = func;}
    void SetFailFunc(FinalFunc func){m_FailFunc = func;}
    virtual E_CMD_STATUS Timeout();
    virtual E_CMD_STATUS Emit(int iErrno = 0, const std::string& strErrMsg = "", const std::string& strErrShow = "");
    bool SetNextState(uint32 s)
    {
        if (s < m_uiStateVecNum && (m_StateCoFuncVec[s])){m_uiNextState = s;return true;}
        return false;
    }
	virtual void OnSucc(){if (m_SuccFunc) m_SuccFunc(this);}
	virtual void OnFail(){if (m_FailFunc) m_FailFunc(this);}
	bool CoroutineYield();
protected:
private:
	FinalFunc m_StateCoFuncVec[StepStateVecSize];//协程状态过程函数
	int m_curCoid;
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
