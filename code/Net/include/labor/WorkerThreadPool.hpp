/*******************************************************************************
 * @file WorkerThreadPool.hpp
 * @brief Worker 进程内全局 std::threadpool（见 Util thread/threadpool.h），供协程池卸载等使用
 ******************************************************************************/
#ifndef NET_WORKER_THREAD_POOL_HPP_
#define NET_WORKER_THREAD_POOL_HPP_

#include "thread/work_stealing_pool.h"

namespace net
{

/**
 * @brief 幂等初始化全局线程池；threadCount==0 → 1 线程起步
 *
 * 默认 1 线程（而非 hw/2）的原因见 WorkerThreadPool.cpp 实现注释。
 * 运行态调整用 ThunderWorkerThreadPool().resize(n)
 */
void InitThunderWorkerThreadPool(unsigned short threadCount);

/** @brief 获取全局线程池引用（首次调用时 1 线程懒初始化）*/
util::WorkStealingPool& ThunderWorkerThreadPool();

} // namespace net

#endif
