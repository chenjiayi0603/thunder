#include "labor/WorkerThreadPool.hpp"
#include <algorithm>

namespace net
{

namespace
{
std::threadpool* g_thunderWorkerPool = nullptr;
} // namespace

void InitThunderWorkerThreadPool(unsigned short threadCount)
{
	if (g_thunderWorkerPool != nullptr)
	{
		return;
	}
	unsigned short n = threadCount == 0 ? 4 : threadCount;
	if (n > THREADPOOL_MAX_NUM)
	{
		n = THREADPOOL_MAX_NUM;
	}
	g_thunderWorkerPool = new std::threadpool(n);
}

std::threadpool& ThunderWorkerThreadPool()
{
	if (g_thunderWorkerPool == nullptr)
	{
		InitThunderWorkerThreadPool(4);
	}
	return *g_thunderWorkerPool;
}

} // namespace net
