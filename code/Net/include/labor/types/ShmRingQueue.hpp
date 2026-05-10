/*******************************************************************************
* Project:  Thunder
* @file     ShmRingQueue.hpp
* @brief    Lock-free SPSC ring buffer in shared memory for Manager↔Worker IPC
* @author   cjy
* @date:    2026-05-10
* @note
* Modify history:
******************************************************************************/
#ifndef SRC_LABOR_TYPES_SHM_RING_QUEUE_HPP_
#define SRC_LABOR_TYPES_SHM_RING_QUEUE_HPP_

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/eventfd.h>

struct ShmRingQueue
{
    // ── 控制块 ──
    // 字段为 std::atomic，支持跨进程无锁访问。
    // mmap 返回页对齐内存，ControlBlock 在 offset 0，自然独占首 cache line。
    struct ControlBlock
    {
        std::atomic<uint64_t> write_index{0};  // 生产者槽位
        std::atomic<uint64_t> read_index{0};   // 消费者槽位
        std::atomic<uint32_t> slot_size{0};    // 每个 slot 大小
        std::atomic<uint32_t> slot_count{0};   // slot 总数
        std::atomic<uint32_t> magic{0};        // 魔数 0x53484D51 ("SHMQ")
        std::atomic<uint32_t> flags{0};        // 保留

        ControlBlock() = default;
    };

    ControlBlock ctrl;

    // ── Slot 内布局（12 字节头）──
    struct SlotHeader
    {
        uint32_t msg_len;   // 总长度（header + body），0 = 空槽
        uint32_t cmd;       // 命令字
        uint32_t seq;       // 序列号
    };
    static constexpr uint32_t HEADER_SIZE = sizeof(SlotHeader);   // 12

    // ── 获取数据区指针 ──
    char* GetSlotData(uint64_t index)
    {
        return reinterpret_cast<char*>(this + 1)
               + (index % static_cast<uint64_t>(ctrl.slot_count.load(std::memory_order_relaxed))) * ctrl.slot_size.load(std::memory_order_relaxed);
    }

    const char* GetSlotData(uint64_t index) const
    {
        return reinterpret_cast<const char*>(this + 1)
               + (index % static_cast<uint64_t>(ctrl.slot_count.load(std::memory_order_relaxed))) * ctrl.slot_size.load(std::memory_order_relaxed);
    }

    uint32_t MaxBodySize() const { return ctrl.slot_size.load(std::memory_order_relaxed) - HEADER_SIZE; }

    // ── 容量检查 ──
    bool IsFull() const
    {
        uint64_t w = ctrl.write_index.load(std::memory_order_acquire);
        uint64_t r = ctrl.read_index.load(std::memory_order_acquire);
        return (w - r) >= ctrl.slot_count;
    }

    bool IsEmpty() const
    {
        uint64_t w = ctrl.write_index.load(std::memory_order_acquire);
        uint64_t r = ctrl.read_index.load(std::memory_order_acquire);
        return w <= r;
    }

    uint64_t Count() const
    {
        uint64_t w = ctrl.write_index.load(std::memory_order_acquire);
        uint64_t r = ctrl.read_index.load(std::memory_order_acquire);
        return (w >= r) ? (w - r) : 0;
    }

    // ── 生产者入队 ──
    // 返回 true 表示成功；false 表示队列满或 body 超长
    bool TryEnqueue(uint32_t cmd, uint32_t seq,
                    const void* body, uint32_t body_len)
    {
        if (body_len > MaxBodySize())
        {
            return false;
        }

        uint64_t w = ctrl.write_index.load(std::memory_order_relaxed);
        uint64_t r = ctrl.read_index.load(std::memory_order_acquire);

        if (w - r >= ctrl.slot_count)
        {
            return false;  // 队列满
        }

        char* slot = GetSlotData(w);
        SlotHeader* hdr = reinterpret_cast<SlotHeader*>(slot);

        const uint32_t total_len = HEADER_SIZE + body_len;
        // 先写 body，再写 header，最后发布 write_index
        if (body && body_len > 0)
        {
            memcpy(slot + HEADER_SIZE, body, body_len);
        }
        hdr->cmd     = cmd;
        hdr->seq     = seq;
        // msg_len 最后写，作为数据就绪标志；消费者读到非零时才取用
        std::atomic_thread_fence(std::memory_order_release);
        hdr->msg_len = total_len;

        ctrl.write_index.store(w + 1, std::memory_order_release);
        return true;
    }

    // ── 消费者出队 ──
    // 返回 true 表示成功取出一条消息；false 表示队列空
    bool TryDequeue(uint32_t& cmd, uint32_t& seq,
                    void* body_buf, uint32_t& body_len)
    {
        uint64_t r = ctrl.read_index.load(std::memory_order_relaxed);
        uint64_t w = ctrl.write_index.load(std::memory_order_acquire);

        if (r >= w)
        {
            body_len = 0;
            return false;  // 队列空
        }

        char* slot = GetSlotData(r);
        SlotHeader* hdr = reinterpret_cast<SlotHeader*>(slot);

        // msg_len == 0 表示槽位尚未就绪（生产者在写 body/cmd/seq）
        uint32_t total_len = hdr->msg_len;
        if (total_len == 0)
        {
            body_len = 0;
            return false;
        }

        cmd      = hdr->cmd;
        seq      = hdr->seq;
        body_len = total_len - HEADER_SIZE;

        if (body_buf && body_len > 0)
        {
            memcpy(body_buf, slot + HEADER_SIZE, body_len);
        }

        // 标记为空槽，允许生产者复用
        hdr->msg_len = 0;
        // 作用：通过内存屏障，保证写入 msg_len = 0 对其他线程可见，避免数据竞争
        std::atomic_thread_fence(std::memory_order_release);

        ctrl.read_index.store(r + 1, std::memory_order_release);

        return true;
    }

    // ── 仅通知，不取数据（用于批量 drain 时先消费 eventfd 再逐个取）──
    // 返回 true 表示队列非空
    bool NotifyPending()
    {
        uint64_t w = ctrl.write_index.load(std::memory_order_acquire);
        uint64_t r = ctrl.read_index.load(std::memory_order_relaxed);
        return w > r;
    }

    // ── 工厂方法 ──
    // 分配大小 = sizeof(ShmRingQueue) + slot_count * slot_size
    static ShmRingQueue* Create(uint32_t slot_count = 128, uint32_t slot_size = 4096)
    {
        size_t total = sizeof(ShmRingQueue) + static_cast<size_t>(slot_count) * slot_size;
        void* mem = mmap(nullptr, total, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED)
        {
            return nullptr;
        }
        ShmRingQueue* q = new (mem) ShmRingQueue();
        q->ctrl.slot_size  = slot_size;
        q->ctrl.slot_count = slot_count;
        q->ctrl.magic      = 0x53484D51;  // "SHMQ"
        return q;
    }

    static void Destroy(ShmRingQueue* q, uint32_t slot_count = 128, uint32_t slot_size = 4096)
    {
        if (q)
        {
            size_t total = sizeof(ShmRingQueue) + static_cast<size_t>(slot_count) * slot_size;
            munmap(q, total);
        }
    }

    // ── eventfd 辅助方法 ──
    static int CreateEventFd()
    {
        return eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);
    }

    static void NotifyEventFd(int efd)
    {
        if (efd >= 0)
        {
            uint64_t one = 1;
            ssize_t ret = write(efd, &one, sizeof(one));
            (void)ret;  // 忽略 EAGAIN
        }
    }

    static void CloseEventFd(int& efd)
    {
        if (efd >= 0)
        {
            close(efd);
            efd = -1;
        }
    }
};

#endif /* SRC_LABOR_TYPES_SHM_RING_QUEUE_HPP_ */
