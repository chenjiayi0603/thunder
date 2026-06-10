# Thunder — Claude Code 项目配置

## 项目概述

Thunder 是一个基于 C++20 的分布式异步集群服务框架，提供 Center 注册发现、Worker 并发处理、HTTP/HTTPS/WebSocket 与内部二进制协议接入、可插拔模块（`.so`）等能力。

### 技术栈

| 类别 | 技术 |
|------|------|
| 语言 | C++20 (CMake >= 3.20) |
| 构建 | CMake + Ninja/Make, `RelWithDebInfo` 默认 |
| 事件循环 | libev |
| I/O 后端 | `EvIoBackend` (epoll) / `UringIoBackend` (liburing) / `AsioUringIoBackend` (standalone Asio io_uring, 主线程直驱) |
| 序列化 | Protobuf (`code/3party/protobuf/build/protoc`, `coor.proto`) |
| 协程 | C++20 协程 Step 体系 (`StepCo20`) + Awaitable |
| 数据库 | MariaDB (mariadb-connector-c) + MongoDB (mongo-c-driver) + Redis (hiredis-vip) |
| 网络 | curl + c-ares + OpenSSL |
| 日志 | log4cplus |
| 加密 | cryptopp |
| 内存 | jemalloc (可选) |
| 集群 | Raft 选主 + 共享内存路由镜像 |
| 测试 | pytest (unit 64 + e2e 25 = 89 cases) + wrk 压测 |
| 部署 | Docker + shell 脚本 (`nodes.sh`) |
| 文档入口 | `INSTALL.md` (快速开始), `README.md` |

---

## 目录结构

```
code/
  Net/               # 网络框架核心（事件循环、协程、编解码、会话管理、Step 状态机）
    include/         # 头文件（cmd/codec/coro/labor/protocol/session/step/storage）
    src/             # 实现（main.cpp、Manager.cpp、Worker.cpp、dispatcher/）
  Center/            # 中心节点（Raft 选主、注册、上报、Admin 管理页）
  Logic/             # 逻辑节点（CmdGetToken 等业务插件）
  Interface/         # 接口节点（HTTP 入口，ModuleHello 等）
  Hello/             # Hello 示例模块（CmdHello、ModuleHello、ModuleShake）
  Proto/             # Protobuf 协议定义（coor.proto）
  Util/              # 工具库（日志、DBI/ORM、线程、curl、Unix 工具）
  3party/            # 第三方子模块（libev/cryptopp/curl/hiredis-vip/log4cplus/
                    #   mariadb-connector-c/mongo-c-driver/protobuf/c-ares）
  test/              # C++ 单元测试（center/coroutine/interface/orm/step）
deploy/              # 安装产物、节点配置、启停脚本（nodes.sh）
tests/               # Python 测试（一键入口: run_all.sh）
  unit/              # 单元测试（64 cases, 零外部依赖）
  e2e/               # 端到端集成测试（25 cases, 需 Docker）
  benchmark/         # 性能基准测试（wrk + curl）
docs/                # 设计文档
  architecture/       # 架构设计 (7篇: IPC/优雅重启/优化)
  io/                 # I/O 后端 (6篇: io_uring/DPDK/策略)
  codec/              # 编解码器 (2篇: HTTPS)
  reports/            # 测试/性能报告 (5篇)
cmake/               # CMake 模块与构建说明
```

---

## 构建与验证

### ⚠️ 构建限制（强制）

- **所有 `cmake --build` 命令必须使用 `-j1`**，严禁多线程编译
- 原因：本机磁盘 IO 是瓶颈，多线程会导致系统卡死
- 违例：`-j$(nproc)`、`-j4`、`--parallel` 等均禁止

### 日常操作速查

```bash
# 一键构建安装 (冷启动)
git submodule update --init --recursive \
  && cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  && cmake --build build --target thirdparty_deploy -j1 \
  && cmake --build build -j1 \
  && cmake --install build

# 仅重编 (第三方已部署)
cmake --build build -j1 && cmake --install build

# 仅重编某模块
cmake --build build --target Net -j1
cmake --build build --target InterfacePlugins -j1

# Proto 变更后
cmake --build build --target thunder_proto_gen -j1
cmake --build build -j1
```

### 部署与测试

**统一入口：`./deploy.sh`**（所有测试/构建/部署操作）

```bash
# 构建
./deploy.sh build                 # cmake configure + build + install

# 测试 (速度层级: unit → e2e → bench)
./deploy.sh test unit             # C++ gtest + Python unit (~45s, 零外部依赖)
./deploy.sh test e2e              # Docker 集成测试 (~3min, 需 Docker)
./deploy.sh test                  # 全部: unit + e2e
./deploy.sh test bench            # wrk 性能基准

# Docker 环境管理
./deploy.sh up                    # 启动开发环境 (docker compose up -d)
./deploy.sh down                  # 停止并清理
./deploy.sh restart               # 重启
./deploy.sh status                # 查看容器状态 + 端口

# 清理
./deploy.sh clean                 # 清理 build/ + Docker + tmp

# 选项
./deploy.sh test unit --skip-build  # 跳过构建，只跑测试
./deploy.sh test e2e --keep-docker  # E2E 后保留容器排障
./deploy.sh test --verbose          # 详细输出
```

**旧命令仍可用**（内部转发到 deploy.sh）：
- `./tests/run_all.sh unit` → 等价 `./deploy.sh test unit`
- `./docker/dev_up_logs.sh` → 等价 `./deploy.sh up`

### 改完代码后必须验证

> 这是唯一的"改动后验证"清单，Agent 行为准则与测试规则均引用此处，勿在别处重复。

#### 改 C++ 代码后 (Agent 自动判断回归范围)

> Agent 根据改动文件自动决定跑哪些测试，不是每次全量跑。
> 回归范围映射表: `docs/reports/01-test-strategy.md` 第 11 章

**Agent 自动执行流程:**

```
Step 1: git diff 看改了什么文件
Step 2: 按映射表判断回归范围:
  ├─ 只改了单个模块 .cpp (不改 .hpp) → ctest -R <模块> + pytest unit
  ├─ 改了 .hpp                     → ctest 全量 (include 它的都得测)
  ├─ 改了 Worker.cpp/Manager.cpp    → ctest 全量 + E2E 全量 + 冒烟
  ├─ 改了 Proto                     → ctest 全量 + E2E 全量
  ├─ 改了配置/脚本                  → Docker 重启 + 冒烟
  └─ 新增文件                       → ctest 全量 (target 变了)
Step 3: 执行回归 → 贴结果
Step 4: 编译总检查: `cmake --build build -j1` 零告警
```

**提交前**: `./tests/regression.sh` (全量, 不管改了啥)

**涉及内存/并发改动**: ASan + valgrind + TSan 必跑并贴报告

#### 全功能回归测试（任何 C++/Proto/脚本改动后必跑）

> 不是只测改动的模块，是测**所有**功能是否被破坏。

```bash
# === 第一步: 编译 + 安装 ===
./deploy.sh build                                # 全量编译，零告警

# === 第二步: 单元测试 (281 C++ + 60 Python = 341 cases) ===
./deploy.sh test unit --skip-build               # 全部通过才继续

# === 第三步: Docker 集群 E2E (8服务, 全协议链路) ===
./deploy.sh test e2e --skip-build                # 全部 26+ cases 通过

# === 第四步: 手动冒烟 (核心链路) ===
docker compose -p thunder-test up -d              # 或 ./deploy.sh up
sleep 20

# HTTP Echo
curl -s http://127.0.0.1:27006/hello/hello -d '{"option":"Echo"}'
# 预期: {"code":0,"msg":"ok"}

# HTTP PoolCpu (协程挂起/恢复验证)
curl -s http://127.0.0.1:27006/hello/hello -d '{"option":"TestHelloPoolCpu"}'
# 预期: {"option":"TestHelloPoolCpu","checksum":786432}

# Interface → Logic S2S 全链路
curl -s http://127.0.0.1:27008/Interface/gentoken -d '{"option":"GenKey"}'
# 预期: {"code":0,"token":"...","key":"...","msg":"success"}

# Center Raft 集群状态 (3节点, 1 leader)
curl -s http://127.0.0.1:26000/admin -d '{"cmd":"show","args":["center"]}'
# 预期: data 数组有3个节点, 1个 leader=yes

# 错误处理: 非法 token
curl -s http://127.0.0.1:27008/Interface/gentoken -d '{"option":"VerifyKey","token":"bad","key":"bad"}'
# 预期: {"code":1}  (业务错误, 非 crash)

# WebSocket 握手 (E2E 已覆盖, 此处验证可达性)
curl -sI -H "Upgrade: websocket" -H "Connection: Upgrade" \
     -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" \
     http://127.0.0.1:27010/hello/shake 2>&1 | head -1
# 预期: HTTP/1.1 101 Switching Protocols

# === 第五步: 清理 ===
./deploy.sh clean                                 # 可选, 释放 Docker 资源
```

- [ ] 281 C++ gtest 全部通过 (不得有 FAIL)
- [ ] 60 Python pytest 全部通过
- [ ] Docker E2E 全部通过
- [ ] 手动冒烟 7 项全部返回预期值
- [ ] 编译零告警 (`-Wall -Wextra`)

#### 改 Proto 后

- [ ] `cmake --build build --target thunder_proto_gen -j1` — 重新生成 .pb.cc/.pb.h
- [ ] `./deploy.sh build` — 全量编译
- [ ] 联调冒烟测试

#### 改部署脚本后

- [ ] `./deploy.sh restart` — 启停正常
- [ ] `./deploy.sh status` — 所有服务状态正常

#### 禁止

- 改完代码不跑编译就提交
- 修改接口后不同步更新 Proto 和所有节点

#### 测试后必须清理

```bash
./deploy.sh clean    # 一键清理 build/ + Docker + tmp
```

---

## 开发规范

### 代码规范

| 规则 | 说明 |
|------|------|
| 语言 | 用中文回复和注释 |
| C++ 标准 | C++20 |
| 格式 | 遵循 `.clang-format` (项目根目录) |
| 提交信息 | 中文, format: `feat(module): description` 或 `fix(module): description` |
| 新功能 | 优先修改已有文件，不随意新建 |
| 注释原则 | 只在 WHY 不明显时写，不写 WHAT |

### 第三方库

- 版本以 `.gitmodules` gitlink 为准，勿随意升级
- Protobuf 只用 `code/3party/protobuf/build` 里的 protoc/libprotobuf，勿与系统旧版混用
- 默认安装前缀为 `deploy/`

---

## 测试问题管理规范

### 问题发现 → Issue → PR 闭环

任何测试中发现的 bug、缺陷、异常行为，必须走以下流程：

1. **发现问题 → 提 Issue**
   - 标题简洁描述问题现象
   - 内容包含：复现步骤、期望行为、实际行为、环境信息
   - 打上标签：`bug` / `enhancement` / `question`

2. **修复问题 → 提交 PR**
   - 分支命名：`fix/issue-{编号}-简短描述`
   - PR 描述中引用 `Closes #{issue编号}`
   - 修复后必须通过测试验证

3. **同类问题不重复出现**
   - 修复后检查是否还有其他同类问题
   - 在 Issue 中记录 root cause，方便后续检索
   - 必要时在 CLAUDE.md "常见问题" 区记录

### 提交规范

| 类型 | 分支名 | 提交信息示例 |
|------|--------|-------------|
| Bug 修复 | `fix/issue-12-xxx` | `fix: 修复 xxx (closes #12)` |
| 功能增强 | `feat/issue-15-xxx` | `feat: xxx (closes #15)` |
| 重构 | `refactor/issue-20-xxx` | `refactor: 清理冗余代码 (closes #20)` |

### 禁止

- 发现问题后不录 Issue 直接改代码
- 同一个 bug 出现两次不查 root cause
- Issue 关闭后不留修复记录和测试用例

---

## 关键架构决策

### I/O 后端 (IoBackend)

| 后端 | 配置值 | 读方式 | 写方式 | 集成方式 | 状态 |
|------|--------|--------|--------|---------|------|
| `EvIoBackend` | `"ev"` | ev_io + ReadFD | ev_io + WriteFD | per-fd epoll watcher | ✅ 默认 |
| `UringIoBackend` | `"uring"` | io_uring_prep_recv | WriteFD 同步 | ev_io(ring_fd) → ReapCqes | ⚠️ 待移除 |
| `AsioUringIoBackend` | `"asio_uring"` | async_read_some | async_write_some | ev_prepare/ev_check/ev_io(ring_fd) 三路 poll | ✅ 主力 |

详细分析见 `docs/io_uring_concurrency_model.md`。

### 集群与路由

- Center 集群 Raft 选主，主从语义下的注册/上报流程
- 路由镜像通过共享内存 (shm) 同步：Manager 写 shm → Worker 定时检查版本号 → 增量更新
- 配置同步：主 Center → 业务节点 Loader → 共享内存 → Worker 定时感知
- 共享内存布局：blob → len → version++ (原子递增，避免半包读取)
- 路由 shm 160KB / 配置 shm 160KB，超限不写入、打错误日志、Worker 继续用旧版本

### 多进程架构

- 多进程 Worker 架构 (Manager/Loader/Worker)，插件动态加载 (`Cmd*.so`、`Module*.so`)
- 基于事件驱动的异步网络模型 (libev)，支持高并发连接处理
- HTTP 编解码支持 HTTP/HTTPS/WebSocket 多种协议

---

## Agent 行为准则

### 1. 先思考再编码 (Think Before Coding)

- 不确定时必须停下来问，不能猜，不能假设
- 存在多种理解时列出选项让用户选，不要替用户做决定
- 发现更简单的方案时主动说出来
- 把 trade-off 摆出来，不要隐藏困惑
- **确认方案后才能改代码** — 用户说"好"或"可以"之后再动手，不要边想边改

### 2. 简洁优先 (Simplicity First)

- 50 行能写完绝不写 200 行
- 没人要求的"灵活性"和"可配置"不加
- 不可能发生的异常场景不做错误处理
- 不为未来可能的需求提前写代码

### 3. 精准修改 (Surgical Changes)

- 只动被要求动的部分，不顺手优化相邻代码
- 匹配项目已有的代码风格 (`.clang-format`)
- 看到不相关的问题提一嘴就行，别动手改
- 每一行改动都能追溯到用户的原始请求

### 4. 目标驱动执行 (Goal-Driven Execution)

| 目标 | 方法 |
|------|------|
| "修 Bug" | 先写能复现 Bug 的测试，再让测试通过 |
| "加校验" | 先写非法输入测试，再让它通过 |
| "重构 X" | 确保改前改后测试都通过 |
| 复杂任务 | 先列分步计划，每步带验证方式 |

### 5. 每次代码改动后必须验证

- 改动后的编译/测试/清理清单见 [构建与验证 → 改完代码后必须验证](#改完代码后必须验证)，改 C++ / 改 Proto / 改部署脚本各有对应项，提交前必跑，禁止跳过。
- 提交前还需对照 [代码审查检查清单](#代码审查检查清单提交前) 自查内存安全、竞态、性能。

---

## 功能状态

### 网络 I/O

- ✅ `EvIoBackend` — 默认后端，GET c100 167k RPS
- ✅ `AsioUringIoBackend` — 主线程直驱，大包优于 ev
- ✅ IoBackend 抽象接口 — 运行时按 `"io_backend"` 配置三档切换
- ✅ S2S 跨节点 TCP 接入 IoBackend — accept 后的连接走异步 I/O
- ✅ 协程 StepCo20 与 IoBackend 解耦 — 通过 `IoCompletionCallback` 回调衔接
- ⚠️ 移除手写 `UringIoBackend` — 待 asio_uring 全链路验证后执行

### AsioUringIoBackend 并发模型

- ✅ 主线程直驱 — io_context 跑在 libev 主线程，零锁零线程跳
- ✅ ev_prepare + ev_check + ev_io(ring_fd) 三路驱动 `io_context.poll()`
- ✅ 大包测试已覆盖 4KB + 64KB，三档横向对比完成
- ✅ S2S 端到端 (Interface→Logic GenKey) ENOTCONN 竞态已修复（commit 3e6939a）

### 集群与路由

- ✅ Center 集群 Raft 选主
- ✅ 节点注册/上报/断连检测
- ✅ 共享内存路由镜像
- ✅ 配置通过 shm 同步到 Worker

### 协议与编解码

- ✅ HTTP/1.1 / HTTPS (OpenSSL) / WebSocket (JSON + Protobuf)
- ✅ 内部二进制协议 (ProtoCodec / ThunderCodec)
- ✅ 自定义协议 (CODEC_PRIVATE / CODEC_APP)

### 模块与插件

- ✅ 插件动态加载 (`Cmd*.so` / `Module*.so`)
- ✅ Hello 示例模块 (HTTP Echo)
- ✅ Admin 管理后台 (Center Web 管理页)

### 存储

- ✅ MariaDB (mariadb-connector-c)
- ✅ MongoDB (mongo-c-driver)
- ✅ Redis (hiredis-vip)
- ✅ Protobuf 序列化 (`coor.proto`)

### 测试与压测

- ✅ pytest 单元测试 64 cases (零外部依赖, 14s)
- ✅ pytest E2E 集成/冒烟测试 25 cases (需 Docker)
- ✅ 一键测试入口 `tests/run_all.sh`
- ✅ wrk 三档横向压测 (GET / POST 小包/大包/64KB)
- ✅ Benchmark 结果文档化 (`docs/performance_benchmark_2026-05-13.md`)
- ✅ 综合测试报告 (`docs/test_and_quality_report_2026-05-13.md`)

---

## Contact

Gitee：chenjiayi/thunder

---

## Skill routing

当用户的请求匹配可用 skill 时，**优先使用 Skill 工具**，不要直接回答。

### 路由规则速查

| 触发词 | Skill | 做什么 |
|--------|-------|--------|
| "检查"、"代码质量"、"全面检查"、`/health` | `/health` | 代码质量仪表盘：类型检查 + lint + 测试 + 死代码 + shell lint，0-10 评分 |
| "review"、"审查"、"看下代码"、`/review` | `/review` | PR 前置审查：SQL 安全、竞态、性能、对抗性审查 |
| "bug"、"报错"、"不工作"、`/investigate` | `/investigate` | Bug 调查，排查错误和异常 |
| "测试"、"QA"、"验证"、`/qa` | `/qa` | 浏览器自动化测试 + 自动修复 |
| "DX"、"开发体验"、`/devex-review` | `/devex-review` | 开发者体验审计 |
| "保存"、"checkpoint"、`/checkpoint` | `/checkpoint` | 保存当前工作状态 |

### Skill 说明

- **`/health`**：运行类型检查 (clang-tidy/clangd)、lint (clang-format)、测试 (pytest)、死代码检测、shell lint，计算 0-10 综合评分并追踪趋势
- **`/review`**：分析 diff，调度 Performance 专项审查员（后端/前端代码自动触发性能审查）、Security 审查员、对抗性审查（主动寻找资源泄漏和性能隐患）、SQL 安全、竞态条件检测
- **`/investigate`**：Bug 调查，排查错误和异常
- **`/qa`**：浏览器自动化测试 Center 管理页，发现问题后自动修复并提交，产生健康评分
- **`/qa-only`**：只要报告，不要修复
- **`/devex-review`**：开发者体验审计

## Thunder框架测试规则

### 代码审查检查清单（提交前）

> 简洁清单，只列"查什么"。工具如何跑见 [改完代码后必须验证](#改完代码后必须验证)；自动化审查接 `/review` `/health` `/qa` skill。

#### 1. 内存非法访问

- use-after-free / 悬垂引用：lambda 与回调捕获引用、协程帧内引用跨挂起点存活
- 越界：buffer / 数组 / `string_view` / `span` 的索引与长度边界
- 未初始化：成员变量、栈对象、从 shm 读出的结构体
- 所有权不清：优先 RAII / `unique_ptr`，慎用裸 `new`/`delete`
- `shared_ptr` 循环引用导致泄漏
- 工具：ASan + valgrind，涉及内存改动必跑并贴报告

#### 2. 竞态条件

- 多进程 shm：version 原子递增、先写 blob 再写 len、防半包读取
- 协程：`co_await` 挂起期间被引用对象必须存活
- 共享可变状态缺锁 / double-checked locking 误用
- 信号处理函数只调用 async-signal-safe 接口
- 工具：TSan，涉及并发改动必跑并贴报告

#### 3. 语法与静态检查

- clang-format 强制（项目根 `.clang-format`），clang-tidy 通过
- 编译零告警：`-Wall -Wextra` 无 warning
- 头文件自包含、include 最小化（include-what-you-use）
- C++20 惯用法适度使用（concepts / ranges / span），不炫技

#### 4. 设计模式优化建议

- 对照本项目模式审查：IoBackend 策略、Step 状态机、插件 RAII 释放、ORM Repository
- 避免过度设计（呼应"简洁优先"：没要求的灵活性/可配置不加）
- 重复逻辑提取，避免 copy-paste 漂移

#### 5. 性能优化建议

- 移动语义 / `emplace` / 避免不必要的临时拷贝
- 大对象按 `const&` 入参，热路径避免内存分配、复用 buffer
- 协程帧 / 虚函数开销在热路径上权衡
- 先 profile 再优化，给真实数据，不拍脑袋

#### 6. QA 检查（接入 skill）

- `/review`：PR 前置审查，覆盖上述 1/2/4/5
- `/health`：类型/lint/测试/死代码 0-10 评分
- `/qa`：浏览器自动化测试 + 自动修复

### 测试要求（按模块）
- io_uring相关测试必须在Linux 5.1+内核上运行，epoll回退路径也必须测试
- C++20协程测试必须验证异步时序和挂起/恢复行为，不能只测同步返回值
- Manager-Worker多进程架构必须多进程联调测试，不允许单进程mock替代
- 共享内存IPC必须验证跨进程数据一致性，包括边界条件和并发读写
- HTTPS编解码器必须测试TLS握手、证书校验、异常断连，不能只测正常请求
- 插件动态加载必须测试加载+卸载+热更新，验证资源完全释放无泄漏
- 性能测试必须给出具体数据（QPS、延迟P99、内存占用），不允许说"性能OK"
- 内存安全必须用ASan/valgrind检测，线程竞争必须用TSan检测，展示完整报告（检查项见 [代码审查检查清单](#代码审查检查清单提交前) 第 1/2 节）

### 测试执行规则
- 必须真实运行，禁止mock/模拟/软件环回
- **测试目标环境优先 k8s，禁止未经声明自动切到 docker-compose**
- 每次测试前必须明确声明：测的是 k8s 还是 docker-compose，为什么选这个环境
- 如果因环境限制切了目标，必须在输出第一行标注 "⚠️ 测试环境已切换: k8s→docker-compose, 原因: ..."
- 实际跑通完整链路，展示运行输出
- 实际跑通完整链路，展示运行输出
- 跑不通就说明具体卡在哪，不要跳过
- 不允许说"应该能跑"或"理论上可以通过"，必须实际执行
- 测试结果必须贴完整输出，包括命令和响应
- 性能数据必须是实际网络I/O测出来的，不是内存拷贝速度
- 硬件/环境限制跑不了的，直接承认，标注"当前环境无法测试"及原因
- 每次测试完成后必须交代：执行了什么命令、完整输出是什么、测的是真实I/O还是模拟数据、如果是模拟哪部分是模拟的
- 撒谎比测不通更严重

### 测试完成标准
- 单元测试通过不算整体通过，端到端集成必须也通过
- 任何阻塞项（如依赖缺失、环境未就绪）必须明确标注，不允许跳过不报
- 部分通过=未通过，要么全通要么明确列出未通过项及原因
- 说"测试通过"必须展示证据：编译输出 + 运行结果 + 检测报告，不允许凭空断言

### 测试结果记录
- 测试结果写入 TEST_STATUS.md
- ✅ 已通过（真实环境）— 列出每项+数据
- ⚠️ 已通过（模拟环境）— 标注模拟方式，不展示模拟性能数据
- ❌ 当前环境无法测试 — 列出原因

## deploytest — Thunder 本地部署测试

当用户说"deploytest"或"本地部署测试"时：

**唯一入口：`./deploy.sh`**

### 第一步：构建 + 单元测试
```bash
./deploy.sh test unit    # C++ gtest (~250 cases) + Python pytest (64 cases)，全部通过才继续
```

### 第二步：E2E 集成测试
```bash
./deploy.sh test e2e     # Docker compose up → 等待服务就绪 → pytest E2E (25 cases) → docker compose down
```
等价于手动流程：`./deploy.sh build` → `./deploy.sh up` → 等待端口 → `./deploy.sh test e2e --skip-build` → `./deploy.sh down`

### E2E 覆盖范围

| 服务 | 测试内容 |
|------|---------|
| **Center (Raft)** | 节点选举、日志复制、服务注册/发现、集群状态查询、配置同步 |
| **HelloHttp** | GET/POST/PUT/DELETE 各方法、参数校验、错误处理 |
| **HelloHttps** | TLS 握手、证书校验、API 端点、异常断连 |
| **HelloWs** | WebSocket 连接→消息收发→断开、异常重连 |
| **Interface** | API 端点、参数校验、错误处理、插件加载→卸载 |
| **跨服务** | Manager-Worker 通信、心跳机制、全链路交互 |
| **性能** | QPS、延迟 P99、内存占用 (真实 I/O) |

### 测试后清理
```bash
./deploy.sh clean        # 一键清理 build/ + Docker + tmp
```

### 规则
- 单元测试通过不算整体通过，E2E 必须也通过
- 失败则分析日志、修复、重试，最多 3 次
- 部分通过 = 未通过，要么全通要么明确列出未通过项及原因
- 模拟测试通过 ≠ 测试通过，硬件限制的标注"当前环境无法测试"及原因
- git add + commit + push 所有改动
---

## Worker 优雅重启 — 回归测试

### 测试范围

改动涉及 `Worker.cpp`, `Manager.cpp`, `CW.hpp`, 影响:
- Worker 生命周期 (fork/exit)
- Manager 子进程管理 (OnChildTerminated)
- CMD 消息路由 (DisposeDataFromWorker)

### 单元测试 (本地, 零依赖)

```bash
# 排空逻辑测试 (7 cases)
cd build/code/test && ctest -R WorkerDrain --output-on-failure

# 覆盖:
#   WorkerDrain.IdleConnectionsClosedOnEnterDrain    — 空闲连接立即关闭
#   WorkerDrain.ActiveConnectionsNotClosed           — 在途连接保留
#   WorkerDrain.DrainCompleteWhenAllDone             — 全部完成=true
#   WorkerDrain.S2SConnectionsSkipped                — S2S连接不排空
#   WorkerDrain.NewConnectionsRejectedDuringDrain    — 排空拒绝新连接
#   WorkerDrain.AcceptNormalWhenNotDraining          — 正常模式正常accept
#   WorkerDrain.DrainTimeoutDetection                — 超时检测
```

### 集成测试 (需 Docker)

```bash
# 全链路 E2E (验证 Manager/Worker 未破坏)
./deploy.sh test e2e --skip-build

# 手动验证优雅重启流程:
# 1. 确认 Worker 正常运行
docker compose -p thunder-test exec hello ps aux | grep robot_W0

# 2. 发 SIGTERM 给 Worker (模拟排空)
docker compose -p thunder-test exec hello kill -TERM $(pgrep robot_W0)

# 3. 等新 Worker 启动 (Manager 会自动 RestartWorker)
sleep 5

# 4. 确认新 Worker 在运行且服务正常
docker compose -p thunder-test exec hello ps aux | grep robot_W0
curl -s http://127.0.0.1:27006/hello/hello -d '{"option":"Echo"}'
# 预期: {"code":0,"msg":"ok"}

# 5. 确认 Manager 日志无 FATAL 错误
docker compose -p thunder-test logs hello 2>&1 | grep -i "fatal\|error" | tail -5
# 预期: 无新错误 (只有旧的启动日志)
```

### 两层回归

```
单功能快速回归 (改了什么测什么):
  ctest -R WorkerDrain               ← 只跑排空测试, 5秒
  ctest -R CenterRaft                ← 只跑 Raft 测试
  python3 -m pytest tests/unit/test_token_verify.py  ← 只跑 token 测试

全功能回归 (改任何代码后必跑):
  ./deploy.sh build                  ← 编译
  ./deploy.sh test unit --skip-build ← 341 单元测试
  ./deploy.sh test e2e  --skip-build ← 26+ Docker E2E
  手动冒烟 7 项                      ← 核心链路 curl
```

**单功能回归 = 快速验证改动没写错。全功能回归 = 验证改动没破坏其他模块。** 两个都要过。

## 触发词：rearrange

当用户说 **rearrange** 时，执行以下流程：

### 适用场景
某个目录下有一堆内容重叠、未分类的 `.md` 文件，需要按功能重组。

### 核心原则
- **新文件 = 速查笔记风格**：精炼、结构化、方便面试前快速翻阅
- **有价值信息补回对应主题文件**：旧文件中的详细原理、完整示例、深入分析，不丢弃，直接补充到新文件对应章节中
- 宁可使单文件变大，也不丢失原理和例子

### 执行步骤

1. **读取所有文件**：读取目标目录下所有 `.md` 文件的内容（注意大文件分段读取）

2. **内容归类**：分析每份文件的主题和重叠点，设计功能分组方案

3. **去重合并 + 提取有价值信息**：
   - 同类内容合并，重复部分只保留最完整的一处
   - **同时将以下内容提取出来**，等新文件创建后补回：
     - 原理性长篇讲解（如"为什么这样设计"、"底层机制分析"）
     - 完整的可运行代码示例（非片段）
     - 对比分析（如 "A vs B 优缺点详解"）
     - 面试深挖中可能问到的扩展知识点
   - 新文件先只保留：核心结论 + 关键代码片段 + 对比表格 + 注意事项

4. **创建新文件**：
   - 创建 `00-总览.md` 作为索引总领文件（含文件地图、全景图、阅读路径）
   - 按功能创建 `01-*.md` 到 `N-*.md`，每份文件自成体系（核心原理 + 关键代码 + 注意事项）
   - 面试考点汇总到最后一篇

5. **将提取的有价值信息补回对应文件**：
   - 原理说明 → 补到对应主题文件的对应章节下
   - 完整示例 → 补到对应主题文件的代码示例区
   - 扩展知识点 → 补到对应文件的「深入理解」或「常见陷阱」章节
   - 确保新文件内容充实，不依赖外部文档

6. **旧文件清理**：确认新文件写完后，删除所有原始旧文件

7. **更新 CLAUDE.md 目录结构**：将新的目录结构反映到本文档的仓库目录结构中

### 文件命名规则

重组后的文件使用 `{序号}-{技术栈前缀}-{主题}.md` 格式：

```
02-go-并发编程.md    # go 技术栈
01-cpp-C++基础语法.md # cpp 技术栈
```

- **技术栈前缀**：当目录名称不能直接体现技术归属时（如 `go/` 目录下的文件在文件浏览器中可能脱离目录上下文），在序号后加技术栈前缀（如 `go`、`cpp`）
- **不需要前缀**：如果目录名本身就是技术名（如 `cpp/`），且文件在目录内引用无歧义，可省略前缀
- **一致性**：同一目录下所有文件保持统一的命名风格

### 要点列举必须带示例

列出多个技术要点时（如「六种逃逸场景」「五种实现方式」等），**每个要点必须附带独立代码示例**，不能用一行注释笼统带过。

❌ 反例（只有名词，无代码）：
```markdown
**六种逃逸场景**：返回指针、interface 调用、闭包、channel 发指针、大对象、切片扩容。
```

✅ 正例（逐条展开，每项有独立代码）：
```markdown
**六种逃逸场景**（含示例）：

```go
// 1. 返回指针
func escape1() *int {
    x := 42
    return &x  // x 逃逸到堆
}

// 2. interface 调用
func escape2() {
    x := 42
    fmt.Println(x)  // x 逃逸（fmt 参数为 interface{}）
}
// ... 其余逐条列出
```
```

**例外**：纯名词罗列（如文件列表、目录结构）不需要逐条代码。

### 禁止「其他」兜底分类

重构或增强文件时，**禁止**出现笼统的兜底章节（如 `### X.Y 其他重要特性` / `### X.Y 其他实用特性`），必须将杂项逐条拆解为**独立子节**（`#### X.Y.Z 具体名称`），每节包含：

| 要素 | 说明 |
|:----|:-----|
| **解决的问题** | 为什么需要这个特性/概念，解决了什么痛点 |
| **完整代码示例** | 含输入/输出/正反对比的可工作代码 |
| **性能/注意事项** | 零开销保证、常见陷阱、选型建议 |

❌ 反例（笼统堆砌）：
```markdown
### 1.5 其他重要特性（含示例）
```cpp
// nullptr —— 类型安全空指针
// enum class —— 强类型枚举
// constexpr —— 编译期计算
```
```

✅ 正例（逐条展开）：
```markdown
### 1.5 类型安全与枚举增强

#### 1.5.1 nullptr — 类型安全空指针

**解决的问题**：`NULL` 本质是整数 `0`，重载解析中会意外匹配 `int` 版本。

```cpp
void foo(int);  void foo(char*);
foo(NULL);      // 调用 foo(int) —— 危险！
foo(nullptr);   // 调用 foo(char*) —— 正确
```

**性能**：零开销抽象，运行时就是 `0`。

---
```

---

### 触发词：rearrange docs
- docs 目录编号前缀扁平化(如 `01-xxx.md`, `02-xxx.md`), 无子目录
- 参考模板: english-learner/docs/architecture/
### 触发词：logs / 日志
- 用 tests/logs.sh 查看, 支持 --logic/--interface/--etcd 指定节点
### 触发词：smoke / 冒烟
- tests/test_smoke.sh --hello/--interface/--etcd 分段测试
### 触发词：chaos / 混沌
- tests/chaos_etcd.sh 三个场景(停服/重启/灾难)

### 触发词：issus / 问题清单
- 所有 bug/优化/设计问题统一记录在 `issus-list.md`
- 发现新问题 → 先确认是真实问题 → 再记录 → 再修复
- 修复后标记 `✅ 已修复`,未修复标记 `🟡`
- 不要未经确认就改状态,不要删除已有条目
### 触发词：代码移动
- `git mv` 移动文件,同步修正所有 include 和 CMakeLists
- 全量构建 + 冒烟验证无回归
### 触发词：删代码
- 先确认零引用 → `grep -rn` 全局搜索 → 再删
- 测试文件如引用也一并清理

### 触发词：性能测试
- 没测就是没测, 别填假数据
- 对比测试要保证只有一个变量不同 (如 body 大小变化、其他条件一致)
- 每次改 backend 配置后等 5 秒让服务重启完成
- 结果直接写入对应文档, 别存脑子里

### 🚫 禁止回退
- **禁止 git reset/rebase 丢弃代码** — 除非用户明确要求
- **禁止 git checkout 覆盖修改** — 所有文件变动必须经过确认
- **禁止 rebase skip** — 冲突时合并解决, 不跳过有效提交
- **禁止 revert file moves/refactors** — 原因: 上次 rebase skip 导致 io/ register/ 目录丢失

### 提交规范 (Commit Rules)
- **只能 git merge，禁止 git rebase** — rebase 会改写历史, 丢弃本地提交
- **有冲突必须手动解决** — 不允许 --skip / --abort / --force
- **解决冲突后立即验证** — 全量编译 + 冒烟测试
- **每步提交前确认工作树干净** — git status 检查无遗漏


### testnewfunc 触发词 (Thunder)

当用户说"testnewfunc"时，执行以下流程：

**1. 定位改动范围**
```bash
git diff HEAD --stat          # 未提交更改
git log --oneline -3           # 最近提交
```
确定影响范围：code/Net | code/Hello* | deploy/admin-web | k8s | build

**2. 全量构建**
```bash
./deploy.sh build              # cmake + make + install, 必须 0 error 0 warning
```

**3. C++ 单元测试**
```bash
ctest -j4 --output-on-failure  # 从 build/code/test 目录运行
```
- 328 项必须 100% 通过
- 失败项逐一排查，不允许跳过

**4. Python 单元测试**
```bash
cd tests && python -m pytest pytest/ -v
```

**5. k8s 部署**（涉及 k8s 配置或部署文件时）
```bash
kubectl apply -f k8s/
kubectl -n thunder rollout restart deployment thunder-admin-web
```

**6. Admin 功能测试**（涉及 Admin 页面改动时）
- 页面可访问: `curl http://127.0.0.1:30090/index.html` → HTTP 200
- SO 镜像列表: `curl http://127.0.0.1:30090/api/so-images` → 返回 JSON
- SO 文件列表: `curl http://IP:8090/api/so-files?image=xxx` → 返回 .so 列表
- SO 提取: `curl -X POST http://IP:8090/api/so-extract ...` → 本地+NFS 双写验证
- 页面功能: grep 检查 selectSoImage / extractAndRefresh / triggerUpdate 等函数存在
- **必须真实请求，禁止 mock**

**7. SO 镜像构建**（涉及 so-images 或 deploy.sh 改动时）
```bash
./deploy.sh build-so all        # 全量构建
./deploy.sh build-so HelloHttp_ModuleHello  # 单独构建
```
- 首次构建 → 全量通过
- 二次构建 → 全部跳过(无变化)

**8. 回归测试（影响范围内的旧功能）**
- 分析改动影响范围，列出受影响的旧功能
- 跑受影响的相关测试
- 不跑全量回归（除非用户明确要求）

**9. 端到端测试（新增/修改的功能）**
- 针对本次改动的功能点，明确列出测试场景
- 实际跑通完整链路，展示运行输出
- 跑不通就说明具体卡在哪，不要跳过

**测试输出要求**:
- 每个测试项必须展示：命令 + 完整输出 + 结果
- 通过 ✅ / 失败 ❌ / 跳过 ⏭ 必须明确标注
- 部分通过 = 未通过，必须列出原因
- 构建失败、ctest 失败 = 阻塞，先修复再继续

**禁止的测试方式**:
- ❌ 只跑 ctest 就说"测试通过"
- ❌ curl 健康检查就说"功能正常"
- ❌ 改完代码不跑测试就提交
- ❌ 说"已验证"但不展示完整输出
- ❌ 部分通过就说"测试通过"
