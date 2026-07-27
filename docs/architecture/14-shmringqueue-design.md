# ShmRingQueue — 共享内存环形队列

> 源码: `code/Net/include/labor/types/ShmRingQueue.hpp`
> Manager↔Worker SPSC 零拷贝消息队列。64B 往返 ~130ns、吞吐 7.9M msg/s（9.3× vs pipe）

---

## 原理

```
┌──── ControlBlock ────┐
│ magic:     0x53484D51│  ← "SHMQ" 魔数
│ slot_size: 4096      │  ← 每个槽大小(字节)
│ slot_count:128       │  ← 总槽数
│ write_pos: 5         │  ← 生产者写入位置
│ read_pos:  3         │  ← 消费者读取位置
└──────────────────────┘

┌──── Slots[128] ──────────────────────────────┐
│ Slot[0]: [cmd|seq|len|body...]               │
│ Slot[1]: [cmd|seq|len|body...]               │
│ Slot[2]: [空]                                 │  ← read_pos=3, 已消费
│ Slot[3]: [空]                                 │
│ Slot[4]: [cmd|seq|len|body...]               │  ← 未消费
│ Slot[5]: [写入中...]                          │  ← write_pos=5
│ ...                                          │
│ Slot[127]: [空]                               │
└───────────────────────────────────────────────┘
```

### 写流程

```
Write(cmd, seq, data, len):
  1. tail = write_pos (volatile read)
  2. head = read_pos  (volatile read)
  3. if (tail - head) >= slot_count → 队列满，返回 false
  4. slot = slots[tail % slot_count]
  5. memcpy(slot.data, data, len); slot.len = len; slot.cmd = cmd; slot.seq = seq
  6. write_pos = tail + 1 (volatile store)
  7. eventfd_write(notify_fd, 1)   ← 通知消费者
```

### 读流程

```
Read():
  1. head = read_pos (volatile read)
  2. tail = write_pos (volatile read)
  3. if head == tail → 队列空，返回 false
  4. slot = slots[head % slot_count]
  5. cmd = slot.cmd; seq = slot.seq; len = slot.len; data = slot.data
  6. read_pos = head + 1 (volatile store)
```

---

## 为什么不用 pipe/socket

- pipe 有内核缓冲区拷贝，大消息时性能差
- 共享内存直接读写，零拷贝
- fork 后父子进程同一物理页，自动继承

---

## 性能基准

| 方案 | 64B 往返延迟 | 吞吐 |
|------|:--------:|:-----:|
| ShmRingQueue | ~130 ns | 7.9M msg/s |
| Unix pipe | ~1200 ns | 0.85M msg/s |
| **加速比** | **9.3x** | **9.3x** |

---

## 事件通知

```
Manager                                                    Worker
  │                                                          │
  │ ShmRingQueue::Write(cmd, data)                           │
  │   → memcpy to shm                                        │
  │   → eventfd_write(m_iMgrToWorkerEfd, 1)                  │
  │                                                          │
  │                             ev_io(m_iMgrToWorkerEfd) ──→ │ Read(cmd, data)
  │                             eventfd 被激活                │   → memcpy from shm
```

eventfd 作为 libev 可监控的文件描述符，实现零 CPU 轮询的通知。
