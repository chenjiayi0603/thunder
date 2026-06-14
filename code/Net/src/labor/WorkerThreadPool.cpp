#include "labor/WorkerThreadPool.hpp"
#include <algorithm>
#include <memory>
#include <thread>

namespace net
{

namespace
{
std::unique_ptr<util::threadpool> g_thunderWorkerPool;
} // namespace

/// 默认线程数：从 1 开始，不够再加
/// 不自动读 hardware_concurrency() 是因为 Thunder 多进程，
/// 4 个 Worker × hw/2 = 严重超订。由 Worker 运行态按需 resize()。
static unsigned short DefaultThreadCount()
{
	return 1;
}

void InitThunderWorkerThreadPool(unsigned short threadCount)
{
	if (g_thunderWorkerPool != nullptr)
	{
		return;
	}
	unsigned short n = threadCount == 0 ? DefaultThreadCount() : threadCount;
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
		InitThunderWorkerThreadPool(0);  // 0 = auto（1 线程起步）
	}
	return *g_thunderWorkerPool;
}

void ResizeThunderWorkerThreadPool(unsigned short threadCount)
{
	if (g_thunderWorkerPool == nullptr)
	{
		InitThunderWorkerThreadPool(threadCount);
		return;
	}
	unsigned short n = threadCount == 0 ? DefaultThreadCount() : threadCount;
	if (n > THREADPOOL_MAX_NUM)
	{
		n = THREADPOOL_MAX_NUM;
	}
	g_thunderWorkerPool->resize(n);
}

} // namespace net
