#pragma once
#ifndef WORKER_DEQUE_H
#define WORKER_DEQUE_H

#include <atomic>
#include <optional>
#include <cstdint>
#include <functional>

namespace util
{

/**
 * Go LRQ 风格固定容量 work-stealing deque
 *
 * 角色分工（与 Chase-Lev 的根本区别）：
 *   push       — 事件循环线程（唯一 tail 写者），无竞争
 *   dequeue    — 本 worker 线程，CAS head++
 *   steal_into — 其他空闲 worker，CAS src.head，1次CAS偷取n个
 *
 * Thunder 约束：push 和 dequeue 是不同线程（事件循环 vs worker），
 * 因此不能用 Chase-Lev（其 pop 要求 push/pop 同线程独占 bottom 端）。
 * Go LRQ 让 push 独占 tail、dequeue/steal 都 CAS head，两端天然分离。
 *
 * 内存布局：head 和 tail 各占独立 cache line，消除 false sharing。
 * 单个 deque：128B (head+tail) + CAP * sizeof(Task)
 */
template<typename Task, uint32_t CAP = 256>
class WorkerDeque
{
    static_assert((CAP & (CAP - 1)) == 0, "CAP must be a power of two");

public:
    static constexpr uint32_t CAPACITY = CAP;

    WorkerDeque() = default;
    WorkerDeque(const WorkerDeque&) = delete;
    WorkerDeque& operator=(const WorkerDeque&) = delete;

    /**
     * push — 仅由事件循环线程调用（单一 tail 写者）
     * 返回 false 表示队列已满，调用方应走 global_q 溢出路径
     */
    // push 失败（队列满）时不消耗 task，调用方可继续尝试其他 deque
    bool push(Task&& task)
    {
        uint32_t h = _head.load(std::memory_order_acquire);
        uint32_t t = _tail.load(std::memory_order_relaxed);
        if (t - h >= CAP)
            return false;                          // task 未被 move，调用方仍持有
        _ring[t & (CAP - 1)] = std::move(task);   // 确认有空位才 move
        _tail.store(t + 1, std::memory_order_release);
        return true;
    }

    /**
     * dequeue — 由本 worker 调用，与 steal_into 竞争 head
     * 返回 nullopt 表示队列为空
     */
    std::optional<Task> dequeue()
    {
        for (;;)
        {
            uint32_t h = _head.load(std::memory_order_acquire);
            uint32_t t = _tail.load(std::memory_order_acquire);
            if (h == t)
                return std::nullopt;
            Task task = _ring[h & (CAP - 1)];
            if (_head.compare_exchange_weak(h, h + 1,
                    std::memory_order_seq_cst,
                    std::memory_order_relaxed))
                return task;
            // CAS 失败：其他 worker 正在 steal，重试
        }
    }

    /**
     * steal_into — 从 src 批量偷取任务追加到本 deque
     *
     * 调用方：其他空闲 worker（本 deque 的 tail 单写者）
     * 偷取量：min(max_n, src可用/2)，再受本 deque 剩余容量限制
     * CAS：仅 1 次（推进 src.head），与偷取数量 n 无关
     *
     * 流程：先复制 ring 槽位（tail 未更新，外部不可见）
     *       → CAS src.head 宣告所有权
     *       → release store 更新本 tail 对外公开
     *
     * 返回实际偷取数，0 表示 src 为空或本 deque 已满
     */
    uint32_t steal_into(WorkerDeque& src, uint32_t max_n)
    {
        for (;;)
        {
            uint32_t sh = src._head.load(std::memory_order_acquire);
            uint32_t st = src._tail.load(std::memory_order_acquire);
            uint32_t avail = st - sh;
            if (avail == 0)
                return 0;

            // 偷 victim 任务的一半，但不超过 max_n
            uint32_t n = avail - avail / 2;
            if (n > max_n)
                n = max_n;

            // 不超过本 deque 剩余容量
            uint32_t my_h = _head.load(std::memory_order_acquire);
            uint32_t my_t = _tail.load(std::memory_order_relaxed);
            uint32_t space = CAP - (my_t - my_h);
            if (n > space)
                n = space;
            if (n == 0)
                return 0;

            // 复制任务到本 deque（tail 未更新，外部不可见）
            for (uint32_t i = 0; i < n; i++)
                _ring[(my_t + i) & (CAP - 1)] = src._ring[(sh + i) & (CAP - 1)];

            // 1 次 CAS 推进 src.head，宣告对这 n 个 slot 的所有权
            if (!src._head.compare_exchange_weak(sh, sh + n,
                    std::memory_order_seq_cst,
                    std::memory_order_relaxed))
                continue; // 被其他 thief 抢了，重试

            // CAS 成功后 release store 公开本 deque 新 tail
            _tail.store(my_t + n, std::memory_order_release);
            return n;
        }
    }

    // 当前任务数（近似，用于 Power of Two Choices 负载比较）
    // tail 和 head 分别 relaxed load，非原子对，用于采样估算即可
    uint32_t size() const
    {
        uint32_t t = _tail.load(std::memory_order_relaxed);
        uint32_t h = _head.load(std::memory_order_relaxed);
        return t - h;
    }

private:
    // head 和 tail 各占独立 cache line，消除 false sharing
    alignas(64) std::atomic<uint32_t> _head{0};
    alignas(64) std::atomic<uint32_t> _tail{0};

    Task _ring[CAP];
};

} // namespace util

#endif // WORKER_DEQUE_H
