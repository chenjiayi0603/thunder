# 构建变体说明与验证矩阵

> 更新：2026-06-18

Thunder 维护多个并行构建目录，每个目录针对一个特定质量维度。本文记录各目录的 CMake 参数、验证目标、当前状态，以及何时该用哪个。

---

## 一、变体总览

| 目录 | BUILD_TYPE | 编译标志 | I/O 后端 | 验证什么 | 当前状态 |
|---|---|---|---|---|---|
| `build` | RelWithDebInfo | 无 | asio+io_uring | 日常开发、CI 基线 | ✅ 常态维护 |
| `build_release` | Release (-O3) | 无 | libev/epoll | 生产性能基准 | ✅ 用于压测对比 |
| `build-asan` | Debug | `-fsanitize=address,undefined` | libev/epoll | 内存越界 + 未定义行为 | ✅ 可用，#85 场景未跑 |
| `build-tsan` | Debug | `-fsanitize=thread` | libev/epoll | 线程竞态（早期） | ✅ 可用 |
| `build_tsan` | RelWithDebInfo | `-fsanitize=thread -g -O1` | libev/epoll | 线程竞态（当前） | ✅ #107 验证已完成 |
| `build_asio_uring` | RelWithDebInfo | 无 | asio+io_uring | asio+io_uring 后端行为 | ✅ 用于后端对比 |
| `build_asan` | RelWithDebInfo | **无（失效）** | libev/epoll | ~~ASan~~（未生效） | ❌ CMake option 未实现 |
| `build_no_https` | RelWithDebInfo | 无 | asio+io_uring | ~~无 TLS~~（未生效） | ❌ CMake option 未实现 |

---

## 二、各变体详解

### 2.1 `build` — 开发主构建

**配置**
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

**验证内容**
- 所有功能特性的正确性（ctest 356 cases）
- E2E 集成测试（`./deploy.sh test e2e`，30 cases）
- asio+io_uring 后端（当前线上默认 I/O 模型）

**何时用**：日常开发、每次提交前的 CI 回归。

---

### 2.2 `build_release` — 生产基准构建

**配置**
```bash
cmake -S . -B build_release \
  -DCMAKE_BUILD_TYPE=Release \
  -DTHUNDER_IO_URING=OFF \
  -DTHUNDER_IO_ASIO_URING=OFF
```

**验证内容**
- `-O3 -DNDEBUG`，最高优化，关闭断言
- I/O 后端：libev/epoll（无 io_uring）
- 目的：与 `build`（asio+io_uring）对比吞吐量，隔离"后端差异"变量

**已验证结果**（`docs/reports/08-io-perf-benchmark-20260606.md`）
- asio+io_uring vs libev/epoll：吞吐差异 < 5%（本机测试）
- 结论：io_uring 在当前负载下无明显增益，但也无回退

**何时用**：性能压测时，作为 libev 基线；与 `build_asio_uring` 对比排除优化级别影响。

---

### 2.3 `build-asan` — AddressSanitizer + UndefinedBehaviorSanitizer

**配置**
```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DTHUNDER_IO_ASIO_URING=OFF
```

**验证内容**

| 检测器 | 捕获的错误类型 |
|---|---|
| AddressSanitizer (ASan) | 堆/栈越界、use-after-free、use-after-return、double-free |
| LeakSanitizer (LSan) | 进程退出时的内存泄漏（默认附带 ASan） |
| UndefinedBehaviorSanitizer (UBSan) | 整数溢出、空指针解引用、非法类型转换 |

**关联 Issue**：#85（TLS 资源泄漏验证，`SSL_CTX` / `SSL*` / `BIO*`）

**运行环境变量**
```bash
export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:leak_check_at_exit=1"
export LSAN_OPTIONS="report_objects=1:max_leaks=100"
```

**已验证**：见 `docs/quality/01-asan-lsan.md`（#85 场景 T1~T6 待跑）

**注意**：ASan 与 TSan 互斥，不能同时开。Debug 模式避免内联导致调用栈不完整。

---

### 2.4 `build-tsan` / `build_tsan` — ThreadSanitizer

两个目录均为 TSan，区别在于优化级别：

| | `build-tsan`（早期） | `build_tsan`（当前） |
|---|---|---|
| BUILD_TYPE | Debug | RelWithDebInfo |
| CXX_FLAGS | `-fsanitize=thread` | `-fsanitize=thread -g -O1` |
| 优化级别 | -O0 | -O1（TSan 推荐：够快、帧不内联）|
| I/O 后端 | libev/epoll | libev/epoll |

**验证内容**：两个线程并发读写同一数据，且无 mutex 保护 → 数据竞争。

**关键被测场景（#107 EtcdGrpcConnector）**

```
gRPC 回调线程                    libev 主循环线程
─────────────────               ─────────────────
GrpcThreadMain()                ConnectorInit()
  m_eventMutex.lock()           m_stopFlag.load()   ← atomic，安全
  m_eventQueue.push()           m_cmdMutex.lock()
  ev_async_send()  ←──────────→ m_cmdQueue.pop()
```

**已验证结果**（`docs/reports/tsan-and-raft-failover-2026-06-18.md`，2026-06-18）
- 总报告：37 条竞争，**全部在 gRPC/abseil 第三方库内部**
- Thunder 源码竞争：**0 条**
- `m_eventMutex` / `m_cmdMutex` / `atomic<bool>` 保护正确

**何时重跑**：修改 `EtcdGrpcConnector.cpp` 中任何多线程共享状态时。

```bash
# 重建（只编译 Hello，不 install）
cmake --build build_tsan --target Hello -j$(nproc)

# 运行（抑制第三方竞争）
TSAN_OPTIONS="suppressions=build_tsan/tsan.supp:log_path=/tmp/tsan_log" \
  build_tsan/bin/Hello -d deploy/HelloHttp/conf/Hello.json --nodaemon &
# 发请求触发并发路径，然后 kill
grep "WARNING: ThreadSanitizer" /tmp/tsan_log.* | grep -v "tsan.supp" | wc -l
# 预期：0（Thunder 源码无竞争）
```

---

### 2.5 `build_asio_uring` — asio+io_uring 后端隔离

**配置**
```bash
cmake -S . -B build_asio_uring \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DTHUNDER_IO_ASIO_URING=ON
```

**验证内容**
- standalone asio + io_uring 路径的功能正确性
- 与 `build_release`（libev/epoll）对比：相同负载下的 QPS / 延迟 / CPU

**已知问题**：64K 以上 body 时 asio_uring 后端可能崩溃（issus #71 相关）。

**何时用**：切换 I/O 后端时的回归；性能对比实验。

---

### 2.6 `build_asan`（下划线）— ⚠️ 当前失效

**问题**：配置时传了 `-DTHUNDER_ASAN=ON`，但 `CMakeLists.txt` 没有消费该变量，导致实际 `CMAKE_CXX_FLAGS` 为空，没有注入 `-fsanitize=address`。构建出的二进制是普通二进制，不会检测内存错误。

**修复方法**：在 `CMakeLists.txt` 加入：
```cmake
option(THUNDER_ASAN "Enable AddressSanitizer + UBSan" OFF)
if(THUNDER_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
```
完成后可统一用 `-DTHUNDER_ASAN=ON` 代替 `build-asan` 的手动 CXX_FLAGS。

**结论**：在修复前，ASan 验证请用 `build-asan`（连字符版本）。

---

### 2.7 `build_no_https`（下划线）— ⚠️ 当前失效

**问题**：同上，`-DTHUNDER_ENABLE_HTTPS=OFF` 的值为 `UNINITIALIZED`，CMakeLists.txt 未消费，HTTPS/TLS 仍然编译进去。

**实际用途（未来）**：在不需要 OpenSSL 的嵌入式或精简环境下编译，去掉 libssl 依赖。

**修复方法**：
```cmake
option(THUNDER_ENABLE_HTTPS "编译 HTTPS/WSS TLS 支持" ON)
if(NOT THUNDER_ENABLE_HTTPS)
  # 从 NET_ALL_SOURCES 中排除 HttpsCodec / WssCodec
  list(FILTER NET_ALL_SOURCES EXCLUDE REGEX ".*[Hh]ttps.*|.*[Ww]ss.*")
  add_compile_definitions(THUNDER_NO_HTTPS)
endif()
```

---

## 三、使用决策树

```
改了什么？
│
├─ 多线程共享状态（EtcdGrpcConnector / ShmRingQueue / Manager）
│   └─→ build_tsan，看 Thunder 源码竞争数是否为 0
│
├─ 内存分配 / TLS 资源 / 指针操作
│   └─→ build-asan，跑相关 ctest + LSan 检查
│
├─ I/O 后端切换（io_uring / libev / asio）
│   └─→ build_asio_uring vs build_release，压测对比
│
├─ 功能逻辑 / API / 协议
│   └─→ build（默认），ctest + E2E
│
└─ 性能回归（怀疑引入了性能下降）
    └─→ build_release，wrk 压测与历史基线对比
```

---

## 四、重建速查

```bash
# 日常构建（最常用）
cmake --build build -j$(nproc)

# TSan（只编译 Hello，不 install）
cmake --build build_tsan --target Hello -j$(nproc)

# ASan（不 install，避免覆盖生产二进制）
cmake --build build-asan --target thunder_test_shm_queue -j$(nproc)

# Release 基准
cmake --build build_release -j$(nproc)
```

---

## 五、待办

| 项目 | 优先级 | 说明 |
|---|---|---|
| 修复 `build_asan`（实现 THUNDER_ASAN option） | 中 | 依赖 #85 完成 |
| 修复 `build_no_https`（实现 THUNDER_ENABLE_HTTPS option） | 低 | 精简部署需求 |
| EtcdGrpcConnector 多端点 failover TSan 验证 | 中 | 连接切换路径有新并发逻辑 |
| #85 场景 T1~T6 在 `build-asan` 下实际执行 | 高 | TLS 资源泄漏尚未用 ASan 确认 |
