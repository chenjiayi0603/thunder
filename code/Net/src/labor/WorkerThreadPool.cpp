#include "labor/WorkerThreadPool.hpp"
#include <memory>

namespace net
{

namespace
{
std::unique_ptr<util::threadpool> g_thunderWorkerPool;
} // namespace

/**
 * @brief 初始化全局线程池
 * @param threadCount  目标线程数；0 = 1 线程起步
 *
 * 默认 1 线程而非 hardware_concurrency()/2 的原因（见 threadpool.h 注释）：
 * Thunder 多 Worker 进程，每个 Worker 有独立 threadpool。
 * 若每进程默认 hw/2，4 进程 × 8 线程 = 32 线程 VS 16 核，严重超订。
 * 由运营配置 worker_thread_pool_size 按需调整。
 */
void InitThunderWorkerThreadPool(unsigned short threadCount)
{
	if (g_thunderWorkerPool != nullptr)
	{
		return;
	}
	unsigned short n = threadCount == 0 ? 1 : threadCount;
	if (n > THREADPOOL_MAX_NUM)
	{
		n = THREADPOOL_MAX_NUM;
	}
	g_thunderWorkerPool = std::make_unique<util::threadpool>(n);
}

util::threadpool& ThunderWorkerThreadPool()
{
	if (g_thunderWorkerPool == nullptr)
	{
		InitThunderWorkerThreadPool(0);  // 0 = 1 线程起步
	}
	return *g_thunderWorkerPool;
}

} // namespace net
