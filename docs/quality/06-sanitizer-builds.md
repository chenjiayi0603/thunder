# Sanitizer 构建 — ASan / TSan / UBSan

> 构建目录: `build-asan/`, `build-tsan/`, `build-ubsan/`
> CMake 版本: 4.3 | 编译器: GCC (需 ≥ 4.8 for ASan, ≥ 4.8 for TSan, ≥ 4.9 for UBSan)

Thunder 有三套独立的 sanitizer 构建目录，用于在开发和 CI 阶段自动检测三类运行时错误。

---

## 背景

Thunder 是一个 **C++20 高性能网关框架**，其代码特征决定了它天然容易踩三类坑：

| 代码特征 | 风险 | 传统手段的局限 |
|:---|:---|:---|
| 手动内存管理 (`CBuffer`、`mmap` 共享内存、Protobuf Arena) | 缓冲区溢出、Use-After-Free、内存泄漏 | valgrind 太慢 (20-50×), 无法在生产构建中运行 |
| 无锁并发 (ShmRingQueue、work-stealing 线程池、`std::atomic`) | 数据竞争、memory order 错误 | 竞争不会 crash, 只在生产环境概率性丢消息, 无法用 gdb 复现 |
| 底层位运算 (协议压缩/加密 control bits、网络包解析 `reinterpret_cast`) | 有符号溢出、移位越界、类型截断 | 编译器不报错, 结果"碰巧正确"直到某天边界触发 |

这三类问题有一个共同点：**不会稳定复现，一旦在生产环境触发就是疑难杂症**。Sanitizer 通过在编译期插桩检测代码，在测试阶段就能暴露这些问题。

## 目的

1. **ASan** — 确保所有 `new/delete`、`malloc/free`、缓冲区操作没有越界和泄漏，覆盖 `CBuffer` 扩容、Protobuf 序列化、共享内存边界
2. **TSan** — 确保无锁队列 (ShmRingQueue、work-stealing deque) 和跨线程 event queue (`etcd gRPC ↔ libev`) 的 `std::atomic` memory order 正确，无数据竞争
3. **UBSan** — 确保整数运算、指针转换、类型截断在所有边界条件下不会触发未定义行为 (启用 `-fno-sanitize-recover=all` 遇错即 abort，不留隐患)

**三者必须全部通过才能合并代码** — 它们检测的问题互不重叠，缺一不可。

> 对比 valgrind: Sanitizer 的插桩在编译期完成，运行时开销仅 1.2-15×，而 valgrind 是动态二进制翻译，开销 20-50×。
> Sanitizer 可以直接在 CI 中随单元测试运行；valgrind 太慢，通常只在怀疑有内存问题时手动跑。

---

## 概览

| 构建目录 | Sanitizer | 编译标志 | 检测什么 | 性能开销 |
|:---|:---|:---|:---|:---:|
| `build-asan/` | AddressSanitizer | `-fsanitize=address` | 内存错误 | ~2× |
| `build-tsan/` | ThreadSanitizer | `-fsanitize=thread` | 数据竞争 | ~5-15× |
| `build-ubsan/` | UndefinedBehaviorSanitizer | `-fsanitize=undefined -fno-sanitize-recover=all` | 未定义行为 | ~1.2× |

三者共同标志: `-fno-omit-frame-pointer` (ASan/TSan) — 保留栈帧指针以生成可读的错误报告。

---

## 1. AddressSanitizer (ASan) — `build-asan/`

### 构建配置

```
CMAKE_BUILD_TYPE: Debug
CMAKE_CXX_FLAGS:  -fsanitize=address -fno-omit-frame-pointer
CMAKE_C_FLAGS:    -fsanitize=address -fno-omit-frame-pointer
LINKER_FLAGS:     -fsanitize=address
Generator:        Ninja
```

### 检测项目

| 类别 | 具体检测 | 示例 |
|:---|:---|:---|
| **堆缓冲区溢出** | heap-buffer-overflow | `buf = malloc(10); buf[10] = 0;` |
| **栈缓冲区溢出** | stack-buffer-overflow | `char buf[10]; buf[10] = 0;` |
| **全局缓冲区溢出** | global-buffer-overflow | 全局数组越界访问 |
| **Use-After-Free** | heap-use-after-free | `free(p); *p = 1;` |
| **Use-After-Return** | stack-use-after-return | 返回局部变量地址后使用 |
| **Use-After-Scope** | stack-use-after-scope | lambda 捕获已析构的局部变量 |
| **Double-Free** | 重复释放同一指针 | `free(p); free(p);` |
| **内存泄漏** | LeakSanitizer (内置) | `malloc` 后未 `free` |
| **分配器边界** | alloc-dealloc-mismatch | `new` 配 `free` / `malloc` 配 `delete` |

### 功能验证 — 最小示例

用一段独立代码验证 ASan 确实在工作（不属于 Thunder 项目，仅验证编译环境和 sanitizer 生效）：

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

**实际输出** (进程非零退出)：

```
==1742917==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x...
WRITE of size 4 at 0x... thread T0
    #0 0x... in main /tmp/test_asan.cpp:4       ← 出错文件和行号
    #1 0x... in __libc_start_call_main ...

0x... is located 0 bytes after 16-byte region [0x...,0x...)
allocated by thread T0 here:
    #0 0x... in operator new[](unsigned long)
    #1 0x... in main /tmp/test_asan.cpp:3       ← 内存分配位置

SUMMARY: AddressSanitizer: heap-buffer-overflow /tmp/test_asan.cpp:4 in main
```

关键字段：错误类型 (`heap-buffer-overflow`)、出错行号 (`#0 in main`)、分配位置。

### Thunder 项目验证 — ShmRingQueue

`ShmRingQueue` 是共享内存无锁环形队列，直接操作裸内存指针。用 ASan 验证其内存安全：

```bash
# 缺省检测 (仅报错, 不 abort)
./build-asan/bin/thunder_test_shm_queue

# 严格模式: 遇错即 abort + 退出时检查泄漏
ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:leak_check_at_exit=1" \
    ./build-asan/bin/thunder_test_shm_queue
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

ASan 常用环境变量：

| 变量 | 值 | 作用 |
|:---|:---|:---|
| `ASAN_OPTIONS=detect_leaks=1` | 默认开启 | 退出时检测内存泄漏 |
| `ASAN_OPTIONS=abort_on_error=1` | 默认关闭 | 首个错误即 abort (CI 推荐) |
| `ASAN_OPTIONS=leak_check_at_exit=1` | 默认开启 | 进程退出时做泄漏检查 |

### 如何运行

```bash
# 手动配置 (已预置, 一般不需重做)
cmake -S . -B build-asan -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
    -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address"

# 构建
cmake --build build-asan -j$(nproc)

# 运行测试 (任何报错说明存在 bug)
./build-asan/bin/thunder_test_e2e_smoke
./build-asan/bin/thunder_test_util_buffer
# ... 逐个运行所有 thunder_test_* 可执行文件
```

### 输出示例 (当检测到 bug 时)

```
=================================================================
==12345==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x...
READ of size 4 at 0x... thread T0
    #0 0x... in MyFunction() /path/to/file.cpp:42
    #1 0x... in main /path/to/main.cpp:10

0x... is located 0 bytes to the right of 10-byte region
allocated by thread T0 here:
    #0 0x... in malloc
    #1 0x... in MyFunction() /path/to/file.cpp:40
```

> ASan 运行时额外占用约 **2-3× 虚拟地址空间** (shadow memory)。Docker/CI 环境中需确保 `ulimit -v` 或容器 memory limit 足够。

---

## 2. ThreadSanitizer (TSan) — `build-tsan/`

### 构建配置

```
CMAKE_BUILD_TYPE: (default, ≈RelWithDebInfo)
CMAKE_CXX_FLAGS:  -fsanitize=thread -fno-omit-frame-pointer
LINKER_FLAGS:     -fsanitize=thread
Generator:        Unix Makefiles
```

### 检测项目

| 类别 | 具体检测 | Thunder 高风险场景 |
|:---|:---|:---|
| **数据竞争** | data race (两个线程同时访问同一内存, 至少一个写, 无同步) | ShmRingQueue 无锁读写、Worker 线程池 work-stealing |
| **锁顺序反转** | lock-order-inversion (潜在死锁) | 多 mutex 嵌套加锁 |
| **信号处理竞争** | signal-unsafe call inside signal handler | Manager SIGTERM/SIGCHLD 处理 |
| **线程泄漏** | thread leak | `std::thread` detach 后未 join |
| **互斥量误用** | mutex 双重解锁、非拥有者解锁 | — |

### 功能验证 — 最小示例

两个线程无保护地递增共享变量，验证 TSan 确实检测到数据竞争：

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

**实际输出** (TSan 报告两条线程对同一变量的一读一写)：

```
==================
WARNING: ThreadSanitizer: data race (pid=...)
  Read of size 4 at 0x... by thread T2:
    #0 operator() /tmp/test_tsan.cpp:5          ← 读操作位置 (T2 线程)

  Previous write of size 4 at 0x... by thread T1:
    #0 operator() /tmp/test_tsan.cpp:4          ← 写操作位置 (T1 线程)

  Location is global 'counter' of size 4 at 0x...  ← 竞争的变量
```

关键字段：`data race`、两条线程的调用栈（一读一写）、竞争变量名。

### Thunder 项目验证 — EtcdGrpcConnector 双线程并发

`EtcdGrpcConnector` 有两条并发路径：gRPC 回调线程 (`GrpcThreadMain`) 和 libev 主循环线程，通过 `m_eventMutex` / `m_cmdMutex` / `atomic<bool>` 保护共享状态。

```bash
# 1. 准备抑制文件 (过滤第三方库的数据竞争)
cat > /tmp/tsan.supp <<'EOF'
race:grpc_core::
race:grpc::
race:google::protobuf::
race:ev_async_send
EOF

# 2. 启动服务，触发 gRPC 注册流程 (双线程并发)
TSAN_OPTIONS="suppressions=/tmp/tsan.supp:log_path=/tmp/tsan_log" \
    ./build-tsan/bin/Hello -d deploy/HelloHttp/conf/Hello.json --nodaemon &
sleep 3 && kill %1

# 3. 检查 Thunder 源码竞争数 (过滤掉抑制文件中已忽略的)
grep "WARNING: ThreadSanitizer" /tmp/tsan_log.* 2>/dev/null | grep -v "tsan.supp" | wc -l
```

**无错时** (当前实际结果): `0`

**有错时** (例如 `m_eventMutex` 未加锁就访问共享队列):

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

TSan 常用环境变量：

| 变量 | 值 | 作用 |
|:---|:---|:---|
| `TSAN_OPTIONS=suppressions=<file>` | 文件路径 | 抑制第三方库的已知竞争 (需在 Thunder 中过滤 gRPC/Protobuf) |
| `TSAN_OPTIONS=log_path=<dir>` | 目录路径 | 将报告写入文件 (避免与 stdout 混合) |
| `TSAN_OPTIONS=history_size=7` | 默认 2 | 增加调用栈深度以定位根因 |

### Thunder 项目中 TSan 的特别价值

Thunder 的核心并发模型是：

```
单线程事件循环 per Worker (无锁)
    +
Work-stealing 线程池 (无锁 MPMC queue)
    +
Manager-Worker ShmRingQueue (共享内存, 原子操作)
    +
etcd gRPC 线程 ↔ libev 主线程 (event queue + ev_async)
```

这些路径的正确性依赖 `std::atomic` 的 memory order。**TSan 是唯一能在测试中捕获 memory order 错误的工具** — 数据竞争不会导致 crash 或 ASan 报错，只在生产环境表现为概率性丢消息或错误路由。

### 如何运行

```bash
# 配置
cmake -S . -B build-tsan \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"

# 构建
cmake --build build-tsan -j$(nproc)

# 运行 (关注 ShmRingQueue, threadpool, work-stealing 测试)
./build-tsan/bin/thunder_test_shm_queue
./build-tsan/bin/thunder_test_util_threadpool
./build-tsan/bin/thunder_test_util_work_stealing_pool
./build-tsan/bin/thunder_test_util_worker_deque
./build-tsan/bin/thunder_bench_work_stealing
```

### 输出示例

```
==================
WARNING: ThreadSanitizer: data race (pid=12345)
  Write of size 4 at 0x7b... by thread T2:
    #0 Worker::StealTask() worker.cpp:156
  Previous read of size 4 at 0x7b... by thread T1:
    #0 Worker::PopTask() worker.cpp:142
  Location is global 'g_taskQueue' of size 64
```

### 注意事项

- **不能与 ASan 同时使用**。如同时需要 ASan + TSan 覆盖率，分别运行。
- TSan 运行时额外占用 **5-10× 内存** (per-thread shadow state)。
- 性能开销 **5-15×**，测试执行时间显著延长。

---

## 3. UndefinedBehaviorSanitizer (UBSan) — `build-ubsan/`

### 构建配置

```
CMAKE_BUILD_TYPE: (default, ≈RelWithDebInfo)
CMAKE_CXX_FLAGS:  -fsanitize=undefined -fno-sanitize-recover=all
LINKER_FLAGS:     -fsanitize=undefined
Generator:        Unix Makefiles
```

关键标志 `-fno-sanitize-recover=all`: **遇到任何 UBSan 错误立即 abort**，不继续执行。这确保 UB 不会静默产生错误结果后继续运行。

### 检测项目

| 类别 | 具体检测 | Thunder 相关代码 |
|:---|:---|:---|
| **有符号整数溢出** | `signed-integer-overflow` | Protobuf 序列号递增、连接计数、队列索引 |
| **除零** | `integer-divide-by-zero` | 一致性哈希取模、负载均衡计算 |
| **空指针解引用** | `null` | `shared_ptr.get()` 返回、插件 dlopen 失败路径 |
| **未对齐指针** | `alignment` | `reinterpret_cast` 网络包头解析 |
| **移位越界** | `shift-base / shift-exponent` | 位运算标志位 (压缩/加密 control bits) |
| **类型转换越界** | `float-cast-overflow / implicit-conversion` | `int64_t` → `int32_t` 截断 |
| **返回无值** | `return` (non-void 函数无 return) | — |
| **Vptr 误用** | `vptr` (虚函数表错误) | 基类析构函数非 virtual |
| **数组边界** | `bounds` | `std::array` / C 数组访问 |
| **枚举越界** | `enum` | `switch` 未覆盖所有枚举值 |

### 功能验证 — 最小示例

有符号整数溢出是最常见的 UB，用一段独立代码验证 UBSan 确实在工作：

```cpp
// /tmp/test_ubsan.cpp
#include <climits>
int main() {
    int x = INT_MAX;
    x = x + 1;   // 有符号整数溢出 — 未定义行为
    return x;
}
```

```bash
g++ -fsanitize=undefined -fno-sanitize-recover=all -o /tmp/test_ubsan /tmp/test_ubsan.cpp
/tmp/test_ubsan
```

**实际输出** (进程 abort)：

```
/tmp/test_ubsan.cpp:4:9: runtime error: signed integer overflow:
    2147483647 + 1 cannot be represented in type 'int'
SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior /tmp/test_ubsan.cpp:4:9
[1]    12345 abort      /tmp/test_ubsan
```

关键字段：错误类型 (`signed integer overflow`)、出错位置 (文件:行号)、触发立即 abort 不继续执行。

### 如何运行

```bash
# 配置
cmake -S . -B build-ubsan \
    -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=undefined" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=undefined"

# 构建
cmake --build build-ubsan -j$(nproc)

# 运行全部测试 (UB 触发直接 abort, 适合批量 CI)
./build-ubsan/bin/thunder_test_e2e_smoke
./build-ubsan/bin/thunder_test_util_json       # yyjson arena allocator
./build-ubsan/bin/thunder_test_util_buffer     # CBuffer 扩容/缩容
./build-ubsan/bin/thunder_test_codec_proto     # Protobuf 编解码
./build-ubsan/bin/thunder_test_dispatcher_conhash  # 取模运算
```

### 输出示例

```
test_util_buffer.cpp:42:15: runtime error: signed integer overflow:
    2147483647 + 1 cannot be represented in type 'int'
SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior test_util_buffer.cpp:42:15
[1]    12345 abort      ./build-ubsan/bin/thunder_test_util_buffer
```

---

## 构建产物对比

| 构建 | Generator | Build Type | 磁盘占用 | Hello 二进制 |
|:---|:---|:---|:---:|:---:|
| `build/` | Unix Makefiles | RelWithDebInfo | 1.7G | 50M |
| `build-asan/` | **Ninja** | Debug | 1.5G | 47M |
| `build-tsan/` | Unix Makefiles | (default) | 1.4G | 42M |
| `build-ubsan/` | Unix Makefiles | (default) | 1.8G | 59M |

- ASan 用 Ninja 生成器 (构建最快)
- UBSan 二进制最大 (59M) 因为 `-fno-sanitize-recover=all` 插桩了更多检查点

---

## 检测范围矩阵

| 构建的可执行文件 | ASan | TSan | UBSan |
|:---|:---:|:---:|:---:|
| **31 个测试** (`thunder_test_*`) | ✅ | ✅ | ✅ |
| **4 个基准** (`thunder_bench_*`) | ✅ | ✅ | ✅ |
| **Hello** (生产二进制) | ✅ | ✅ | ✅ |

所有 36 个可执行文件在三个构建中的集合完全一致，覆盖了 Thunder 全部 C++ 代码路径。

---

## CI 集成建议

```
提交前必须:
  build-asan: 全部 31 测试通过              ← 阻塞合并
  build-ubsan: 全部 31 测试通过 (abort=FAIL)  ← 阻塞合并

周期性 (每夜) 或 PR 可选:
  build-tsan: 全部 31 测试通过              ← TSan 太慢, 非阻塞但需关注

运行耗时估算 (31 测试, i9-12900H):
  ASan:  ~2 min  (2× 开销)
  TSan:  ~8 min  (5-15× 开销)
  UBSan: ~1 min  (1.2× 开销)
```

---

## 已知注意事项

1. **不可混用**: ASan 和 TSan 互斥，不能同时传递给 `-fsanitize=`。
2. **ASan + LeakSanitizer**: 默认随 ASan 启用。用 `ASAN_OPTIONS=detect_leaks=0` 可单独关闭。
3. **动态库**: 当 `.so` 插件由 ASan 构建的 Hello 加载时，插件 .so 也必须用 ASan 编译 (否则 shadow memory 冲突)。
4. **AddressSanitizer 需要虚拟地址空间**: Docker 中可能需要 `--security-opt seccomp=unconfined` 或调整 `vm.overcommit_memory`。
5. **TSan 假阳性**: 对 `std::atomic` 的 `memory_order_relaxed` 操作可能报伪竞争，需人工审查。
