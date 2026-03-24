/*******************************************************************************
 * @file WorkerThreadPool.hpp
 * @brief Worker 进程内全局 std::threadpool（见 Util thread/threadpool.h），供协程池卸载等使用
 ******************************************************************************/
#ifndef NET_WORKER_THREAD_POOL_HPP_
#define NET_WORKER_THREAD_POOL_HPP_

#include "thread/threadpool.h"

namespace net
{

/** @brief 幂等初始化；线程数限制在 THREADPOOL_MAX_NUM 内 */
void InitThunderWorkerThreadPool(unsigned short threadCount);

/** @brief 未显式初始化时首次调用会以 4 线程懒初始化 */
std::threadpool& ThunderWorkerThreadPool();

} // namespace net

#endif
