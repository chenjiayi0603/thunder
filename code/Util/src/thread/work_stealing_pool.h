#pragma once
#ifndef WORK_STEALING_POOL_H
#define WORK_STEALING_POOL_H

#include <array>
#include <vector>
#include <atomic>
#include <future>
#include <thread>
#include <functional>
#include <stdexcept>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include "concurrentqueue.h"
#include "thread/worker_deque.h"

#ifndef THREADPOOL_MAX_NUM
#define THREADPOOL_MAX_NUM 16
#endif

namespace util
{

/**
 * Work-Stealing 线程池（Go LRQ 风格）
 *
 * 公开 API 与 util::threadpool 完全相同：
 *   commit / idlCount / queueSize / maxQueueSize / resize
 *
 * 内部差异：
 *   - 每个 worker 持有两个 WorkerDeque，消除 push/steal 的 tail 竞争：
 *       _submit_deques[id]  仅由 commit() 线程 push，worker dequeue / 其他 worker steal
 *       _local_deques[id]   仅由 worker[id] 写入（steal_into），worker 自己 dequeue
 *   - commit() 三级分发：Power of Two Choices → 全扫 → global_q
 *   - worker 循环：local_deque → submit_deque → steal → global_q → yield
 *   - 缩容：worker 退出前将两个 deque 的剩余任务 drain 到 global_q
 *
 * 关联文档：docs/architecture/23-work-stealing-threadpool.md
 * 关联 issue：#109
 */
class WorkStealingPool
{
public:
    static constexpr size_t kDefaultQueueDepthMultiplier = 64;

private:
    using Task  = std::function<void()>;
    using Deque = WorkerDeque<Task, 256>;

    static constexpr uint32_t kStealBatch = 128; // CAP / 2

    // _submit_deques: commit() → push；worker → dequeue，thief → steal_into(src)
    // _local_deques:  worker[id] 独占写（steal_into dst）；thief → steal_into(src)
    std::array<Deque, THREADPOOL_MAX_NUM> _submit_deques;
    std::array<Deque, THREADPOOL_MAX_NUM> _local_deques;

    moodycamel::ConcurrentQueue<Task> _global_q;

    std::vector<std::thread> _pool;
    std::atomic<bool>   _run          { true };
    std::atomic<int>    _idlThrNum    { 0 };
    std::atomic<size_t> _queueSize    { 0 };
    size_t              _maxQueueSize;
    std::atomic<int>    _excessThreads{ 0 };
    std::atomic<int>    _totalCreated { 0 };
    std::atomic<int>    _totalExited  { 0 };
    std::atomic<int>    _activeWorkers{ 0 };
    std::atomic<int>    _waitingWorkers{ 0 };
    std::condition_variable _cv;
    std::mutex              _cv_mutex;

public:
    explicit WorkStealingPool(unsigned short size = 1, size_t maxQueue = 0)
        : _maxQueueSize(maxQueue > 0
              ? maxQueue
              : static_cast<size_t>(size) * kDefaultQueueDepthMultiplier)
    {
        addThread(size);
    }

    ~WorkStealingPool()
    {
        _run.store(false, std::memory_order_release);
        for (auto& t : _pool)
            if (t.joinable()) t.join();
    }

    /**
     * 提交任务，返回 std::future
     *
     * 三级分发：
     *   1. Power of Two Choices（O(1)，随机采样 2 个 _submit_deque，选任务少的）
     *   2. 顺序全扫（O(N)，N≤16）
     *   3. global_q（真正的 burst 蓄水，仅全部 submit_deque 满时触发）
     *
     * commit() 只写 _submit_deques，不碰 _local_deques，消除 tail 竞争。
     */
    template<class F, class... Args>
    auto commit(F&& f, Args&&... args) -> std::future<decltype(f(args...))>
    {
        if (!_run.load(std::memory_order_acquire))
            throw std::runtime_error("commit on WorkStealingPool is stopped.");

        size_t sz = _queueSize.fetch_add(1, std::memory_order_acq_rel);
        if (sz >= _maxQueueSize)
        {
            _queueSize.fetch_sub(1, std::memory_order_relaxed);
            throw std::runtime_error(
                "commit on WorkStealingPool: queue full ("
                + std::to_string(_maxQueueSize) + ")");
        }

        using RetType = decltype(f(args...));
        auto task = std::make_shared<std::packaged_task<RetType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<RetType> fut = task->get_future();

        Task wrapped = [task, this]() {
            (*task)();
            _queueSize.fetch_sub(1, std::memory_order_release);
        };

        int n = _activeWorkers.load(std::memory_order_acquire);
        if (n <= 0)
        {
            _global_q.enqueue(std::move(wrapped));
            return fut;
        }

        // Level 1: Power of Two Choices（仅采样 _submit_deques）
        uint32_t un = static_cast<uint32_t>(n);
        uint32_t a   = xorshift32() % un;
        uint32_t b   = xorshift32() % un;
        uint32_t idx = (_submit_deques[a].size() <= _submit_deques[b].size()) ? a : b;
        if (_submit_deques[idx].push(std::move(wrapped)))
        {
            if (_waitingWorkers.load(std::memory_order_acquire) > 0)
                _cv.notify_one();
            return fut;
        }

        // Level 2: 顺序全扫
        for (int i = 0; i < n; i++)
            if (_submit_deques[i].push(std::move(wrapped)))
            {
                if (_waitingWorkers.load(std::memory_order_acquire) > 0)
                    _cv.notify_one();
                return fut;
            }

        // Level 3: 真正 burst，所有 submit_deque 全满
        _global_q.enqueue(std::move(wrapped));

        // 唤醒一个等待中的 worker（如果全部 idle）
        if (_waitingWorkers.load(std::memory_order_acquire) > 0)
            _cv.notify_one();
        return fut;
    }

    size_t queueSize()    const { return _queueSize.load(std::memory_order_relaxed); }
    size_t maxQueueSize() const { return _maxQueueSize; }
    int    idlCount()     const { return _idlThrNum.load(std::memory_order_relaxed); }
    int    thrCount()     const
    {
        return _totalCreated.load(std::memory_order_relaxed)
             - _totalExited.load(std::memory_order_relaxed);
    }

    void resize(unsigned short n)
    {
        if (n < 1) n = 1;
        if (n > THREADPOOL_MAX_NUM) n = THREADPOOL_MAX_NUM;
        int cur = _totalCreated.load(std::memory_order_acquire)
                - _totalExited.load(std::memory_order_acquire);
        if (n > static_cast<unsigned short>(cur))
            addThread(static_cast<unsigned short>(n - cur));
        else if (static_cast<int>(n) < cur)
            _excessThreads.fetch_add(cur - n, std::memory_order_release);
    }

private:
    static uint32_t xorshift32()
    {
        thread_local uint32_t s = 2463534242u;
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }

    void drainToGlobal(int id)
    {
        while (auto t = _local_deques[id].dequeue())
            _global_q.enqueue(std::move(*t));
        while (auto t = _submit_deques[id].dequeue())
            _global_q.enqueue(std::move(*t));
    }

    void addThread(unsigned short size)
    {
        for (; _pool.size() < THREADPOOL_MAX_NUM && size > 0; --size)
        {
            int id = _totalCreated.fetch_add(1, std::memory_order_relaxed);
            _activeWorkers.fetch_add(1, std::memory_order_release);

            _pool.emplace_back([this, id] {
                _idlThrNum++;
                uint32_t tick = 0;

                while (true)
                {
                    tick++;

                    // ── 61-tick：防高负载下 global_q 长期饥饿 ──────────────
                    if (tick % 61 == 0)
                    {
                        Task t;
                        if (_global_q.try_dequeue(t))
                        {
                            _idlThrNum--;
                            t();
                            _idlThrNum++;
                            continue;
                        }
                    }

                    // ── 本地 steal buffer（steal_into 的目标，无竞争）─────
                    if (auto t = _local_deques[id].dequeue())
                    {
                        _idlThrNum--;
                        (*t)();
                        _idlThrNum++;
                        continue;
                    }

                    // ── submit deque（commit() 写入，worker dequeue 消费）──
                    if (auto t = _submit_deques[id].dequeue())
                    {
                        _idlThrNum--;
                        (*t)();
                        _idlThrNum++;
                        continue;
                    }

                    // ── 退出检查（deque 已空时才检查）────────────────────
                    if (_excessThreads.load(std::memory_order_acquire) > 0)
                    {
                        _excessThreads.fetch_sub(1, std::memory_order_relaxed);
                        drainToGlobal(id);
                        _activeWorkers.fetch_sub(1, std::memory_order_release);
                        _totalExited++;
                        _idlThrNum--;
                        return;
                    }
                    if (!_run.load(std::memory_order_acquire))
                    {
                        drainToGlobal(id);
                        _activeWorkers.fetch_sub(1, std::memory_order_release);
                        _totalExited++;
                        _idlThrNum--;
                        return;
                    }

                    // ── steal：随机起点，最多 min(4,N-1) 次 ──────────────
                    int n = _activeWorkers.load(std::memory_order_acquire);
                    if (n > 1)
                    {
                        uint32_t start = xorshift32() % static_cast<uint32_t>(n);
                        int tries = std::min(4, n - 1);
                        bool stole = false;
                        for (int i = 0; i < tries && !stole; i++)
                        {
                            int victim = (static_cast<int>(start) + i) % n;
                            if (victim == id) continue;
                            // 先尝试从 victim 的 submit_deque steal
                            if (_local_deques[id].steal_into(_submit_deques[victim], kStealBatch) > 0)
                            {
                                stole = true;
                            }
                            // 再尝试从 victim 的 local_deque steal
                            else if (_local_deques[id].steal_into(_local_deques[victim], kStealBatch) > 0)
                            {
                                stole = true;
                            }
                        }
                        if (stole)
                        {
                            if (auto t = _local_deques[id].dequeue())
                            {
                                _idlThrNum--;
                                (*t)();
                                _idlThrNum++;
                            }
                            continue;
                        }
                    }

                    // ── idle 路径：deque 空 + steal 失败 → 捞 global_q ───
                    {
                        Task t;
                        if (_global_q.try_dequeue(t))
                        {
                            _idlThrNum--;
                            t();
                            _idlThrNum++;
                            continue;
                        }
                    }

                    // idle: cv 阻塞等通知，避免 yield() 空转消耗 CPU
                    _waitingWorkers.fetch_add(1, std::memory_order_release);
                    {
                        std::unique_lock<std::mutex> lk(_cv_mutex);
                        _cv.wait_for(lk, std::chrono::milliseconds(10),
                            [this] { return !_run.load(std::memory_order_acquire)
                                           || _queueSize.load(std::memory_order_acquire) > 0
                                           || _excessThreads.load(std::memory_order_acquire) > 0; });
                    }
                    _waitingWorkers.fetch_sub(1, std::memory_order_release);
                }
            });
        }
    }
};

} // namespace util

#endif // WORK_STEALING_POOL_H
