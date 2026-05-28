/*******************************************************************************
* Project:  Thunder
* @file     ShmRingQueue.hpp
* @brief    Lock-free SPSC ring buffer in shared memory for Manager↔Worker IPC
* @author   cjy
* @date:    2026-05-10
* @note
*   设计要点:
*     1. 单生产者单消费者 (SPSC) — 读写各自独占 write_index / read_index，无需锁
*     2. 共享内存 — MAP_SHARED | MAP_ANONYMOUS，fork() 后父子进程自然共享
*     3. 无锁消息传递 — 用 msg_len 字段作为"数据就绪"标志，替代传统锁/信号量
*     4. eventfd 通知 — 生产者写完写 eventfd，消费者 poll eventfd 后批量 drain
*     5. 零拷贝 — body 直接写入 slot 数据区，无中间缓冲区
*
*   内存布局 (一次 mmap 分配):
*     ┌──────────────────────────────────────────────────────┐
*     │  ControlBlock (cache line 对齐)                       │
*     │    write_index | read_index | slot_size | slot_count │
*     │    magic       | flags                               │
*     ├──────────────────────────────────────────────────────┤
*     │  Slot[0]  (slot_size 字节)                            │
*     │    ┌─ SlotHeader (12B): msg_len | cmd | seq          │
*     │    ├─ body 区 (slot_size - 12B)                      │
*     ├──────────────────────────────────────────────────────┤
*     │  Slot[1]                                              │
*     │  ...                                                  │
*     └──────────────────────────────────────────────────────┘
*
*   并发协议:
*     生产者: 写 body → 写 cmd/seq → release fence → 写 msg_len → release write_index
*     消费者: 读 write_index(acquire) → 读 msg_len → 读 cmd/seq → 读 body
*
*     关键: msg_len 是"数据就绪"标志位。
*     生产者最后写 msg_len (非零)，消费者先检查 msg_len != 0 才取数据。
*     消费者读完将 msg_len 写回 0，生产者可安全覆写该 slot。
*     cmd/seq 写在 msg_len 之前，消费者读到 msg_len != 0 时 cmd/seq 保证已可见。
*     (release fence 保证 cmd/seq/body 在 msg_len 之前对其他核心可见)
*
*   TryDequeue 不保证输出缓冲区以 '\0' 结尾 — 调用方应使用
*   body_len 判断有效长度，不可依赖 strlen / EXPECT_STREQ。
*
* Modify history:
*   2026-05-24  补充设计注释与内存布局文档
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
    // ═══════════════════════════════════════════════════════════════════════
    // 控制块 — 整个队列的元数据，位于 mmap 区域开头 (offset 0)
    //
    // 字段均为 std::atomic，支持跨进程无锁访问。
    // mmap 返回页对齐内存 (4096B 边界)，ControlBlock 自然独占首 cache line，
    // 避免与 Slot[0] 产生 false sharing。
    // ═══════════════════════════════════════════════════════════════════════
    struct ControlBlock
    {
        std::atomic<uint64_t> write_index{0};  // 生产者下一个写入槽位 (单调递增, 永不回绕)
        std::atomic<uint64_t> read_index{0};   // 消费者下一个读取槽位 (单调递增, 永不回绕)
        std::atomic<uint32_t> slot_size{0};    // 每个 slot 的字节数 (header + body)
        std::atomic<uint32_t> slot_count{0};   // slot 总数 (队列容量)
        std::atomic<uint32_t> magic{0};        // 魔数 0x53484D51 ("SHMQ")，用于校验
        std::atomic<uint32_t> flags{0};        // 保留

        ControlBlock() = default;
    };

    ControlBlock ctrl;

    // ═══════════════════════════════════════════════════════════════════════
    // Slot 布局: 12 字节定长头 + 变长 body
    //
    // msg_len = HEADER_SIZE + body_len，为 0 表示空槽 (可被生产者安全覆写)
    // msg_len 充当"数据就绪"标志 — 生产者最后写它，消费者先检查它
    // ═══════════════════════════════════════════════════════════════════════
    struct SlotHeader
    {
        uint32_t msg_len;   // 消息总长度 (header + body)，0 = 空槽
        uint32_t cmd;       // 命令字
        uint32_t seq;       // 序列号
    };
    static constexpr uint32_t HEADER_SIZE = sizeof(SlotHeader);   // 12

    // ── 根据全局单调索引定位 slot ──
    // 用 index % slot_count 定位物理槽位 (环形缓冲区)
    //
    // this + 1 解释:
    //   this 指向 mmap 区域起始 (ShmRingQueue 对象本身)。
    //   this + 1 = (char*)this + sizeof(ShmRingQueue)，即跳过 ControlBlock，
    //   指向紧跟其后的 Slot 数组起始地址。这是一种 C 风格 "struct 尾部变长数组"
    //   模式: Create() 一次性 mmap 分配 sizeof(ShmRingQueue) + slot_count*slot_size，
    //   Slot[] 数据区紧贴在 ShmRingQueue 对象之后（零额外指针，fork 友好）。
    //
    //   内存布局:
    //     mmap 区域: [ShmRingQueue 对象][Slot[0]][Slot[1]]...[Slot[N-1]]
    //                ^                    ^
    //                this                 this + 1
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

    // 单条消息 body 最大长度 (不含 header)
    uint32_t MaxBodySize() const { return ctrl.slot_size.load(std::memory_order_relaxed) - HEADER_SIZE; }

    // ── 容量查询 ──
    // 用 acquire 保证读到生产者的最新 write_index / read_index

    // 队列满: 未读消息数 >= slot_count (write_index - read_index >= 容量)
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

    // ═══════════════════════════════════════════════════════════════════════
    // 生产者入队 (SPSC — 仅一个写者)
    //
    // 流程:
    //   1. 检查容量 (w - r < slot_count)
    //   2. 写 body 到 slot 数据区
    //   3. 写 cmd / seq 到 header
    //   4. release fence — 保证步骤 2-3 的写入在步骤 5 之前对其他核心可见
    //   5. 写 msg_len — "数据就绪"信号
    //   6. release write_index — 通知消费者有新数据
    //
    // 为什么 msg_len 最后写: 消费者只检查 msg_len != 0，若消费者看到非零
    // msg_len 但 cmd/seq/body 还未写入 (因 CPU 乱序/缓存)，将读到脏数据。
    // release fence 解决了这个问题。
    // ═══════════════════════════════════════════════════════════════════════
    bool TryEnqueue(uint32_t cmd, uint32_t seq,
                    const void* body, uint32_t body_len)
    {
        if (body_len > MaxBodySize())
        {
            return false;  // body 超过 slot 容量
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

        // 步骤 2: 先写 body (数据区)
        if (body && body_len > 0)
        {
            memcpy(slot + HEADER_SIZE, body, body_len);
        }
        // 步骤 3: 再写 header 字段
        hdr->cmd     = cmd;
        hdr->seq     = seq;
        // 步骤 4: 内存屏障 — cmd/seq/body 写入对消费者可见
        std::atomic_thread_fence(std::memory_order_release);
        // 步骤 5: 最后写 msg_len，消费者以此判断数据就绪
        hdr->msg_len = total_len;

        // 步骤 6: 发布 write_index，消费者通过 eventfd/poll 感知
        ctrl.write_index.store(w + 1, std::memory_order_release);
        return true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 消费者出队 (SPSC — 仅一个读者)
    //
    // 流程:
    //   1. 读 write_index (acquire) — 保证看到生产者最新写入
    //   2. 检查 msg_len — 0 = 未就绪或已被消费
    //   3. 读 cmd / seq / body
    //   4. 写 msg_len = 0 — 标记为空槽，生产者可安全覆写
    //   5. release read_index — 通知生产者空间已释放
    //
    // 注意: 不会在 body 末尾追加 '\0'，调用方应使用 body_len 判断有效长度
    // ═══════════════════════════════════════════════════════════════════════
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

        // 检查数据就绪标志: 生产者可能正在写入此 slot
        uint32_t total_len = hdr->msg_len;
        if (total_len == 0)
        {
            body_len = 0;
            return false;  // 槽位未就绪 (生产者在写但尚未设置 msg_len)
        }

        cmd      = hdr->cmd;
        seq      = hdr->seq;
        body_len = total_len - HEADER_SIZE;

        // 拷贝 body，不追加 '\0' (节省一次写操作)
        if (body_buf && body_len > 0)
        {
            memcpy(body_buf, slot + HEADER_SIZE, body_len);
        }

        // 标记为空槽 — 释放给生产者复用
        hdr->msg_len = 0;
        // release fence: 保证 msg_len = 0 对生产者可见 (生产者检查此值判断可覆写)
        std::atomic_thread_fence(std::memory_order_release);

        ctrl.read_index.store(r + 1, std::memory_order_release);

        return true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 通知辅助 — 轻量级检查队列是否有待消费数据
    //
    // 用于批量 drain 场景: 消费者被 eventfd 唤醒后先调 NotifyPending()
    // 确认队列非空，再循环 TryDequeue() 直至队列空
    // ═══════════════════════════════════════════════════════════════════════
    bool NotifyPending()
    {
        uint64_t w = ctrl.write_index.load(std::memory_order_acquire);
        uint64_t r = ctrl.read_index.load(std::memory_order_relaxed);
        return w > r;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 工厂方法 — 创建/销毁共享内存队列
    //
    // MAP_SHARED | MAP_ANONYMOUS: 匿名共享映射。
    // 第一次 mmap 返回零填充页；fork() 后子进程继承该映射，
    // 父子进程的 ControlBlock / Slot 均指向同一物理页，实现零拷贝 IPC。
    //
    // 注意: 每次 Create 返回独立映射，不会复用之前的物理页。
    // 若需跨进程重启后恢复队列 (如 Worker 重启)，应将 mmap 改为
    // 基于文件 (shm_open / file-backed mmap)。
    //
    // 分配大小 = sizeof(ShmRingQueue) + slot_count * slot_size
    // ═══════════════════════════════════════════════════════════════════════
    static ShmRingQueue* Create(uint32_t slot_count = 128, uint32_t slot_size = 4096)
    {
        size_t total = sizeof(ShmRingQueue) + static_cast<size_t>(slot_count) * slot_size;
        void* mem = mmap(nullptr, total, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED)
        {
            return nullptr;
        }
        // placement new: 在 mmap 区域上构造对象 (不分配新内存)
        ShmRingQueue* q = new (mem) ShmRingQueue();
        // 设置不可变参数 (进程生命周期内不变，relaxed 足够)
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
