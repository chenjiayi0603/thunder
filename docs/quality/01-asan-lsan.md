# ASan / LeakSanitizer 测试过程与结果

> 关联 issue：#85（issus-list.md）  
> 目标：验证 TLS 连接资源（SSL_CTX / SSL* / BIO*）无内存泄漏  
> 格式：每次运行追加「运行记录」章节，保留历史，可随时回归对比

---

## 1. 构建方法

### 1.1 CMake ASan target（需先合入 #85 的 CMake 改动）

```bash
# 配置 ASan 构建目录（独立于正常 build）
cmake -S /home/tommychen/thunder \
      -B /home/tommychen/thunder/build_asan \
      -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_ASAN=ON

# 构建（只编译 codec 测试目标）
cmake --build /home/tommychen/thunder/build_asan \
      --target test_codec_wss test_codec_https \
      -j$(nproc)
```

> **注意**：ASan 与 TSan 不能同时开启；ASan 与 Release 优化(-O2)同时使用时部分帧会被内联，建议 Debug 模式。

### 1.2 运行环境变量

```bash
export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:leak_check_at_exit=1"
export LSAN_OPTIONS="report_objects=1:max_leaks=100"
```

### 1.3 当前状态（#85 未实现前）

在 #85 实现前，可直接对现有 `build/` 目录的测试二进制加 ASAN_OPTIONS 运行（无 ASan instrumentation，LSan 不会检测到 Thunder 代码泄漏，但验证运行无崩溃）：

```bash
# 临时验证：用系统 ASan wrapper（不精确，仅作参考）
ASAN_OPTIONS=detect_leaks=0 ./build/code/test/codec/test_codec_wss --gtest_filter="WssCodec.*"
```

---

## 2. 测试矩阵

每个场景独立可运行，失败时可单独重跑。

### 场景 T1：正常连接-断开循环（100 次）

**目标**：`EnsureState` 创建的 SSL_CTX + SSL* + BIO × 2，`RemoveConnection` 后全部释放。

**测试文件**：`code/test/codec/test_codec_tls_leak.cpp`（#85 实现后新增）

```bash
./build_asan/code/test/codec/test_codec_tls_leak \
    --gtest_filter="TlsLeak.ConnectDisconnectCycle"
```

**通过标准**：
- [ ] gtest PASSED
- [ ] LSan 报告：`ERROR: LeakSanitizer: detected memory leaks` **不出现**
- [ ] 程序退出码 0

---

### 场景 T2：握手 PAUSE 中途断开

**目标**：`SSL_do_handshake` 返回 `WANT_READ` 时调用 `RemoveConnection`，资源不泄漏。

```bash
./build_asan/code/test/codec/test_codec_tls_leak \
    --gtest_filter="TlsLeak.HandshakePauseDisconnect"
```

**通过标准**：
- [ ] gtest PASSED
- [ ] LSan 0 泄漏

---

### 场景 T3：oPendingPlainSend 缓冲区断开

**目标**：握手未完成时 `EncodeToConnection` 将数据写入 `oPendingPlainSend`，断连时 pending buffer 被清空。

```bash
./build_asan/code/test/codec/test_codec_tls_leak \
    --gtest_filter="TlsLeak.PendingPlainSendOnDisconnect"
```

**通过标准**：
- [ ] gtest PASSED
- [ ] LSan 0 泄漏

---

### 场景 T4：现有 WssCodec 全量用例（基线回归）

**目标**：现有 11 个 WssCodec 用例在 ASan 模式下无新增泄漏或崩溃。

```bash
./build_asan/code/test/codec/test_codec_wss
```

**通过标准**：
- [ ] 11/11 PASSED（证书不存在时 3 个 TLS 用例 SKIPPED 可接受）
- [ ] LSan 0 泄漏

---

### 场景 T5：HttpsCodec 连接-断开循环（100 次）

**目标**：HttpsCodec 与 WssCodec 使用相同 TlsConnState，同样验证。

```bash
./build_asan/code/test/codec/test_codec_tls_leak \
    --gtest_filter="TlsLeak.HttpsConnectDisconnectCycle"
```

**通过标准**：
- [ ] gtest PASSED
- [ ] LSan 0 泄漏

---

### 场景 T6：WorkerThreadPool 全局 `new` 泄漏（关联 #95）

**目标**：验证 `ThunderWorkerThreadPool()` 的裸 `new` 是否触发 LSan 报告。

```bash
# 运行任何使用线程池的测试
ASAN_OPTIONS="detect_leaks=1" \
    ./build_asan/code/test/codec/test_codec_wss \
    --gtest_filter="WssCodec.TlsHandshakeCompletes"
```

**通过标准（修复前/后对比）**：

| 状态 | LSan 输出 |
|------|---------|
| 修复前（裸 `new`）| `LEAK: std::threadpool` 出现 |
| 修复后（`unique_ptr` #95 已修复）| 0 泄漏，unique_ptr 静态析构时自动 delete |

---

## 3. 运行记录

> 每次运行在此追加，格式固定，保留历史。

---

### Run #1 — 2026-06-14（基线，ASan build 未实现）

**运行人**：tommychen  
**目的**：建立基线，确认现有用例在无 ASan instrumentation 下运行正常  
**构建**：正常 build（无 -fsanitize），ASAN_OPTIONS=detect_leaks=0

| 场景 | 命令 | 结果 | 备注 |
|------|------|:----:|------|
| T4 基线（现有 WssCodec 11 用例）| `./build/code/test/codec/test_codec_wss` | ⏳ 待运行 | #85 CMake 改动未合入 |
| T1~T3、T5 | 新增用例未写 | ⏳ 待实现 | #85 未完成 |
| T6 线程池裸 new | 无 ASan，无法检测 | ⏳ 待运行 | #95 未完成 |

**结论**：基线记录完成。等 #85 CMake target 合入后执行 Run #2。

---

## 4. 快速回归命令

```bash
# 全量 ASan 回归（#85 完成后）
cd /home/tommychen/thunder
cmake -B build_asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build_asan -j$(nproc) \
    --target test_codec_wss test_codec_https test_codec_tls_leak

ASAN_OPTIONS="detect_leaks=1:abort_on_error=1" \
LSAN_OPTIONS="report_objects=1" \
    build_asan/code/test/codec/test_codec_tls_leak && \
    build_asan/code/test/codec/test_codec_wss && \
echo "✅ ASan/LSan 全量通过" || echo "❌ 存在泄漏或崩溃"
```

---

## 5. 常见 LSan 输出解读

```
=================================================================
==12345==ERROR: LeakSanitizer: detected memory leaks

Direct leak of 4096 byte(s) in 1 object(s) allocated from:
    #0 0x... in operator new(unsigned long)
    #1 0x... in SSL_CTX_new                 ← OpenSSL 创建 CTX
    #2 0x... in net::HttpsCodec::EnsureState
    #3 0x... in net::HttpsCodec::Decode
```

**判断是否是 Thunder 的泄漏**：
- 调用栈含 `net::` 前缀 → Thunder 代码负责释放 → 需修复
- 调用栈仅含 `SSL_*` / `BIO_*` → 检查 `RemoveConnection` 是否调用了对应的 `SSL_free` / `BIO_free_all`
- 调用栈仅含 `std::threadpool` → 关联 #95，`static local` 修复后消失

---

## 6. 新增测试场景方法

1. 在 `code/test/codec/test_codec_tls_leak.cpp` 增加 `TEST(TlsLeak, 新场景名称)`
2. 在本文档「测试矩阵」章节追加对应行（场景编号递增）
3. 在「运行记录」中下次 Run 时执行并记录结果
4. 通过后在 issus-list.md #85 验收标准对应项打勾
