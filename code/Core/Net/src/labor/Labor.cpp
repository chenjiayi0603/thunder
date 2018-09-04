/*******************************************************************************
 * Project:  AsyncServer
 * @file     Labor.cpp
 * @brief 
 * @author   cjy
 * @date:    2017年9月6日
 * @note
 * Modify history:
 ******************************************************************************/
#include "Labor.hpp"

namespace net
{

bool tagCoroutineArg::CoroutineYield(){return labor->CoroutineYield();}

log4cplus::Logger tagCoroutineArg::GetLogger(){labor->GetLogger();}

Labor::Labor()
{
	m_CoroutineSchedule.schedule = util::coroutine_open();
}

Labor::~Labor()
{
	if (m_CoroutineSchedule.schedule)
	{
		coroutine_close(m_CoroutineSchedule.schedule);
	}
}

bool Labor::CoroutineNewWithArg(util::coroutine_func func,tagCoroutineArg *arg)
{
	arg->labor = this;
	int coid = CoroutineNew(func,arg);
	if (coid >= 0)
	{
		auto ret = m_CoroutineScheduleArgs.insert(std::make_pair(coid,arg));
		if (!ret.second)
		{
			LOG4_ERROR("%s failed to add args(%p) for coid(%u)", __FUNCTION__,arg,coid);
			return false;
		}
		return true;
	}
	return false;
}

int Labor::CoroutineNew(util::coroutine_func func,void *ud)
{
	int coid(0);
	coid = util::coroutine_new(m_CoroutineSchedule.schedule, func, ud);
	if (coid >= 0)
	{
		m_CoroutineSchedule.coroutineIds.insert(coid);
		m_CoroutineSchedule.coroutineRunIter = m_CoroutineSchedule.coroutineIds.begin();
		LOG4_TRACE("%s coroutine coid(%u) status(%d)",__FUNCTION__,coid,util::coroutine_status(m_CoroutineSchedule.schedule,coid));
	}
	else
	{
		LOG4_ERROR("%s coroutine invalid coid(%u)", __FUNCTION__, coid);

	}
	return coid;
}

bool Labor::CoroutineResumeWithTimes(int nMaxTimes)
{
	while (nMaxTimes > 0 && CoroutineResume()) --nMaxTimes;
	return (nMaxTimes > 0 ? false:true);
}

bool Labor::CoroutineResume()
{
	LOG4_TRACE("%s current TaskSize(%u) Args size(%u)",
			__FUNCTION__,CoroutineTaskSize(),m_CoroutineScheduleArgs.size());
	tagCoroutineSchedule& coroutineSchedule = m_CoroutineSchedule;
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
		int nStatus = util::coroutine_status(coroutineSchedule.schedule,coid);
		LOG4_TRACE("%s coroutine_status coid(%d) status(%d)",__FUNCTION__,coid,nStatus);
		if (0 == nStatus)
		{
			LOG4_TRACE("%s dead coid(%d)",__FUNCTION__,coid);
			coroutineSchedule.coroutineIds.erase(coid);
			auto argIter = m_CoroutineScheduleArgs.find(coid);
			if (argIter != m_CoroutineScheduleArgs.end())
			{
				delete argIter->second;
				m_CoroutineScheduleArgs.erase(argIter);
			}
			continue;
		}
		{
			LOG4CPLUS_TRACE_FMT(GetLogger(), "%s CoroutineResume coid(%d)",__FUNCTION__,coid);
			{
				int running_id = util::coroutine_running(coroutineSchedule.schedule);
				if (running_id >= 0)//抢占式(唤醒操作时一般是不会有正在运行的协程)
				{
					LOG4_TRACE("%s coroutine_yield running_id(%d)", __FUNCTION__, running_id);
					util::coroutine_yield(coroutineSchedule.schedule);//放弃执行权
				}
				LOG4_TRACE("%s coroutine_resume coid(%d) status(%d)",
						__FUNCTION__, coid,util::coroutine_status(coroutineSchedule.schedule,coid));
				util::coroutine_resume(coroutineSchedule.schedule,coid);//执行函数
				int status = util::coroutine_status(coroutineSchedule.schedule,coid);
				if (0 == status)
				{
					LOG4_TRACE("%s dead coid(%d)",__FUNCTION__,coid);
					coroutineSchedule.coroutineIds.erase(coid);
					auto argIter = m_CoroutineScheduleArgs.find(coid);
					if (argIter != m_CoroutineScheduleArgs.end())
					{
						delete argIter->second;
						m_CoroutineScheduleArgs.erase(argIter);
					}
				}
			}
			++coroutineSchedule.coroutineRunIter;
			return true;
		}
	}
	LOG4CPLUS_DEBUG_FMT(GetLogger(), "no co to run");
	return false;
}

bool Labor::CoroutineYield()
{
    util::schedule* schedule = m_CoroutineSchedule.schedule;
    int running_id = util::coroutine_running(schedule);
    if (running_id >= 0)
    {
        LOG4_TRACE("%s coroutine_yield running_id(%d) status(%d)",
                __FUNCTION__, running_id,coroutine_status(schedule,running_id));
        util::coroutine_yield(schedule);//放弃执行权
        return true;
    }
    else
    {
        LOG4_WARN("%s no running coroutine", __FUNCTION__);
        return false;
    }
    return true;
}

int Labor::CoroutineRunning()
{
	int running_id(0);
	running_id = util::coroutine_running(m_CoroutineSchedule.schedule);
	LOG4_TRACE("%s coroutine_status running_id(%d)",__FUNCTION__, running_id);
	return running_id;
}

uint32 Labor::CoroutineTaskSize()
{
	return m_CoroutineSchedule.coroutineIds.size();
}
int Labor::CoroutineStatus(int coid)
{
	if (coid >= 0)
	{
		return util::coroutine_status(m_CoroutineSchedule.schedule,coid);
	}
	LOG4_WARN("%s invalid coid(%d)", __FUNCTION__,coid);
	return 0;
}

bool Labor::IsCoroutineEnable()
{
	return true;
}

bool Labor::CoroutineResume(int coid)
{
	if (coid >= 0)
	{
		util::coroutine_resume(m_CoroutineSchedule.schedule,coid);
		int status = util::coroutine_status(m_CoroutineSchedule.schedule,coid);
		if (0 == status)
		{
			m_CoroutineSchedule.coroutineIds.erase(coid);
		}
		return true;
	}
	LOG4_WARN("%s invalid coid(%d) coroutineName(%u)", __FUNCTION__,coid);
	return false;
}

} /* namespace net */
