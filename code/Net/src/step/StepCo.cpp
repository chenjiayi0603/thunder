/*
 * StepCo.cpp
 *
 *  Created on: 2020年1月9日
 *      author   cjy
 */
#include  <limits.h>
#include "NetError.hpp"
#include "NetDefine.hpp"
#include "StepCo.hpp"

namespace net
{

#ifdef USE_COROUTINE

void StepCo::AddCoroutinueFunc(FinalFunc func)
{
	if (m_uiStateVecNum < StepStateVecSize)
	{
		m_StateCoFuncVec[m_uiStateVecNum++] = func;
	}
}
E_CMD_STATUS StepCo::Emit(int iErrno , const std::string& strErrMsg , const std::string& strErrShow )
{
	LOG4_TRACE("%s() uiState(%u)",__FUNCTION__,m_uiState);
	if (0 != iErrno)
	{
		m_iErrno = iErrno;
		m_strErrMsg = strErrMsg;
		OnFail();
		LOG4_WARN("%s() Fail uiLastState(%u) uiState(%u)",__FUNCTION__,m_uiLastState,m_uiState);
		return STATUS_CMD_FAULT;
	}
	if (m_uiNextState >= 0)
    {
        //如果修改了状态则运行该状态(因为可以在回调中修改状态)
        LOG4_TRACE("%s() uiLastState(%u) next uiState(%u)",__FUNCTION__,m_uiLastState,m_uiState);
        m_uiState = m_uiNextState;
        m_uiNextState = -1;
    }
	if (m_uiState < m_uiStateVecNum)
	{
		m_uiLastState = m_uiState;
		LOG4_TRACE("%s() uiLastState(%u) uiState(%u) before run",__FUNCTION__,m_uiLastState,m_uiState);
		if (m_StateCoFuncVec[m_uiState])
		{
			if (m_curCoid == -1)
			{
				m_curCoid = GetLabor()->CoroutineNew((util::coroutine_func)m_StateCoFuncVec[m_uiState],this);
			}
			if (m_curCoid >= 0)
			{
				int s = GetLabor()->CoroutineStatus(m_curCoid);
				if (0 != s)
				{
					m_RunClock.StartClock(m_uiState);
					GetLabor()->CoroutineResume(m_curCoid);
					m_RunClock.EndClock();
					s = GetLabor()->CoroutineStatus(m_curCoid);
				}
                // COROUTINE_DEAD 0 COROUTINE_READY 1 COROUTINE_RUNNING 2 COROUTINE_SUSPEND 3
				if (0 == s)
				{
					++m_uiState;//执行完本状态，转为下一个状态
					m_curCoid = -1;
					if (m_uiState < m_uiStateVecNum && m_StateCoFuncVec[m_uiState])
					{
						LOG4_TRACE("%s() CoroutineStatus(%d) uiLastState(%u) next uiState(%u)",__FUNCTION__,s,m_uiLastState,m_uiState);
						return STATUS_CMD_RUNNING;
					}
				}
				else//未执行完则之后继续执行，还未到下一个状态
				{
					LOG4_TRACE("%s() CoroutineStatus(%d) uiLastState(%u) next uiState(%u) uiStateVecNum(%u)",
							__FUNCTION__,s,m_uiLastState,m_uiState,m_uiStateVecNum);
					return STATUS_CMD_RUNNING;
				}
			}
			else
			{
				LOG4_WARN("%s() m_curCoid(%d) uiLastState(%u) next uiState(%u) uiStateVecNum(%u)",
						__FUNCTION__,m_curCoid,m_uiLastState,m_uiState,m_uiStateVecNum);
				return STATUS_CMD_FAULT;
			}
		}
	}
	OnSucc();
	LOG4_TRACE("%s() complete uiState(%u) uiLastState(%u) uiStateVecNum(%u)",__FUNCTION__,m_uiState,m_uiLastState,m_uiStateVecNum);
	return STATUS_CMD_COMPLETED;
}

E_CMD_STATUS StepCo::Timeout()
{
    LOG4_TRACE("%s()",__FUNCTION__);
    ++m_uiTimeOutCounter;
    if (m_uiTimeOutCounter < m_uiTimeOutMax)
    {
    	if (m_uiTimeOutRetry > 0)
    	{
    		if (-1 == m_curCoid)
    		{
    			SetNextState(m_uiState - 1);
    		}
    		LOG4_WARN("%s() retry last stage. uiTimeOutCounter(%u) uiTimeOutMax(%u) uiTimeOutRetry(%u) State(%u)",
    		    		__FUNCTION__,m_uiTimeOutCounter,m_uiTimeOutMax,m_uiTimeOutRetry,m_uiState);
    		return Emit();//retry last stage
    	}
        return STATUS_CMD_RUNNING;
    }
    LOG4_ERROR("%s() uiTimeOutCounter(%u) uiTimeOutMax(%u) uiTimeOutRetry(%u) StepState(%p,%u)",
    		__FUNCTION__,m_uiTimeOutCounter,m_uiTimeOutMax,m_uiTimeOutRetry,this,m_uiState);
    OnFail();
    return STATUS_CMD_FAULT;
}

bool StepCo::CoroutineYield()
{
	if (m_StateCoFuncVec[m_uiState])//是协程函数才放弃执行权
	{
		return GetLabor()->CoroutineYield();
	}
	return true;
}

#endif

}
