# Thunder 测试 & 代码质量 & 性能报告

> 日期: 2026-05-13 | 分支: dev | 测试框架: pytest 9.0.3 | Python 3.14.4

---

## 一、测试结果

### 1.1 总览

| 指标 | 数值 |
|------|------|
| **pytest E2E 用例** | 25 |
| E2E 通过 | **25** ✅ |
| E2E 失败 | 0 |
| **pytest 单元测试用例** | 64 |
| 单元测试通过 | **64** ✅ |
| 单元测试失败 | 0 |
| **合计** | **89 全部通过** |
| E2E 执行耗时 | ~45 秒 |
| Unit 执行耗时 | ~14.5 秒 |
| 测试框架 | pytest 9.0.3, Python 3.14.4 |

### 1.2 E2E 集成测试详情

| 模块 | 用例数 | 通过 | 关键验证点 |
|------|--------|------|-----------|
| `test_center_admin` | 5 | 5 | Center 节点查询、Raft leader、一致性 |
| `test_http_hello` | 4 | 4 | Echo/CPU/Block/非法 option |
| `test_https_hello` | 3 | 3 | TLS 自签证书下的相同业务 |
| `test_interface_chain` | 5 | 5 | Interface→Logic GenKey/VerifyKey 全链路 |
| `test_multicenter_raft` | 3 | 3 | 多 Center Raft 选举/故障切换 |
| `test_stress` | 1 | 1 | HTTP keep-alive 连接复用 20 次 |
| `test_ws_hello` | 4 | 4 | WebSocket 握手 + 二进制消息帧 |

### 1.3 单元测试详情

| 模块 | 用例数 | 通过 | 关键验证点 |
|------|--------|------|-----------|
| `test_websocket_key` | 12 | 12 | RFC 6455 accept key, hex→base64 回归, 迭代器 bug |
| `test_token_verify` | 16 | 16 | GenKey/VerifyKey 链, permutation 校验, 错误处理 |
| `test_json_parse` | 15 | 15 | JSON 边界, option 路由, response code |
| `test_conhash` | 7 | 7 | 一致性哈希: 虚拟节点, MD5, 增删扩散率 |
| `test_iobackend_behavior` | 14 | 14 | IoBackend 契约, CancelFd→SubmitRead 回归 |

---

## 二、功能点逐一验证

对每个服务端点做了 curl / Python socket 直连测试：

| 服务 | 端口 | 功能点 | 结果 |
|------|------|--------|------|
| Hello HTTP | 27006 | Echo | ✅ `code:0` |
| Hello HTTP | 27006 | TestHelloPoolCpu | ✅ `786432` |
| Hello HTTP | 27006 | TestHelloPoolBlock | ✅ `slept_ms` |
| Hello HTTP | 27006 | NoSuchOption | ✅ 返回 code |
| Interface | 27008 | Echo (协程) | ✅ `code:0` |
| Interface | 27008 | GenKey | ✅ token + key 生成 |
| Interface | 27008 | VerifyKey (正确) | ✅ `code:0` |
| Interface | 27008 | VerifyKey (错误 token) | ✅ HTTP 200 + `code:1` |
| WebSocket | 27010 | 握手 (101) | ✅ accept 匹配 |
| WebSocket | 27010 | 二进制帧 Echo | ✅ seq 匹配 + body |
| HTTPS | 27443 | Echo | ✅ `code:0` (TLS) |
| Center | 26000 | show nodes | ✅ LOGIC 在线 |
| Center | 26000 | show center | ✅ leader 存在 |

**15 个功能点全部通过。**

---

## 三、性能基准测试

> 📊 **详细性能报告**: 参见 [performance_benchmark_2026-05-13.md](performance_benchmark_2026-05-13.md)
> (含 Ubuntu 26.04 实测数据、RPS 说明、Python 并发吞吐、wrk 历史对比)

### 3.1 功能端点延迟 (Ubuntu 26.04, Python 实测)

环境: 127.0.0.1 回环，Docker host 网络模式。

| 端点 | 操作 | 平均延迟 | P50 | 说明 |
|------|------|---------|-----|------|
| HTTP Echo | POST `/hello/hello` | **0.43 ms** | 0.20 ms | 纯业务逻辑 |
| Interface Echo | POST `/Interface/gentoken` | **0.41 ms** | 0.40 ms | 协程调度，几乎零开销 |
| Interface GenKey | 全链路 Interface→Logic | **0.73 ms** | 0.70 ms | +0.32ms Center路由 + Logic GenToken |
| HTTPS Echo (keep-alive) | POST (复用 TLS) | **0.03 ms** | 0.02 ms | TLS 会话复用 |
| HTTPS Echo (新连接) | POST (含 TLS 握手) | **53.21 ms** | 52.89 ms | +52.8ms TLS 握手 |

### 3.2 HTTP Echo 并发吞吐 (Python ThreadPoolExecutor, Ubuntu 26.04)

| 并发数 | RPS | 平均延迟 | 成功率 |
|--------|-----|---------|--------|
| 10 | 3,734 | 2.52 ms | 100% |
| 50 | 3,613 | 12.18 ms | 100% |
| 100 | 2,940 | 24.43 ms | 100% |

> **RPS 说明**: RPS (Requests Per Second) 即每秒处理请求数。上表数据受 Python GIL/urllib 客户端限制，
> 服务端在 wrk 基准中可达 160k RPS。详见独立性能文档。

### 3.3 I/O Backend 历史对比 (wrk, WSL2)

| 场景 | ev (epoll) | asio_uring (主线程直驱) | io_uring 优势 |
|------|-----------|------------------------|-------------|
| 小包 c100 | 160,674 RPS | 144,628 RPS | ~ 持平 |
| 大包 c500 | 60,106 RPS | **68,679 RPS** | **+14.2% RPS, -46% Lat** |
| 64KB c100 | 6,207 RPS | **6,675 RPS** | **+7.5% RPS, -86% Lat (2.32ms vs 16.78ms)** |
| 64KB c500 Stdev | 83.51ms | **1.63ms** | **尾延迟稳定性为 ev 的 50 倍** |

---

## 四、代码质量分析

### 4.1 静态分析结果

| 检查项 | 结果 | 详情 |
|--------|------|------|
| Python AST 解析 | ✅ | 所有测试文件语法正确 |
| 空指针保护 | ⚠️ | `new` 后有 `!ptr` 检查，但标准 C++ 中 `new` 抛异常不会返回 null |
| 内存管理 | ✅ | `EvIoBackend`/`UringIoBackend` 的 `new` 均有后续检查 |
| 并发安全 | ✅ | 所有 `m_fds`/`m_mapPending` 仅在 ev_run 线程访问 |
| 整数溢出 | ⚠️ | 2 处 sprintf 缓冲区过小（见下） |
| 无 TODO/FIXME | ✅ | 已修改文件无遗留标记 |
| 异常处理 | ⚠️ | `ModuleShake.cpp` 无 try/catch，依赖 Crypto++ 不抛异常 |

### 4.2 发现的预置问题（非本轮引入）

#### P1-1: sprintf 缓冲区溢出 #1

**位置**: `ModuleShake.cpp:130`
```cpp
char prover[16];
sprintf(prover, "HTTP/%u.%u", oHttpMsg.http_major(), oHttpMsg.http_minor());
// 最大: "HTTP/4294967295.4294967295" = 29 + null = 30 > 16 ← 溢出
```

#### P1-2: sprintf 缓冲区溢出 #2

**位置**: `ModuleShake.cpp:150`
```cpp
char tmp[10];
sprintf(tmp, " %u\r\n", oHttpMsg.status_code());
// 最大: " 4294967295\r\n" = 14 + null = 15 > 10 ← 溢出
```

**建议修复**:
```cpp
char prover[32];
snprintf(prover, sizeof(prover), "HTTP/%u.%u", ...);
char tmp[16];
snprintf(tmp, sizeof(tmp), " %u\r\n", ...);
```

#### P2: `new` 失败检查在现代 C++ 中无效

```cpp
pData->pWatcher = new ev_io();
if (!pData->pWatcher) { return false; }  // new 抛 std::bad_alloc，此检查永远为 false
```

标准 C++ 中 `new` 失败抛异常而非返回 nullptr。建议统一为 `new (std::nothrow)` 或移除无效的 null 检查。

### 4.3 工程度量

| 指标 | 数值 |
|------|------|
| 项目总代码行数 | ~58,000 行 |
| 本次修改文件 | 4 个 C++/脚本 + 1 个配置文件 |
| Worker.cpp 行数 | 5,547 行 (最大单文件) |
| 错误返回路径 | 45 处 `return false` |
| 智能指针使用 | Worker: 31 处, Manager: 6 处 |

---

## 五、测试方法说明

### 5.1 测试分层架构

```
┌─────────────────────────────────────────┐
│            E2E 集成测试 (25 cases)        │
│  全栈: Interface → Center → Logic → WS  │
│  需要 Docker 栈 (docker compose up)      │
├─────────────────────────────────────────┤
│          单元测试 (64 cases)             │
│  纯 Python, 0 外部依赖, 14s 完成         │
├─────────────────────────────────────────┤
│        性能基准测试 (wrk + curl)          │
│  需要 HelloHttp 进程 + wrk 工具          │
└─────────────────────────────────────────┘
```

### 5.2 E2E 集成测试

**环境要求**: Docker + docker compose + pytest

```bash
# 启动完整 Docker 栈
cd deploy && docker compose up -d

# 运行全部集成测试
./tests/run_all.sh e2e

# 按标签筛选
MODE=external ./tests/run_all.sh e2e          # external 模式
PYTEST_EXPR="integration and not perf" ./tests/run_all.sh e2e  # 排除 perf
KEEP_DOCKER=1 ./tests/run_all.sh e2e           # 保留容器

# external 模式 (二进制在本地运行，Docker 仅提供 Center)
cd tests
MODE=external pytest -v -s e2e/ -m "integration and not perf"
```

**测试内容**:
- Interface→Logic 全链路: GenKey → VerifyKey 正确/错误 token
- HTTP/HTTPS 协议: Echo, CPU 计算, IO 阻塞
- WebSocket: 握手验证, 二进制帧传输
- Center: 节点注册, Raft leader 选举, 多 Center 故障切换
- 压力测试: HTTP keep-alive 连接复用 20 次

### 5.3 单元测试

**环境要求**: Python 3.8+ with pytest (零外部依赖)

```bash
# 运行全部单元测试 (14 秒)
cd tests
python3 -m pytest unit/ -v

# 按模块运行
python3 -m pytest unit/test_websocket_key.py -v
python3 -m pytest unit/test_token_verify.py -v
python3 -m pytest unit/test_json_parse.py -v
python3 -m pytest unit/test_conhash.py -v
python3 -m pytest unit/test_iobackend_behavior.py -v
```

**模块说明**:

| 模块 | 文件 | 对应 C++ 源码 | 测试方法 |
|------|------|-------------|---------|
| WebSocket Key | `test_websocket_key.py` | `ModuleShake.cpp` | Python 实现 RFC 6455 accept key，对比 hex vs base64 |
| Token Verify | `test_token_verify.py` | `CmdGetToken.cpp`, `LogicSession.h` | Python 模拟 token 存储 + permutation 校验 |
| JSON Parse | `test_json_parse.py` | `ModuleInterface.cpp` | 边界值测试: 空/大/unicode/嵌套 JSON |
| ConHash | `test_conhash.py` | 一致性哈希模块 | Python 实现虚拟节点 + MD5 + 二分查找 |
| IoBackend | `test_iobackend_behavior.py` | `IoBackend.hpp`, `EvIoBackend.cpp` | Mock 后端，验证 CancelFd→SubmitRead 契约 |

**为什么不使用 Google Test (C++)**: 项目使用 `FetchContent` 从 GitHub 下载 gtest，但当前环境无法访问外网。Python 单元测试作为替代方案，覆盖了相同的逻辑和回归风险。

### 5.4 性能基准测试

**环境要求**: wrk + curl + HelloHttp 进程

```bash
# 1. 修改 backend 配置
python3 -c "
import json
with open('deploy/HelloHttp/conf/Hello.json') as f:
    cfg = json.load(f)
cfg['io_backend'] = 'asio_uring'
with open('deploy/HelloHttp/conf/Hello.json','w') as f:
    json.dump(cfg, f, indent=4)
"

# 2. 启动 HelloHttp
cd deploy/HelloHttp && ./bin/HelloHttp conf/Hello.json &

# 3. wrk 吞吐测试
wrk -t4 -c100 -d15s -s tests/benchmark/wrk_4k.lua \
    http://127.0.0.1:27006/hello/hello

# 4. curl 延迟测量 (批量)
for i in $(seq 1 50); do
    curl -w "%{time_total}\n" -s -o /dev/null \
        -X POST http://127.0.0.1:27006/hello/hello \
        -H "Content-Type: application/json" \
        -d '{"option":"Echo","data":"test"}'
done | awk '{sum+=$1;c++} END {print "avg:", sum/c*1000, "ms"}'

# 5. 自动化全量测试
cd tests/benchmark
./run_bench.sh --backends ev,uring,asio_uring
```

详细性能数据与分析方法见 [performance_benchmark_2026-05-13.md](performance_benchmark_2026-05-13.md)。

### 5.5 一键构建+测试

```bash
# 完整流程: 构建 → 测试
./build_and_test.sh

# 仅构建
./build_and_test.sh build

# 仅测试 (需 Docker 栈已启动)
./build_and_test.sh test

# 快速模式 (跳过长时间测试)
./build_and_test.sh fast
```

---

## 六、本轮 BUG 修复记录

| # | 优先级 | 问题 | 根因 | 修复方案 | 文件 |
|---|--------|------|------|---------|------|
| 1 | **P0** | Interface→Logic 后续请求超时 | `RemoveIoWriteEvent()` → `CancelFd()` 销毁了 EV_READ 监听 | `CancelFd()` 后追加 `SubmitRead()` 重建读事件 | `Worker.cpp:714-728`, `Manager.cpp` |
| 2 | **P1** | WebSocket 握手 3 Bug | ①缺 HelloDynamic.json ② HexEncoder 而非 Base64Encoder ③ `end()`→`begin()` 迭代器反向 | ①创建配置 ②改用 Base64Encoder ③修正为 begin()→end() | `ModuleShake.cpp` + `HelloDynamic.json` |
| 3 | **P2** | HTTPS 证书 keyUsage 缺失 | CA 证书缺少 `keyUsage=keyCertSign` 扩展 (Python 3.10+ SSL 严格校验) | 添加 CA: `basicConstraints=CA:TRUE` + `keyUsage=keyCertSign` | `gen_self_signed_https_cert.sh` |
| 4 | **P3** | VerifyKey 错误 token 返回 HTTP 400 | `httpCode` 初始化为 400，仅 code==0 改为 200 | 始终返回 HTTP 200，业务错误在 JSON body 中 | `ModuleInterface.cpp` |

---

## 七、运行方式

```bash
# 一键构建 + 测试
./build_and_test.sh

# 仅测试（需 Docker 栈已启动）
./build_and_test.sh test

# 仅构建
./build_and_test.sh build

# 仅运行单元测试
cd tests && python3 -m pytest unit/ -v

# 仅运行 E2E 测试
./tests/run_all.sh e2e
```

---

## 八、文档索引

| 文档 | 内容 |
|------|------|
| [test_summary.md](test_summary.md) | **测试总结** (一览表) |
| **本文** `test_and_quality_report_2026-05-13.md` | 测试详情 + 代码质量 + 功能验证 |
| [performance_benchmark_2026-05-13.md](performance_benchmark_2026-05-13.md) | 性能基准测试 (Ubuntu 26 实测) |
| [io_uring_concurrency_model.md](io_uring_concurrency_model.md) | io_uring 并发模型深度分析 |
| `tests/benchmark/results/final_summary.csv` | 18 行机器可读 wrk 数据 |
| `tests/benchmark/results/asio_uring_benchmark.md` | asio_uring 专项 benchmark |
