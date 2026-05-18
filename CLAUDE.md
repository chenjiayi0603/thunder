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
docs/                # 架构设计文档
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

```bash
# 启停
( cd deploy && ./nodes.sh restart all )
( cd deploy && ./nodes.sh status )

# 全部测试 (单元 + E2E)
./tests/run_all.sh

# 仅单元测试 (14s, 零外部依赖)
./tests/run_all.sh unit

# 仅 E2E 集成测试 (需 Docker)
./tests/run_all.sh e2e
MODE=external ./tests/run_all.sh e2e

# 构建 + 全部测试
./tests/run_all.sh build+test

# 仅构建（不跑测试）
./tests/run_all.sh build

# 清理构建产物
./tests/run_all.sh clean

# 性能基准测试 (需 wrk)
./tests/run_all.sh bench
```

### 改完代码后必须验证

- [ ] `cmake --build build -j1` — 编译通过
- [ ] `cmake --install build` — 安装到 deploy/
- [ ] (如改 Proto) `cmake --build build --target thunder_proto_gen -j1`

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

### 5. 每次代码改动后必须执行

#### 改 C++ 代码后

- [ ] `cmake --build build -j1` — 编译检查 (必须通过)
- [ ] `cmake --install build` — 安装到 deploy/
- [ ] `( cd deploy && ./nodes.sh restart all )` — 重启节点
- [ ] `./tests/run_all.sh unit` — 单元测试 (14s)
- [ ] `./tests/run_all.sh e2e` — E2E 测试 (需 Docker)

#### 改 Proto 后

- [ ] `cmake --build build --target thunder_proto_gen -j1` — 重新生成 .pb.cc/.pb.h
- [ ] `cmake --build build -j1` — 全量编译
- [ ] 联调冒烟测试

#### 改部署脚本后

- [ ] `( cd deploy && ./nodes.sh restart all )` — 启停正常
- [ ] `( cd deploy && ./nodes.sh status )` — 所有节点状态正常

#### 禁止

- 改完代码不跑编译就提交
- 只验证改动功能，不验证相关功能
- 修改接口后不同步更新 Proto 和所有节点

#### 测试后必须清理

每次测试（E2E、冒烟、压测、构建部署）完成后，必须清理：

```bash
# Docker Compose 容器与网络（停止并移除，避免端口占用和网络残留）
( cd docker && docker compose down 2>/dev/null )

# Docker 构建残留（每次 docker compose build 累计数百 MB overlay 层）
docker system prune -f

# io_uring 诊断日志（busy loop 时可增长到几十 GB）
rm -f /tmp/asio_uring_diag.log

# E2E 临时文件（若存在）
rm -rf /tmp/e2e-* /tmp/stress-* 2>/dev/null
```

**原因**：Docker overlayfs 每次构建不自动清理，多次 `docker compose build` 可占满磁盘且 overlay mount 导致 `df` 虚报 100%。`asio_uring_diag.log` 在 ring_fd busy loop 场景下单次可达数十 GB。

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
