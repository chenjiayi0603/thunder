/*******************************************************************************
 * @file WorkerThreadPool.hpp
 * @brief Worker 进程内全局 std::threadpool（见 Util thread/threadpool.h），供协程池卸载等使用
 ******************************************************************************/
#ifndef NET_WORKER_THREAD_POOL_HPP_
#define NET_WORKER_THREAD_POOL_HPP_

#include "thread/threadpool.h"

namespace net
{

/** @brief 幂等初始化；threadCount==0 → 1 线程起步 */
void InitThunderWorkerThreadPool(unsigned short threadCount);

/** @brief 未显式初始化时首次调用即 1 线程懒初始化 */
util::threadpool& ThunderWorkerThreadPool();

/**
 * @brief 运行时动态调整全局线程池线程数
 * @param threadCount  目标线程数；0 = 1 线程
 *
 * 增大：直接创建新 worker 线程，新 worker 从队列取任务执行。
 * 缩小：标记空闲 worker 自行退出，不影响正在执行的任务和已入队请求。
 */
void ResizeThunderWorkerThreadPool(unsigned short threadCount);

} // namespace net

#endif
