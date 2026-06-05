# Thunder 测试体系设计

> 2026-06-02 | 四层测试金字塔

---

## 一、测试金字塔

```
                        ┌─────────┐
                        │ 手动冒烟 │  7 项, 2 分钟
                        │  curl   │  人眼验证核心链路
                       ┌┴─────────┴┐
                       │ Docker E2E │  26 cases, 3 分钟
                       │  真实集群  │  全协议链路, 8 容器
                      ┌┴───────────┴┐
                      │  Python 单元 │  60 cases, 0.04 秒
                      │  纯逻辑      │  JSON/Token/WS/ConHash
                     ┌┴─────────────┴┐
                     │   C++ 单元     │  288 cases, 4 秒
                     │   纯逻辑       │  21 个模块全覆盖
                     └───────────────┘
```

## 二、C++ 单元测试 (gtest)

### 运行

```bash
./deploy.sh test unit --skip-build
# 或: cd build/code/test && ctest -j1 --output-on-failure
```

### 覆盖模块 (288 cases)

| 模块 | cases | 验证内容 |
|------|-------|---------|
| **CenterRaft** | 29 | Raft 多数派、MergeRing、节点ID分配、游标归一化 |
| **CJsonObject** | 36 | JSON 解析/构造/拷贝/数组/嵌套/错误处理 |
| **FastPath** | 30 | HTTP 方法/QueryString/Content-Length/KeepAlive/分块 |
| **NetInterface** | 16 | 配置读取/Launch/Register/Json2Pb/Pb2Json |
| **TcpCenterConnector** | 22 | 单多Center/消息消费/Raft Leader/double init |
| **CBuffer** | 21 | 读写往返/边界越界/大块数据/SkipBytes/Discard |
| **HttpCodec** | 12 | HTTP请求响应编解码/POST body/大响应/状态码 |
| **ShmRingQueue** | 10 | 入队出队/空满/SPSC/fork子进程/Worker重启模拟 |
| **ConHash** | 11 | 一致性哈希/增删节点/确定性/空环 |
| **RedisOperator** | 10 | 字段添加/结构设置/Pipeline命令 |
| **ThreadPool** | 8 | 任务提交/Future/多任务并发/析构等待 |
| **Session** | 8 | ID构造/超时回调/永久标记/活跃时间 |
| **ConnectionAttr** | 8 | fd/seq/收发缓冲/Identify/RemoteAddr |
| **ClientMsgCodec** | 8 | 编解码往返/空body/截断/损坏 |
| **ProtoCodec** | 8 | 编解码往返/空body/截断/多消息 |
| **ProtoCoor** | 9 | Raft/NodeReport 序列化往返 |
| **ProtoMsg** | 7 | MsgHead/MsgBody/HttpMsg 序列化 |
| **MemOperator** | 7 | 构造/字段添加/清空/模型因子 |
| **StepCo20** | 5 | HttpRespAwaiter/协程回调/挂起/lambda |
| **ThunderOrmMysql** | 4 | InsertFuture/AsyncInsert 异常路径 |
| **ThunderOrmRedis** | 3 | SetFuture/AsyncSet 异常路径 |
| **WorkerDrain** | 7 | 排空逻辑: 空闲关闭/在途保留/S2S跳过/超时 |
| **E2ESmoke** | 3 | Center/HTTP GET/POST (需运行中服务, 跳过) |
| **Coroutine20** | 1 | C++20协程异步任务头文件可用性 |

### 单功能快速回归

```bash
ctest -R WorkerDrain          # 只跑排空 (7 cases, ~0.1s)
ctest -R CenterRaft           # 只跑 Raft (29 cases, ~0.5s)
ctest -R HttpCodec            # 只跑 HTTP (12 cases)
ctest -R ConHash              # 只跑一致性哈希 (11 cases)
ctest -R FastPath             # 只跑 FastPath (30 cases)
```

---

## 三、Python 单元测试 (pytest)

### 运行

```bash
python3 -m pytest tests/unit/ -v
```

### 覆盖模块 (60 cases)

| 模块 | cases | 验证内容 |
|------|-------|---------|
| **test_json_parse** | 16 | 空/Null/嵌套/Unicode/大body/布尔/数字/数组/转义/option分发 |
| **test_token_verify** | 16 | GenKey→VerifyKey/错误token/permutation/多token隔离/空值拒绝 |
| **test_websocket_key** | 12 | RFC6455示例/空key/base64往返/确定性/非hex回归/迭代器bug |
| **test_iobackend_behavior** | 10 | SubmitRead/Write/CancelFd/HasPending/Name/CancelFd后补交回归 |
| **test_conhash** | 6 | 单节点/空环/多节点分布/增删节点最小重分配/确定性 |

---

## 四、Docker 集成测试 (E2E)

### 集群拓扑

```
8 个容器, host 网络, 真实 TCP/TLS/HTTP/WS/Raft:

  Center × 1     (27000/27022/27032)  — Raft 3 节点集群
  Logic    × 1   (16068)              — 业务逻辑
  Interface × 1  (27008/27009)        — 客户端接入
  HelloHttp × 1  (27006)              — HTTP 示例
  HelloWs   × 1  (27010)              — WebSocket 示例
  HelloHttps × 1 (27443)              — HTTPS 示例
  Redis     × 1  (6379)               — 缓存
  MariaDB   × 1  (3306)               — 数据库
```

### 运行

```bash
./deploy.sh test e2e                # 构建 + 启动集群 + 测试 + 清理 (~3min)
./deploy.sh test e2e --skip-build   # 仅测试 (需集群已在运行)
./deploy.sh test e2e --keep-docker  # 测试后保留容器排障
```

### 覆盖链路 (26 cases)

| 模块 | cases | 验证内容 |
|------|-------|---------|
| **test_http_hello** | 4 | Echo/PoolCpu/PoolBlock/NoSuchOption |
| **test_https_hello** | 3 | TLSv1.3 握手 + Echo/PoolCpu/PoolBlock |
| **test_interface_chain** | 5 | GenKey/VerifyKey S2S链路/并发去重/错误拒绝 |
| **test_center_admin** | 5 | show nodes/center/leader一致性/非法cmd |
| **test_multicenter_raft** | 3 | Leader一致性/故障转移/业务链路可达 |
| **test_ws_hello** | 4 | WebSocket握手/Echo消息/CPU/Block |
| **test_stress** | 1 | 连接复用 keepalive |
| **test_wrk_smoke** | 1 | wrk 压测冒烟 |

---

## 五、手动冒烟

```bash
# 启动集群后, 快速验证 7 条核心链路
docker compose -p thunder-test up -d && sleep 20

# 1. HTTP Echo
curl -s http://127.0.0.1:27006/hello/hello -d '{"option":"Echo"}'
# {"code":0,"msg":"ok"}

# 2. HTTP PoolCpu (协程挂起/恢复)
curl -s http://127.0.0.1:27006/hello/hello -d '{"option":"TestHelloPoolCpu"}'
# {"option":"TestHelloPoolCpu","checksum":786432}

# 3. Interface→Logic S2S 全链路
curl -s http://127.0.0.1:27008/Interface/gentoken -d '{"option":"GenKey"}'
# {"code":0,"token":"...","key":"...","msg":"success"}

# 4. Center Raft 集群状态
curl -s http://127.0.0.1:26000/admin -d '{"cmd":"show","args":["center"]}'
# data 数组 3 节点, 1 个 leader=yes

# 5. 错误处理 (非法 token)
curl -s http://127.0.0.1:27008/Interface/gentoken -d '{"option":"VerifyKey","token":"bad","key":"bad"}'
# {"code":1}

# 6. WebSocket 握手
curl -sI -H "Upgrade: websocket" -H "Connection: Upgrade" \
     -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" \
     http://127.0.0.1:27010/hello/shake 2>&1 | head -1
# HTTP/1.1 101 Switching Protocols

# 7. 连接复用
for i in 1 2 3 4 5; do
  curl -s http://127.0.0.1:27006/hello/hello -d '{"option":"Echo"}'
done
# 5次全部 {"code":0,"msg":"ok"}
```

## 六、性能基准

```bash
# wrk 压测 (仅 ev 后端)
./tests/benchmark/run_quick_bench.sh

# 或手动:
wrk -t4 -c100 -d60s --latency http://127.0.0.1:27006/hello/hello
# 当前 ev 基准: ~158K RPS, P50=630μs
```

## 七、测试清单 (提交前)

```
□ ./deploy.sh build                      编译零告警
□ ctest -R <改动模块>                    单功能快速回归
□ ./deploy.sh test unit --skip-build     341 单元测试全部通过
□ ./deploy.sh test e2e  --skip-build     26+ E2E 全部通过
□ 手动冒烟 7 项                          全部返回预期值
```

## 八、测试耗时

```
单功能快速回归:   ctest -R <模块>         ~1 秒
单元测试全量:     unit (341 cases)        ~10 秒
Docker E2E:       e2e (26 cases)          ~3 分钟
手动冒烟:         7 项 curl               ~2 分钟
性能基准:         wrk 60s                 ~1 分钟
─────────────────────────────────────────────────
全量 (最慢):      unit + e2e + 冒烟       ~5 分钟
全量 (跳过e2e):    unit + 冒烟             ~3 分钟
快速迭代:         ctest -R <模块>         ~1 秒
```


---

## 九、回归测试详解

### 9.1 什么是回归测试

```
回归测试 ≠ 单元测试

单元测试: 测新写的代码对不对   → "我写对了吗"
回归测试: 测旧的代码坏了没有   → "我没把别人的代码搞坏吧"
```

改 `Worker::Run()` 加了个排空循环，单元测试只验证排空逻辑正确性。
回归测试要验证：HTTP Echo 还能用吗？Interface→Logic 的 GenKey 还能通吗？Center Raft心跳还正常吗？

### 9.2 回归范围由改动范围决定

```
改了什么                      回归什么
─────────────────────────────────────────────────────────────
Worker.cpp (IO/事件循环)       HTTP Echo, GenKey S2S, Manager IPC, Raft 心跳
Manager.cpp (进程管理/fork)    Worker 启动退出, OnChildTerminated, Center 注册
代码层 (dec)                    HTTP/HTTPS/WS 方法, ProtoCodec, FastPath
Proto (coor.proto)             Interface→Logic RPC, Center→Manager 协议
脚本 (node.sh/deploy.sh)      集群启停, 端口监听, 服务发现
配置 (JSON)                    所有读取该配置的路径
```

### 9.3 三层回归策略

```
第一层: 相关模块单元测试 (秒级)
  改了 Worker.cpp     → ctest -R "WorkerDrain|StepCo20" 
  改了 Manager.cpp    → ctest -R "CenterRaft|TcpCenterConnector"
  改了 HttpCodec      → ctest -R "HttpCodec|FastPath"
  改了 Proto          → ctest -R "ProtoMsg|ProtoCoor|ProtoCodec"

第二层: 集成测试 (分钟级)
  任何改动             → python3 -m pytest tests/unit/ -q        (60 cases)
  任何改动             → python3 -m pytest tests/e2e/ -q --mode=external

第三层: 全量回归 (提交前)
  任何改动             → ./deploy.sh test              (unit + e2e)
                    + 手动冒烟 7 项
```

### 9.4 回归检查清单 (按改动类型)

#### 改 Worker.cpp (最危险)

```
Worker.cpp 是框架核心, 6020 行, 影响一切

必跑:
  □ ctest -R "WorkerDrain"          排空逻辑
  □ ctest -R "StepCo20"             协程调度
  □ ctest -R "Coroutine"            协程基础
  □ python3 -m pytest tests/unit/   60 Python 单元
  □ Docker E2E 全量                  26 cases
  □ 手动冒烟 7 项                   核心链路
  □ 检查: Manager 日志有无 "error" 或 "fatal"
  □ 检查: Worker 进程是否正常退出 (ev_loop 不卡死)
```

#### 改 Manager.cpp

```
影响: Worker 生命周期, Center 通信, IPC

必跑:
  □ ctest -R "CenterRaft"           Raft 选主
  □ ctest -R "TcpCenterConnector"   Center 连接器
  □ ctest -R "ShmRingQueue"         共享内存队列
  □ Docker E2E: 特别关注 test_multicenter_raft
  □ 手动验证: kill -TERM Worker → Manager 重启成功
```

#### 改 Hello/Interface/Logic 业务插件

```
影响范围小, 只影响对应端点

必跑:
  □ ctest -R "NetInterface"         接口测试
  □ Docker E2E: 对应模块的 cases
  □ curl 手动验证受影响的端点
```

#### 改 Proto

```
影响: 所有使用该 proto 消息的序列化/反序列化

必跑:
  □ ctest -R "Proto"                所有 Proto 测试
  □ ctest -R "CenterRaft"           Raft 消息受影响
  □ Docker E2E 全量                 序列化变更影响全网
```

#### 改配置 JSON

```
影响: 端口/日志/插件加载

必跑:
  □ Docker 集群重启 → 所有服务 healthy
  □ ss -tln | grep <端口>           确认端口监听
  □ curl 对应的 API                 确认端点可用
```

### 9.5 常见回归遗漏 (最容易踩的坑)

| 改了 | 容易忘记测的 | 原因 |
|------|------------|------|
| Worker::Run() | Manager::OnChildTerminated | Worker 退出方式变了, Manager 处理逻辑可能不兼容 |
| AcceptClientConn | Interface 的 accept 路径 | 只测了 HelloHttp, 忘了 Interface 也用同一个函数 |
| SendTo | AutoSend/AutoConnect | SendTo 底层改了, 自动连接路径没测 |
| Codec | HTTPS/WSS | 改了 HTTP 解码, HTTPS 走同一个 Decode(换了一层TLS) |
| IoBackend | DPDK 后端 | 只测了 ev, asio_uring/native_uring 路径没跑 |
| Proto | Center↔Manager 消息 | 改了 coor.proto, 只测了 Worker 侧, Manager 侧没测 |

### 9.6 一键全量回归脚本

```bash
#!/bin/bash
# 保存为 tests/regression.sh
set -e

echo "=== Thunder 全量回归 ==="
echo ""

# 第一层: 单元测试
echo "--- C++ 单元测试 ---"
cd build/code/test && ctest -j1 --output-on-failure | tail -3

echo "--- Python 单元测试 ---"
python3 -m pytest tests/unit/ -q

# 第二层: Docker E2E
echo "--- Docker E2E ---"
./deploy.sh test e2e --skip-build

# 第三层: 手动冒烟 (需集群在运行)
echo "--- 手动冒烟 ---"
echo -n "HTTP Echo: "
curl -s http://127.0.0.1:27006/hello/hello -d '{"option":"Echo"}' | grep -q '"code":0' && echo "✅" || echo "❌"

echo -n "PoolCpu: "
curl -s http://127.0.0.1:27006/hello/hello -d '{"option":"TestHelloPoolCpu"}' | grep -q '786432' && echo "✅" || echo "❌"

echo -n "GenKey: "
curl -s http://127.0.0.1:27008/Interface/gentoken -d '{"option":"GenKey"}' | grep -q '"code":0' && echo "✅" || echo "❌"

echo -n "VerifyKey(bad): "
curl -s http://127.0.0.1:27008/Interface/gentoken -d '{"option":"VerifyKey","token":"bad","key":"bad"}' | grep -q '"code":1' && echo "✅" || echo "❌"

echo -n "Center Raft: "
curl -s http://127.0.0.1:26000/admin -d '{"cmd":"show","args":["center"]}' | grep -q '"leader":"yes"' && echo "✅" || echo "❌"

echo -n "404 route: "
curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:27006/hello/nosuch | grep -q "404" && echo "✅" || echo "❌"

echo -n "Keepalive 5x: "
ok=true; for i in 1 2 3 4 5; do
  curl -s http://127.0.0.1:27006/hello/hello -d '{"option":"Echo"}' | grep -q '"code":0' || ok=false
done; $ok && echo "✅" || echo "❌"

echo ""
echo "=== 回归完成 ==="
```


---

## 十、测试命令速查

### 命令对照

| 命令 | 编译 | C++单元 | Python单元 | Docker编排 | pytest E2E | 清理 |
|------|------|---------|-----------|-----------|-----------|------|
| `./deploy.sh test` | ✅ | ✅ 288 | ✅ 60 | ✅ up | ✅ 26 | ✅ down |
| `./deploy.sh test unit` | ✅ | ✅ 288 | ✅ 60 | — | — | — |
| `./deploy.sh test e2e` | ✅ | — | — | ✅ up | ✅ 26 | ✅ down |
| `./deploy.sh test unit --skip-build` | — | ✅ 288 | ✅ 60 | — | — | — |
| `./deploy.sh test e2e --skip-build` | — | — | — | 用已有集群 | ✅ 26 | ❌ 保留 |
| `python3 -m pytest tests/e2e/ -q --mode=external` | — | — | — | — | ✅ 26 | — |
| `ctest` | — | ✅ 288 | — | — | — | — |
| `./tests/regression.sh` | — | ✅ 288 | ✅ 60 | — | ✅ 26 | — |

### 常用场景

```bash
# 开发迭代: 改完代码快速验证编译+单元
./deploy.sh test unit --skip-build

# 改完集成代码: 集群已在运行, 只跑 E2E
python3 -m pytest tests/e2e/ -q --mode=external

# 提交前: 全量 (编译 + 单元 + E2E + 清理)
./deploy.sh test

# 提交前: 跳过编译 (已有 build)
./tests/regression.sh
```

### regression.sh vs deploy.sh test

```
regression.sh:
  · 不编译
  · 不编排 Docker (假设集群已在运行)
  · ctest + pytest unit + pytest e2e (--mode=external) + 冒烟

deploy.sh test:
  · 编译 + Docker build/up/down
  · ctest + pytest unit + pytest e2e (--mode=local)
  · 完整自动化, 但更慢 (~5分钟 vs ~2分钟)
```


---

## 十一、回归范围确定方法

### 11.1 不是拍脑袋, 是看代码依赖

```
改了文件 A → 谁 include 了 A 的头文件? → 谁调用了 A 的函数? → 谁的数据结构 A 在填充?

这些"谁"就是回归范围。
```

### 11.2 改动范围 → 回归范围映射表

| 改了哪个目录/文件 | 回归测试 | 原因 |
|------------------|---------|------|
| `code/Net/src/labor/Worker.cpp` | `ctest -R "WorkerDrain\|StepCo20\|Coroutine"` + E2E全量 + 冒烟 | 框架核心, 6020行, 影响一切 IO/事件/协议 |
| `code/Net/src/labor/Manager.cpp` | `ctest -R "CenterRaft\|TcpCenterConnector\|ShmRingQueue"` + E2E全量 | 进程管理+IPC+Center通信 |
| `code/Net/src/codec/HttpCodec.cpp` | `ctest -R "HttpCodec\|FastPath"` + HTTP E2E | HTTP 编解码 |
| `code/Net/src/codec/ProtoCodec.cpp` | `ctest -R "ProtoCodec\|ProtoMsg\|ProtoCoor"` + S2S E2E | 内部协议 |
| `code/Net/src/codec/HttpsCodec.cpp` | HTTPS E2E | TLS 编解码 |
| `code/Net/src/labor/IoBackend*` | `ctest -R "IoBackend"` + Python `test_iobackend_behavior` + E2E全量 | 影响所有 IO |
| `code/Net/src/step/` | `ctest -R "StepCo20"` + S2S E2E | 协程/Step 调度 |
| `code/Net/src/session/` | `ctest -R "Session"` + E2E全量 | 连接生命周期 |
| `code/Center/` | `ctest -R "CenterRaft"` + Raft E2E | Center 插件 |
| `code/Hello/` | HTTP/WS E2E | Hello 插件 |
| `code/Interface/` | Interface E2E | Interface 插件 |
| `code/Logic/` | S2S E2E + `test_token_verify` | Logic 插件 + Token 逻辑 |
| `code/Proto/coor.proto` | `ctest -R "Proto"` + E2E全量 | 序列化影响全网 |
| `code/Util/` | 全量单元 (所有模块都可能用到) | 工具库被全项目引用 |
| 配置 JSON | Docker 集群重启 + 冒烟 | 端口/插件加载/日志 |
| `deploy/*.sh` | 手动启停 + `ss -tln` 检查端口 | 部署脚本 |
| `cmake/` `CMakeLists.txt` | 全量编译 + 单元测试 | 编译选项变化影响所有 target |
| 新增文件 | 如果是 `.cpp` → 看哪个 target 链接了它 | target = new ctest 入口 |

### 11.3 快速确定: 一行命令看依赖

```bash
# 改了 Worker.cpp, 谁调用了 Worker 的方法?
grep -rn "Worker::" code/Net/src/ code/Center/src/ code/Hello/src/ code/Interface/src/ code/Logic/src/ \
  --include="*.cpp" | grep -v "Worker.cpp" | cut -d: -f1 | sort -u

# 改了 HttpCodec.hpp, 谁 include 了它?
grep -rn '#include.*HttpCodec' code/ --include="*.cpp" --include="*.hpp" | cut -d: -f1 | sort -u

# 改了 Proto, 哪些测试受影响?
grep -rn 'ProtoCodec\|ProtoMsg\|ProtoCoor\|coor.proto' code/test/ --include="*.cpp" | cut -d: -f1 | sort -u
```

### 11.4 回归脚本选择

```
只改了一个模块的 .cpp (不改头文件):
  → ctest -R <模块名> + python3 -m pytest tests/unit/ -q

改了头文件 (.hpp):
  → ctest 全量 (所有 include 它的模块都要测) + pytest 全量

改了 Worker.cpp 或 Manager.cpp:
  → ctest 全量 + pytest 全量 + E2E 全量 + 冒烟

改了 Proto:
  → ctest 全量 + E2E 全量 (序列化变更影响全网)

改了配置/脚本:
  → Docker 集群重启 + 冒烟
```

### 11.5 快速迭代 vs 提交前

```
快速迭代 (开发中):
  Worker.cpp 改了一行 → ctest -R WorkerDrain (5秒)
  → 立刻知道排空逻辑对不对
  → 改完再跑全量

提交前:
  ./tests/regression.sh
  → 不管改了啥, 全量过一遍
  → 保证没破坏别人的代码
```

