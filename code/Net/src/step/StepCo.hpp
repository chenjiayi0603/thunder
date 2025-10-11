#ifndef SRC_StepCo_HPP_
#define SRC_StepCo_HPP_

#include "StepState.hpp"

namespace net
{

#ifdef USE_COROUTINE

/**
 * @brief 协程访问步骤
 * @note 在不同状态下可访问网络接口，在网路接口结果到达时，会进入下一个状态。可根据需求设置状态运行失败钩子函数和状态运行成功钩子函数。
 */
class StepCo: public StepState
{
    typedef StepState super;
public:
	typedef bool (*StateFunc)(StepCo*);
	typedef void (*FinalFunc)(StepCo*);
	//开始状态步骤(参考StepState)
	StepCo() = default;
	StepCo(const tagMsgShell& stInMsgShell, const MsgHead& oInMsgHead):StepState(stInMsgShell,oInMsgHead){}
	StepCo(const tagMsgShell& stInMsgShell, const MsgHead& oInMsgHead, const MsgBody& oInMsgBody):StepState(stInMsgShell,oInMsgHead,oInMsgBody){}
	StepCo(const tagMsgShell& stInMsgShell, const HttpMsg& oInHttpMsg):StepState(stInMsgShell,oInHttpMsg){}
	virtual ~StepCo() = default;//在StepState析构函数回收StepState的成员
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
	virtual void OnSucc()override{if (m_SuccFunc) m_SuccFunc(this);}
	virtual void OnFail()override{if (m_FailFunc) m_FailFunc(this);}
	bool CoroutineYield();
protected:
private:
	FinalFunc m_StateCoFuncVec[StepStateVecSize] = {0};//协程状态过程函数
	int m_curCoid = -1;
    FinalFunc m_SuccFunc = nullptr;
    FinalFunc m_FailFunc = nullptr;
};

#endif

}
typedef net::StepState::StateFunc StepStateFunc;
typedef net::StepState::FinalFunc StepFinalFunc;

#endif
