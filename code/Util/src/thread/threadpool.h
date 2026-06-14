#pragma once
#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <atomic>
#include <future>
#include <thread>
#include <functional>
#include <stdexcept>
#include "concurrentqueue.h"   // moodycamel::ConcurrentQueue — https://github.com/cameron314/concurrentqueue

//线程池最大容量,应尽量设小一点
#define  THREADPOOL_MAX_NUM 16

namespace util
{

/**
 * @brief 基于 moodycamel::ConcurrentQueue 的无锁线程池，带队列上限背压保护
 *
 * ── Thunder 中的定位 ──
 *
 * Thunder 是单线程事件循环模型（每个 Worker 一个 libev/io_uring 循环）。
 * 当业务逻辑需要执行 阻塞调用（同步 SDK、文件读写）或 CPU 密集计算时，
 * 若直接在事件循环中执行会阻塞整个 Worker，导致所有连接延迟飙升。
 *
 * threadpool 的职责就是承接这些阻塞/耗时任务，将事件循环解放出来。
 * 使用方式：co_await MakePoolOffloadAwaiter() 将任务提交到池，
 * 完成后通过 PostToEventLoop() 在事件循环上 resume 协程。
 *
 * ── 为什么是 threadpool 而不是其他方案 ──
 *
 * 方案                    | 为什么不适合 Thunder
 * ------------------------|--------------------------------------------------
 * std::execution::par     | 数据并行（对集合元素批量处理），Thunder 是任务并行
 * 每个模块开独立线程      | 线程数不可控，闲置浪费，难以统一管理
 * io_uring 全异步         | SDK 不提供异步接口；CPU 密集计算无法异步化
 * 纯协程（无池）          | 协程只在单线程内协作，不能利用多核
 * threadpool（当前方案）   | ✅ 通用任务队列 + 多 worker + 协程集成
 *
 * ── 设计要点 ──
 *
 * 1. namespace util（非 std）：原版注入 namespace std 违反 C++ 标准，已修复
 * 2. moodycamel::ConcurrentQueue：lock-free MPMC，多生产者并发入队不串行
 * 3. try_dequeue + yield：避开 mutex + condvar 的 syscall 开销
 * 4. 队列上限 + 背压（_maxQueueSize）：超限抛异常，防止 OOM，不破坏 FIFO
 * 5. resize(n) 动态扩缩容：从 1 线程起步，运营配置 worker_thread_pool_size；
 *    增大直接建新线程，缩小标记空闲 worker 自行退出，不影响执行中的任务
 *
 * ── 性能参考 ──
 *
 * 与旧版（std::queue + mutex + condvar）对比：
 *   - 4P-4C 典型 offload：2.5x 加速
 *   - 16P-4C 高并发：2.8x 加速
 * 详见 docs/performance/03-threadpool-queue-bench.md
 *
 * ── 多进程说明 ──
 *
 * Thunder 单机多 Worker 进程，每个 Worker 有独立的 threadpool 实例。
 * 默认从 1 线程起步而非 auto hardware_concurrency()/2，是因为
 * 4 个 Worker × hw/2 = 严重超订。应由运营配置 worker_thread_pool_size。
 */
class threadpool
{
public:
	/**
	 * @brief 默认队列深度倍数（队列上限 = size * kDefaultQueueDepthMultiplier）
	 *
	 * 计算依据（以 10ms/任务 估算）：
	 *   - 4 线程 × 64 = 256  →  256 × 10ms / 4 = 640ms backlog
	 *   - 8 线程 × 64 = 512  →  512 × 10ms / 8 = 640ms backlog
	 *   - 16 线程 × 64 = 1024 → 1024 × 10ms / 16 = 640ms backlog
	 * 不论线程数多少，满队列时 ~640ms 内可消化完，足够吸收突发。
	 * 内存：1024 × ~120 bytes/task ≈ 120KB，可忽略。
	 */
	static constexpr size_t kDefaultQueueDepthMultiplier = 64;

private:
	using Task = std::function<void()>;

	moodycamel::ConcurrentQueue<Task> _tasks; // 无锁 MPMC 队列
	std::vector<std::thread> _pool;           // 线程池
	std::atomic<bool> _run{ true };           // 线程池是否执行
	std::atomic<int>  _idlThrNum{ 0 };        // 空闲线程数量（近似值，供测试断言用）
	std::atomic<size_t> _queueSize{ 0 };      // 当前队列深度，用于背压检查
	size_t _maxQueueSize;                     // 队列上限，构造函数设定
	std::atomic<int> _excessThreads{ 0 };     // 应退出的多余线程数（resize 缩小用）
	std::atomic<int> _totalCreated{ 0 };      // 累计创建线程数（addThread）
	std::atomic<int> _totalExited{ 0 };       // 累计退出线程数（worker exit）

public:
	/**
	 * @param size        线程数
	 * @param maxQueue    队列上限；0 表示自动 = size * kDefaultQueueDepthMultiplier
	 */
	inline threadpool(unsigned short size = 1, size_t maxQueue = 0)
		: _maxQueueSize(maxQueue > 0 ? maxQueue : static_cast<size_t>(size) * kDefaultQueueDepthMultiplier)
	{
		addThread(size);
	}
	inline ~threadpool()
	{
		_run.store(false, std::memory_order_release);
		for (auto& t : _pool)
		{
			if (t.joinable())
				t.join();
		}
	}

	/**
	 * @brief 提交一个任务到线程池
	 * @return std::future，调用 .get() 等待任务执行完毕并获取返回值
	 * @throws std::runtime_error 若线程池已停止或队列满（背压）
	 *
	 * 背压语义：当 _queueSize >= _maxQueueSize 时抛异常。
	 * 保留 FIFO 顺序性：只在 enqueue 前拒绝，不重排已入队任务。
	 */
	template<class F, class... Args>
	auto commit(F&& f, Args&&... args) -> std::future<decltype(f(args...))>
	{
		if (!_run.load(std::memory_order_acquire))
			throw std::runtime_error("commit on ThreadPool is stopped.");

		// 背压：原子预留 slot。若超出上限则回滚并抛异常。
		// 用 fetch_add 代替 check-then-add 避免 TOCTOU 竞态。
		size_t sz = _queueSize.fetch_add(1, std::memory_order_acq_rel);
		if (sz >= _maxQueueSize)
		{
			_queueSize.fetch_sub(1, std::memory_order_relaxed); // 回滚
			throw std::runtime_error(
				"commit on ThreadPool: queue full (" + std::to_string(_maxQueueSize) + ")");
		}

		using RetType = decltype(f(args...));
		auto task = std::make_shared<std::packaged_task<RetType()>>(
			std::bind(std::forward<F>(f), std::forward<Args>(args)...)
		);
		std::future<RetType> future = task->get_future();

		// enqueue 在 slot 预留后执行，不会失败；task 执行后释放 slot
		_tasks.enqueue([task, this]() {
			(*task)();
			_queueSize.fetch_sub(1, std::memory_order_release);
		});

		return future;
	}

	/// 当前队列深度
	size_t queueSize() const { return _queueSize.load(std::memory_order_relaxed); }
	/// 队列上限
	size_t maxQueueSize() const { return _maxQueueSize; }
	/// 空闲线程数量（近似）
	int idlCount() const { return _idlThrNum.load(std::memory_order_relaxed); }
	/// 当前活跃线程数（累计创建 - 累计退出）
	int thrCount() const
	{
		return _totalCreated.load(std::memory_order_relaxed)
			 - _totalExited.load(std::memory_order_relaxed);
	}

	/**
	 * @brief 动态调整线程数
	 * @param n 目标线程数，限制在 [1, THREADPOOL_MAX_NUM]
	 *
	 * 增大：直接创建新 worker 线程。
	 * 缩小：标记 _excessThreads，空闲 worker 自行退出，不影响正在执行的任务。
	 */
	void resize(unsigned short n)
	{
		if (n < 1) n = 1;
		if (n > THREADPOOL_MAX_NUM) n = THREADPOOL_MAX_NUM;

		// 用累计创建 - 累计退出 计算当前实际活跃线程数
		int cur = _totalCreated.load(std::memory_order_acquire)
				- _totalExited.load(std::memory_order_acquire);
		if (n > static_cast<unsigned short>(cur))
		{
			addThread(static_cast<unsigned short>(n - cur));
		}
		else if (static_cast<int>(n) < cur)
		{
			_excessThreads.fetch_add(cur - n, std::memory_order_release);
		}
	}

private:
	void addThread(unsigned short size)
	{
		for (; _pool.size() < THREADPOOL_MAX_NUM && size > 0; --size)
		{
			_totalCreated++;
		_pool.emplace_back([this] {
				_idlThrNum++;
				while (true)
				{
					Task task;
					if (_tasks.try_dequeue(task))
					{
						_idlThrNum--;
						task();
						_idlThrNum++;
						continue;  // 执行完任务继续取下一个
					}

					// 队列空 → 检查是否应退出
					if (_excessThreads.load(std::memory_order_acquire) > 0)
					{
						// resize 缩小：空闲 worker 退出，不影响已有请求
						_excessThreads.fetch_sub(1, std::memory_order_relaxed);
						_totalExited++;
						_idlThrNum--;
						return;
					}
					if (!_run.load(std::memory_order_acquire))
					{
						// 线程池已停止
						_totalExited++;
						_idlThrNum--;
						return;
					}

					// 还活着且无任务 → yield 让出时间片
					std::this_thread::yield();
				}
			});
		}
	}
};

} // namespace util

#endif
