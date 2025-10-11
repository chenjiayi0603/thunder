#ifndef SRC_Coroutine_HPP_
#define SRC_Coroutine_HPP_
#include <set>
#include <map>
#include <unordered_map>
#include "coroutine/coroutine.h"

namespace net
{

#ifdef USE_COROUTINE

/**
 * @brief 自定义协程参数需要继承tagCoroutineArg
 */
struct tagCoroutineArg
{
};

/**
 * @brief 协程调度器
 */
struct tagCoroutineSchedule
{
	tagCoroutineSchedule() = default;
	tagCoroutineSchedule(const tagCoroutineSchedule& coroutine)
	{
		schedule = coroutine.schedule;
		coroutineIds = coroutine.coroutineIds;
		coroutineRunIter = coroutine.coroutineRunIter;
	}
	const tagCoroutineSchedule& operator = (const tagCoroutineSchedule& coroutine)
	{
		schedule = coroutine.schedule;
		coroutineIds = coroutine.coroutineIds;
		coroutineRunIter = coroutine.coroutineRunIter;
		return *this;
	}
	util::schedule* schedule = nullptr;
	std::set<int> coroutineIds;
	std::set<int>::iterator coroutineRunIter;
};

/**
 * @brief 协程管理类
 */
class Coroutine
{
public:
	Coroutine();
    virtual ~Coroutine();
    /*
	 * @brief 唤醒协程
	 * @param nMaxTimes 唤醒协程次数 (最大执行协程次数，0则执行所有的协程)
	 * @return 返回true 还有需要执行的协程，返回false没有还需要执行的协程
	 * */
	bool CoroutineResumeWithTimes(unsigned int nMaxTimes=0);
	/*
	 * @brief 创建一个协程
	 * @param func 协程函数
	 * @param arg 协程参数（在协程执行完毕后自动销毁）
	 * @return 是否成功
	 * */
	bool CoroutineNewWithArg(util::coroutine_func func,tagCoroutineArg *arg);
	/*
	 * @brief 唤醒协程。自定义调用策略,轮流执行规则
	 * */
	bool CoroutineResume();
	/*
	 * @brief  唤醒指定协程
	 * */
	bool CoroutineResume(int coid);
	/*
	 * @brief  返回协程状态
	 * */
	int CoroutineStatus(int coid);
	/*
	 * @brief  返回协程数量
	 * */
	unsigned int CoroutineTaskSize();
	/*
	 * @brief  在协程函数中放弃协程的本次执行
	 * */
	bool CoroutineYield();
	/*
	 * @brief  获取正在进行的协程的id
	 * */
	int CoroutineRunning();
protected:
	/*
	 * @brief 创建一个协程
	 * @param func 协程函数
	 * @return ud 协程参数（在协程执行完毕后不会自动销毁）
	 * @return 返回协程id，失败为-1
	 * */
	int CoroutineNew(util::coroutine_func func,void *ud);
private:
	tagCoroutineSchedule m_CoroutineSchedule;
	std::unordered_map<int,tagCoroutineArg*> m_CoroutineScheduleArgs;

	friend class StepCo;
};

#endif

}

#endif /* SRC_Coroutine_HPP_ */
