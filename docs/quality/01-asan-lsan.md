# 编译选项功能验证

每个编译选项一个最小示例，验证它真的在检测目标错误。

---

## ASan — 堆越界检测

```cpp
// /tmp/test_asan.cpp
#include <cstdlib>
int main() {
    int* p = new int[4];
    p[4] = 1;   // 越界写，第 5 个元素不存在
    delete[] p;
}
```

```bash
g++ -fsanitize=address -fno-omit-frame-pointer -o /tmp/test_asan /tmp/test_asan.cpp
/tmp/test_asan
```

**有错时的实际输出**：
```
==1742917==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x... at pc 0x...
WRITE of size 4 at 0x... thread T0
    #0 0x... in main /tmp/test_asan.cpp:4       ← 出错文件和行号
    #1 0x... in __libc_start_call_main ...

0x... is located 0 bytes after 16-byte region [0x...,0x...)
allocated by thread T0 here:
    #0 0x... in operator new[](unsigned long)
    #1 0x... in main /tmp/test_asan.cpp:3       ← 内存分配位置

SUMMARY: AddressSanitizer: heap-buffer-overflow /tmp/test_asan.cpp:4 in main
```
进程以非零退出。关键字段：错误类型 (`heap-buffer-overflow`)、出错行 (`#0 in main`)、分配位置。

### Thunder：ShmRingQueue 内存操作

`ShmRingQueue` 直接操作共享内存指针，是内存错误的高发区。`build-asan` 目录已用 `-fsanitize=address,undefined` 编译。

```bash
ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:leak_check_at_exit=1" \
    build-asan/bin/thunder_test_shm_queue
```

**无错时**（当前实际输出）：
```
[==========] 10 tests from 2 test suites ran. (26 ms total)
[  PASSED  ] 10 tests.
```

**有错时**（例如 `GetSlotData` 返回的指针越界写）：
```
==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x...
WRITE of size 128 at 0x... thread T0
    #0 0x... in ShmRingQueue::TryEnqueue(...)       ← Thunder 函数
          code/Net/include/labor/types/ShmRingQueue.hpp:187
    #1 0x... in ShmRingQueueUnit_Overflow_Test::TestBody()
          code/test/labor/test_shm_queue.cpp:45

0x... is located 0 bytes after 64-byte region [0x...,0x...)
allocated by thread T0 here:
    #0 0x... in ShmRingQueue::Create(unsigned int, unsigned int)
          code/Net/include/labor/types/ShmRingQueue.hpp:130

SUMMARY: AddressSanitizer: heap-buffer-overflow ShmRingQueue.hpp:187 in ShmRingQueue::TryEnqueue
```
调用栈里出现 `net::` 或 `ShmRingQueue::` 前缀的函数，说明是 Thunder 代码本身的问题，需要修。只出现 `OpenSSL::` 或 `grpc::` 的是第三方，可暂时忽略。

---

## TSan — 数据竞争检测

```cpp
// /tmp/test_tsan.cpp
#include <thread>
int counter = 0;
int main() {
    std::thread t1([] { counter++; });
    std::thread t2([] { counter++; });
    t1.join(); t2.join();
}
```

```bash
g++ -fsanitize=thread -o /tmp/test_tsan /tmp/test_tsan.cpp
/tmp/test_tsan
```

**有错时的实际输出**：
```
==================
WARNING: ThreadSanitizer: data race (pid=...)
  Read of size 4 at 0x... by thread T2:
    #0 operator() /tmp/test_tsan.cpp:5          ← 读操作位置（T2 线程）

  Previous write of size 4 at 0x... by thread T1:
    #0 operator() /tmp/test_tsan.cpp:4          ← 写操作位置（T1 线程）

  Location is global 'counter' of size 4 at 0x...  ← 竞争的变量
```
关键字段：`data race`、两条线程的调用栈（一读一写）、竞争变量名。

### Thunder：EtcdGrpcConnector 双线程并发

`EtcdGrpcConnector` 有两条并发路径：gRPC 回调线程（`GrpcThreadMain`）和 libev 主循环线程，通过 `m_eventMutex` / `m_cmdMutex` / `atomic<bool>` 保护共享状态。`build_tsan` 已用 `-fsanitize=thread` 编译。

```bash
# 第一步：准备抑制文件（过滤第三方竞争）
cat > /tmp/tsan.supp <<'EOF'
race:grpc_core::
race:grpc::
race:google::protobuf::
race:ev_async_send
EOF

# 第二步：启动服务，触发 gRPC 注册流程（双线程并发）
TSAN_OPTIONS="suppressions=/tmp/tsan.supp:log_path=/tmp/tsan_log" \
    build_tsan/bin/Hello -d deploy/HelloHttp/conf/Hello.json --nodaemon &
sleep 3 && kill %1

# 第三步：检查 Thunder 源码竞争数
grep "WARNING: ThreadSanitizer" /tmp/tsan_log.* 2>/dev/null | grep -v "tsan.supp" | wc -l
```

**无错时**（当前实际结果）：
```
0
```

**有错时**（例如 `m_eventMutex` 未加锁就访问共享队列）：
```
WARNING: ThreadSanitizer: data race (pid=...)
  Write of size 8 at 0x... by thread T2 (gRPC 回调线程):
    #0 0x... in EtcdGrpcConnector::GrpcThreadMain()
          code/Net/src/register/EtcdGrpcConnector.cpp:210
    #1 0x... in std::thread::_M_run ...

  Previous read of size 8 at 0x... by thread T1 (libev 主循环):
    #0 0x... in EtcdGrpcConnector::AsyncCb(ev_loop*, ev_async*, int)
          code/Net/src/register/EtcdGrpcConnector.cpp:163

  Location is a member 'm_eventQueue' of object 0x... of type EtcdGrpcConnector
```
两条线程的函数名都出现在 `EtcdGrpcConnector.cpp` 里，说明是 Thunder 源码竞争，需要加锁。
