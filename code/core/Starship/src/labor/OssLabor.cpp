/*******************************************************************************
 * Project:  AsyncServer
 * @file     OssLabor.cpp
 * @brief 
 * @author   cjy
 * @date:    2017年9月6日
 * @note
 * Modify history:
 ******************************************************************************/
#include "OssLabor.hpp"

namespace oss
{

OssLabor::OssLabor()
{
	m_CoroutineSchedule.schedule = loss::coroutine_open();
}

OssLabor::~OssLabor()
{
	if (m_CoroutineSchedule.schedule)
	{
		coroutine_close(m_CoroutineSchedule.schedule);
	}
}

int OssLabor::CoroutineNew(loss::coroutine_func func,void *ud)
{
	int coid(0);
	coid = loss::coroutine_new(m_CoroutineSchedule.schedule, func, ud);
	if (coid >= 0)
	{
		m_CoroutineSchedule.coroutineIds.insert(coid);
		m_CoroutineSchedule.coroutineRunIter = m_CoroutineSchedule.coroutineIds.begin();
		LOG4_TRACE("%s coroutine coid(%u) status(%d)",__FUNCTION__,coid,loss::coroutine_status(m_CoroutineSchedule.schedule,coid));
	}
	else
	{
		LOG4_ERROR("%s coroutine invalid coid(%u)", __FUNCTION__, coid);

	}
	return coid;
}

bool OssLabor::CoroutineResume()
{
	CoroutineSchedule& coroutineSchedule = m_CoroutineSchedule;
	while (coroutineSchedule.coroutineIds.size() > 0)
	{
		if (coroutineSchedule.coroutineRunIter == coroutineSchedule.coroutineIds.end())
		{
			coroutineSchedule.coroutineRunIter = coroutineSchedule.coroutineIds.begin();
		}
		int coid = *coroutineSchedule.coroutineRunIter;
		if (0 > coid)
		{
			LOG4_ERROR("%s invaid coid(%d)",__FUNCTION__,coid);
			coroutineSchedule.coroutineIds.erase(coid);
			continue;
		}
		int nStatus = loss::coroutine_status(coroutineSchedule.schedule,coid);
		LOG4_TRACE("%s coroutine_status coid(%d) status(%d)",__FUNCTION__,coid,nStatus);
		if (0 == nStatus)
		{
			LOG4_TRACE("%s dead coid(%d)",__FUNCTION__,coid);
			coroutineSchedule.coroutineIds.erase(coid);
			continue;
		}
		{
			LOG4CPLUS_TRACE_FMT(GetLogger(), "%s CoroutineResume coid(%d)",__FUNCTION__,coid);
			{
				int running_id = loss::coroutine_running(coroutineSchedule.schedule);
				if (running_id >= 0)//抢占式(唤醒操作时一般是不会有正在运行的协程)
				{
					LOG4_TRACE("%s coroutine_yield running_id(%d)", __FUNCTION__, running_id);
					loss::coroutine_yield(coroutineSchedule.schedule);//放弃执行权
				}
				LOG4_TRACE("%s coroutine_resume coid(%d) status(%d)",
						__FUNCTION__, coid,loss::coroutine_status(coroutineSchedule.schedule,coid));
				loss::coroutine_resume(coroutineSchedule.schedule,coid);//执行函数
				int status = loss::coroutine_status(coroutineSchedule.schedule,coid);
				if (0 == status)
				{
					coroutineSchedule.coroutineIds.erase(coid);
				}
			}
			++coroutineSchedule.coroutineRunIter;
			return true;
		}
	}
	LOG4CPLUS_DEBUG_FMT(GetLogger(), "no co to run");
	return false;
}

bool OssLabor::CoroutineYield()
{
    loss::schedule* schedule = m_CoroutineSchedule.schedule;
    int running_id = loss::coroutine_running(schedule);
    if (running_id >= 0)
    {
        LOG4_TRACE("%s coroutine_yield running_id(%d) status(%d)",
                __FUNCTION__, running_id,coroutine_status(schedule,running_id));
        loss::coroutine_yield(schedule);//放弃执行权
        return true;
    }
    else
    {
        LOG4_WARN("%s no running coroutine", __FUNCTION__);
        return false;
    }
    return true;
}

int OssLabor::CoroutineRunning()
{
	int running_id(0);
	running_id = loss::coroutine_running(m_CoroutineSchedule.schedule);
	LOG4_TRACE("%s coroutine_status running_id(%d)",__FUNCTION__, running_id);
	return running_id;
}

uint32 OssLabor::CoroutineTaskSize()
{
	return m_CoroutineSchedule.coroutineIds.size();
}
int OssLabor::CoroutineStatus(int coid)
{
	if (coid >= 0)
	{
		return loss::coroutine_status(m_CoroutineSchedule.schedule,coid);
	}
	LOG4_WARN("%s invalid coid(%d)", __FUNCTION__,coid);
	return 0;
}

bool OssLabor::IsCoroutineEnable()
{
	return true;
}

bool OssLabor::CoroutineResume(int coid)
{
	if (coid >= 0)
	{
		loss::coroutine_resume(m_CoroutineSchedule.schedule,coid);
		int status = loss::coroutine_status(m_CoroutineSchedule.schedule,coid);
		if (0 == status)
		{
			m_CoroutineSchedule.coroutineIds.erase(coid);
		}
		return true;
	}
	LOG4_WARN("%s invalid coid(%d) coroutineName(%u)", __FUNCTION__,coid);
	return false;
}

} /* namespace oss */
