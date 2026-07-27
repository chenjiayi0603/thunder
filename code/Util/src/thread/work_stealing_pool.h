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
 * ── commit() 三级分发（生产者路径）────────────────────────────────────
 *
 *   commit(fn)
 *       │
 *       ▼
 *   ┌────────────┐  fail   ┌────────────┐  fail   ┌────────────┐
 *   │ Level 1    │ ──────→ │ Level 2    │ ──────→ │ Level 3    │
 *   │ Two Choices│         │ 全扫 16 个 │         │ global_q   │
 *   │ O(1) 挑2个  │         │ submit_deq │         │ 无界兜底   │
 *   └─────┬──────┘         └─────┬──────┘         └─────┬──────┘
 *         │ success              │ success              │
 *         ▼                      ▼                      ▼
 *   _submit_deques[idx]  _submit_deques[i]      ConcurrentQueue
 *   .push(task)          .push(task)            .enqueue(task)
 *
 *
 * ── worker 消费循环（消费者路径）──────────────────────────────────────
 *
 *   每个 worker[id] 有自己的一对 deque：
 *     _submit_deques[id] — commit() 写, worker 读, 别人 steal（生产者→消费者）
 *     _local_deques[id]  — worker 独占写(steal_into), 自己读（本地缓存）
 *
 *   while(true):
 *     ┌─ 1. _local_deques[id].dequeue()         ← 自己 steal 来的，最快，无锁
 *     ├─ 2. _submit_deques[id].dequeue()        ← commit() 投递的
 *     ├─ 3. steal 其他 worker                    ← 负载均衡
 *     │      先抢 victim._submit_deques (积压多)
 *     │      再抢 victim._local_deques  (备用)
 *     ├─ 4. _global_q.try_dequeue()            ← 兜底
 *     └─ 5. cv.wait()                           ← 空闲等通知
 *
 *
 * ── 缩容 drain ────────────────────────────────────────────────────────
 *
 *   resize(n) → _excessThreads += (cur - n)
 *   worker 空闲时 CAS 抢到退出权:
 *     drainToGlobal(id): _local[id] + _submit[id] → _global_q
 *     然后线程退出（任务不丢）
 *
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

    moodycamel::ConcurrentQueue<Task> _global_q;  // 无锁 MPMC 并发队列（CAS），三级分发兜底 + 缩容 drain 目标

    std::vector<std::thread> _pool;
    std::atomic<bool>   _run          { true };
    std::atomic<int>    _idlThrNum    { 0 };    // 当前空闲线程数（cv.wait 中的不计入）
    std::atomic<size_t> _queueSize    { 0 };    // 所有队列中未完成任务总数，用于背压
    size_t              _maxQueueSize;          // 达到时 commit() 阻塞等待
    std::atomic<int>    _excessThreads{ 0 };    // resize 缩容: 需要退出的线程数，CAS 抢
    std::atomic<int>    _totalCreated { 0 };
    std::atomic<int>    _totalExited  { 0 };
    std::atomic<int>    _activeWorkers{ 0 };    // 当前活跃 worker 数（不含已标记退出但未析构的）
    std::atomic<int>    _waitingWorkers{ 0 };   // cv.wait 中的线程数，commit() 据此决定是否 notify
    std::condition_variable _cv;
    std::mutex              _cv_mutex;

public:
    explicit WorkStealingPool(unsigned short size = 1, size_t maxQueue = 0)
        : _maxQueueSize(maxQueue > 0
              ? maxQueue
              : static_cast<size_t>(size) * kDefaultQueueDepthMultiplier)  // 默认队列容量 = 线程数 × 64
    {
        addThread(size);
    }

    ~WorkStealingPool()
    {
        _run.store(false, std::memory_order_release);
        _cv.notify_all();
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

        // 先占位再检查：fetch_add 拿到的是旧值，旧值 >= 上限说明满，回退并阻塞等待
        size_t sz = _queueSize.fetch_add(1, std::memory_order_acq_rel);
        if (sz >= _maxQueueSize)
        {
            _queueSize.fetch_sub(1, std::memory_order_relaxed);
            {
                std::unique_lock<std::mutex> lk(_cv_mutex);
                _cv.wait(lk, [this] {
                    return !_run.load(std::memory_order_acquire)
                        || _queueSize.load(std::memory_order_acquire) < _maxQueueSize;
                });
            }
            if (!_run.load(std::memory_order_acquire))
                throw std::runtime_error("commit on WorkStealingPool is stopped.");
            sz = _queueSize.fetch_add(1, std::memory_order_acq_rel);
        }

        using RetType = decltype(f(args...));
        auto task = std::make_shared<std::packaged_task<RetType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<RetType> fut = task->get_future();

        Task wrapped = [task, this]() {
            (*task)();
            _queueSize.fetch_sub(1, std::memory_order_release);
            _cv.notify_all();  // 任务完成释放队列槽位，唤醒被背压阻塞的 commit()
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

        // Level 2: 全扫所有 deque（缩容后 active workers 和 deque index 可能不对齐）
        for (int i = 0; i < THREADPOOL_MAX_NUM; i++)
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

    // 动态调整线程数，不丢任务
    // 扩容: 直接 addThread
    // 缩容: 只设 _excessThreads 标记，worker 空闲时自行 CAS 抢退出权并 drain 任务
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

    // 线程退出前将两个 deque 的剩余任务搬到 global_q，保证任务不丢
    // 先搬 _local（本线程独占无竞争），再搬 _submit（可能有 commit() 正在 push，但放后面减少窗口）
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
                    // worker 始终优先消费自己的 deque，global_q 可能永远排不上队
                    // 每 61 次循环（质数，避免与业务周期共振）强制检查一次 global_q
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
                    {
                        int e = _excessThreads.load(std::memory_order_acquire);
                        while (e > 0)
                        {
                            if (_excessThreads.compare_exchange_weak(e, e - 1,
                                    std::memory_order_acq_rel, std::memory_order_relaxed))
                            {
                                drainToGlobal(id);
                                _activeWorkers.fetch_sub(1, std::memory_order_release);
                                _totalExited++;
                                _idlThrNum--;
                                return;
                            }
                        }
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
                    // 缩容后 n 可能小于实际 deque index，此时扩大扫范围
                    int scanN = (n > 1) ? n : THREADPOOL_MAX_NUM;
                    {
                        uint32_t start = xorshift32() % static_cast<uint32_t>(scanN);
                        int tries = std::min(4, scanN - 1);
                        if (n <= 1) tries = std::min(8, scanN); // 单 worker 多扫几个 deque
                        bool stole = false;
                        for (int i = 0; i < tries && !stole; i++)
                        {
                            int victim = (static_cast<int>(start) + i) % scanN;
                            if (victim == id) continue;
                            // steal_into(dst, batch): 从 victim 队列批量窃取 batch 个任务到自己的 _local_deques[id]（dst 参数）
                            // 优先级 1: 抢 victim _submit_deques — commit() 投递的生产者队列，积压概率最高
                            if (_local_deques[id].steal_into(_submit_deques[victim], kStealBatch) > 0)
                            {
                                stole = true;
                            }
                            // 优先级 2: 抢 victim _local_deques — victim 自己 steal 来的本地缓存，次优先
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
                    // 唤醒条件（满足任一即退出 wait）:
                    //   !_run         — 析构/stop，线程退出
                    //   _queueSize>0  — commit() 有新任务入队，起来干活
                    //   _excessThreads>0 — resize 缩容，CAS 抢退出权
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
