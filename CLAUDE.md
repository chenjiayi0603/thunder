# Thunder — Claude Code 项目配置

## 项目概述
Thunder 是一个基于 C++20 的分布式异步集群服务框架，提供 Center 注册发现、Worker 并发处理、HTTP/HTTPS/WebSocket 与内部二进制协议接入、可插拔模块（`.so`）等能力。

## 技术栈
- **语言**：C++20（CMake >= 3.20 构建）
- **构建**：CMake + Ninja/Make，`RelWithDebInfo` 默认
- **事件循环**：libev
- **序列化**：Protobuf（`code/3party/protobuf/build/protoc`，`coor.proto`）
- **协程**：C++20 协程 Step 体系（`StepCo20`）与 Awaitable
- **数据库**：MariaDB（mariadb-connector-c）+ MongoDB（mongo-c-driver）+ Redis（hiredis-vip）
- **网络**：curl + c-ares + OpenSSL
- **日志**：log4cplus
- **加密**：cryptopp
- **内存**：jemalloc（可选）
- **集群**：Raft 选主 + 共享内存路由镜像
- **测试**：pytest（联调/冒烟/性能）+ wrk 压测
- **部署**：Docker + shell 脚本（`nodes.sh`）
- **入口文档**：`INSTALL.md`（快速开始）、`README.md`

## 目录结构
```
code/
  Net/               # 网络框架核心（事件循环、协程、编解码、会话管理、Step 状态机）
    include/         # 头文件（cmd/codec/coro/labor/protocol/session/step/storage）
    src/             # 实现（main.cpp、Manager.cpp、Worker.cpp、dispatcher/ 等）
  Center/            # 中心节点（Raft 选主、注册、上报、Admin 管理页）
    src/
      CmdRaftRequestVote/      # Raft 投票
      CmdRaftAppendEntries/    # Raft 日志复制
      CmdNodeRegister/         # 节点注册
      CmdNodeReport/           # 节点上报
      CmdNodeDisconnect/       # 节点断连
      ModuleAdmin/             # Web 管理后台
  Logic/             # 逻辑节点（CmdGetToken 等业务插件）
  Interface/         # 接口节点（HTTP 入口，ModuleHello 等）
  Hello/             # Hello 示例模块（CmdHello、ModuleHello、ModuleShake）
  Proto/             # Protobuf 协议定义（coor.proto）
  Util/              # 工具库（日志、DBI/ORM、线程、curl、算法、Unix 工具）
  3party/            # 第三方子模块（libev/cryptopp/curl/hiredis-vip/log4cplus/
                    #   mariadb-connector-c/mongo-c-driver/protobuf/c-ares）
  test/              # 单元测试（center/coroutine/interface/orm/step）
deploy/              # 安装产物、节点配置、启停脚本（nodes.sh）、测试脚本
  Center/            # Center 部署配置
  HelloHttp/         # HTTP 示例节点
  HelloWs/           # WebSocket 示例节点
  HelloHttps/        # HTTPS 示例节点
  Interface/         # Interface 节点
  Logic/             # Logic 节点
  tests/             # pytest 测试（integration/smoke/perf）
docs/                # 架构设计文档
cmake/               # CMake 模块与构建说明
```

## ⚠️ 构建限制（强制）
- **所有 cmake --build 命令必须使用 `-j1`**，严禁使用 `-j$(nproc)` 或多线程编译
- 原因：本机磁盘 IO 是瓶颈，多线程编译会导致系统卡死
- 违例：`cmake --build build`、`cmake --build build -j4`、`cmake --build build --parallel` 等均禁止

## 开发规范
- **语言**：用中文回复和注释
- **C++**：遵循 `.clang-format`（项目根目录），C++20 标准
- **提交**：中文提交信息，format: `feat(module): description` 或 `fix(module): description`
- **新功能**：优先修改已有文件，不随意新建
- **注释**：只在 WHY 不明显时写，不写 WHAT
- **构建验证**：改完代码必须 `cmake --build build -j1` 通过
- **Proto 变更**：改了 `coor.proto` 后执行 `cmake --build build --target thunder_proto_gen -j1`

## 关键架构决策
- **IoBackend 抽象**：支持三档运行时切换 — `ev`（epoll）/ `uring`（liburing 手写）/ `asio_uring`（standalone Asio io_uring，主线程直驱）
- 基于事件驱动的异步网络模型（libev），支持高并发连接处理
- 多进程 Worker 架构（Manager/Loader/Worker），插件动态加载（`Cmd*.so`、`Module*.so`）
- Center 集群 Raft 选主，主从语义下的注册/上报流程
- 路由镜像通过共享内存（shm）同步：Manager 写 shm → Worker 定时检查版本号 → 增量更新
- 配置同步：主 Center → 业务节点 Loader → 共享内存 → Worker 定时感知
- 共享内存布局：blob → len → version++（原子递增，避免半包读取）
- 路由 shm 160KB / 配置 shm 160KB，超限不写入、打错误日志、Worker 继续用旧版本
- HTTP 编解码支持 HTTP/HTTPS/WebSocket 多种协议

## 重要决策
- 第三方库版本以 `.gitmodules` gitlink 为准，勿随意升级
- Protobuf 只用 `code/3party/protobuf/build` 里的 protoc/libprotobuf，勿与系统旧版混用
- 构建默认 `-j1` 减轻磁盘 IO 压力（本机 IO 足够可改 `-j$(nproc)`）
- 默认安装前缀为 `deploy/`
- 联调测试优先用 pytest（`deploy/tests/pytest`），覆盖 HTTP/HTTPS/WS/Interface/Raft 关键链路

## 常见操作

### 构建
```bash
# 一键构建安装
git submodule update --init --recursive \
  && cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  && cmake --build build --target thirdparty_deploy -j1 \
  && cmake --build build -j1 \
  && cmake --install build

# 仅重编（第三方已部署）
cmake --build build -j1 && cmake --install build

# 仅重编某模块
cmake --build build --target Net -j1
cmake --build build --target InterfacePlugins -j1
```

### 部署与测试
```bash
# 启停
( cd deploy && ./nodes.sh restart all )
( cd deploy && ./nodes.sh status )

# 联调/冒烟
python3 -m pytest deploy/tests/pytest -m "integration or smoke" --mode=local

# 性能
WRK_THREADS=4 WRK_CONNECTIONS=100 WRK_DURATION=10s \
  python3 -m pytest deploy/tests/pytest -m perf --mode=local -s

# Center 管理页
# http://172.24.177.85:26000/admin
```

### 改完代码后必须验证
```bash
# 1. 编译检查
cmake --build build -j1

# 2. 安装
cmake --install build

# 3. （如果改了 Proto）
cmake --build build --target thunder_proto_gen -j1
```

---

## Skill routing

When the user's request matches an available skill, ALWAYS invoke it using the Skill
tool as your FIRST action. Do NOT answer directly, do NOT use other tools first.
The skill has specialized workflows that produce better results than ad-hoc answers.

### Key routing rules

**代码质量检查（Code Quality）— 自动触发 `/health`：**
- 用户说 "检查"、"检查一下"、"检查代码"、"代码质量"、"质量检查"、"health check"、"code quality"、"跑一下检查"、"全面检查"、"帮我检查" → invoke **/health**
- `/health` 会：运行类型检查（clang-tidy/clangd）、lint（clang-format）、测试（pytest）、死代码检测、shell lint，计算 0-10 综合评分并追踪趋势

**代码审查（Code Review）— 自动触发 `/review`：**
- 用户说 "审查"、"review"、"code review"、"看看代码"、"看下 diff"、"检查改动"、"审查代码" → invoke **/review**
- `/review` 会：分析 diff，调度 Performance 专项审查员（后端/前端代码自动触发性能审查）、Security 审查员、对抗性审查（Adversarial Review 主动寻找资源泄漏和性能隐患）、SQL 安全、竞态条件检测等

**Bug 排查 — 自动触发 `/investigate`：**
- 用户说 "为什么坏了"、"bug"、"报错"、"error"、"不工作"、"出问题了"、"排查"、"怎么修" → invoke **/investigate**

**QA 测试 — 自动触发 `/qa`：**
- 用户说 "测试"、"test"、"QA"、"跑测试"、"验证一下"、"find bugs" → invoke **/qa**
- `/qa` 会：浏览器自动化测试 Center 管理页，发现问题后自动修复并提交，产生健康评分
- 只要报告不要修复 → invoke **/qa-only**

**开发者体验审计 — 自动触发 `/devex-review`：**
- 用户说 "DX"、"开发体验"、"文档质量"、"上手体验" → invoke **/devex-review**

**保存进度 — 自动触发 `/checkpoint`：**
- 会话结束、切换上下文、长时间暂停前 → invoke **/checkpoint**

### 检查命令速查表

| 命令/触发词 | 技能 | 做什么 |
|---|---|---|
| "检查"、"代码质量"、"全面检查" | `/health` | 代码质量仪表盘：类型检查 + lint + 测试 + 死代码 + shell lint，0-10 评分 |
| "review"、"审查"、"看下代码" | `/review` | PR 前置审查：SQL 安全、竞态、性能、对抗性审查（资源泄漏/性能隐患） |
| "bug"、"报错"、"不工作" | `/investigate` | Bug 调查，排查错误和异常 |
| "测试"、"QA"、"验证" | `/qa` | 浏览器自动化测试 + 自动修复 |
| "DX"、"开发体验" | `/devex-review` | 开发者体验审计 |
| "保存"、"checkpoint" | `/checkpoint` | 保存当前工作状态 |

---

## Agent 行为准则

### 1. 先思考再编码（Think Before Coding）
- 不确定时必须停下来问，不能猜，不能假设
- 存在多种理解时列出选项让用户选，不要替用户做决定
- 发现更简单的方案时主动说出来，不要默默选最复杂的路
- 把 trade-off 摆出来，不要隐藏困惑

### 2. 简洁优先（Simplicity First）
- 50 行能写完绝不写 200 行
- 没人要求的"灵活性"和"可配置"不加
- 不可能发生的异常场景不做错误处理
- 不为未来可能的需求提前写代码

### 3. 精准修改（Surgical Changes）
- 只动被要求动的部分，不顺手优化相邻代码
- 匹配项目已有的代码风格（`.clang-format`），哪怕觉得自己写得更好
- 看到不相关的问题提一嘴就行，别动手改
- 每一行改动都能追溯到用户的原始请求

### 4. 目标驱动执行（Goal-Driven Execution）
- "修 Bug" → 先写能复现 Bug 的测试，再让测试通过
- "加校验" → 先写非法输入测试，再让它通过
- "重构 X" → 确保改前改后测试都通过
- 复杂任务先列分步计划，每步带验证方式

### 5. 每次改动后必须执行的测试

#### 改 C++ 代码后：
1. `cmake --build build -j1` — 编译检查（必须通过）
2. `cmake --install build` — 安装到 deploy/
3. `( cd deploy && ./nodes.sh restart all )` — 重启节点
4. `python3 -m pytest deploy/tests/pytest -m "integration or smoke" --mode=local` — 联调冒烟测试

#### 改 Proto 后：
1. `cmake --build build --target thunder_proto_gen -j1` — 重新生成 .pb.cc/.pb.h
2. `cmake --build build -j1` — 全量编译
3. 联调冒烟测试

#### 改部署脚本后：
1. `( cd deploy && ./nodes.sh restart all )` — 启停正常
2. `( cd deploy && ./nodes.sh status )` — 所有节点状态正常

#### 禁止：
- 改完代码不跑编译就提交
- 只验证改动功能，不验证相关功能
- 修改接口后不同步更新 Proto 和所有节点

## 当前功能状态

### 网络 I/O
- ✅ libev epoll 后端（`EvIoBackend`）— 默认后端，GET c100 167k RPS
- ✅ liburing 手写后端（`UringIoBackend`）— ev_io(ring_fd) 驱动，待 asio_uring 验证后移除
- ✅ standalone Asio io_uring 后端（`AsioUringIoBackend`）— 主线程直驱，大包优于 ev
- ✅ IoBackend 抽象接口 — 运行时按 `"io_backend"` 配置三档切换
- ✅ S2S 跨节点 TCP 接入 IoBackend — accept 后的连接走异步 I/O
- ✅ 协程 StepCo20 与 IoBackend 解耦 — 通过 `IoCompletionCallback` 回调衔接

### AsioUringIoBackend 并发模型
- ✅ 主线程直驱（当前）— io_context 跑在 libev 主线程，零锁零线程跳
- ✅ ev_prepare + ev_check + ev_io(ring_fd) 三路驱动 io_context.poll()
- ✅ 独立线程+ev_async 桥接（历史）— 第一版实现，跨线程开销已消除

### 集群与路由
- ✅ Center 集群 Raft 选主
- ✅ 节点注册/上报/断连检测
- ✅ 共享内存路由镜像（Manager 写 → Worker 增量感知）
- ✅ 配置通过 shm 同步到 Worker

### 协议与编解码
- ✅ HTTP/1.1 编解码
- ✅ HTTPS（OpenSSL）
- ✅ WebSocket（JSON + Protobuf）
- ✅ 内部二进制协议（ProtoCodec / ThunderCodec）
- ✅ 自定义协议（CODEC_PRIVATE / CODEC_APP）

### 模块与插件
- ✅ 插件动态加载（`Cmd*.so` / `Module*.so`）
- ✅ Hello 示例模块（HTTP Echo）
- ✅ Admin 管理后台（Center Web 管理页）

### 测试与压测
- ✅ pytest 集成/冒烟测试（`deploy/tests/pytest`）
- ✅ wrk 三档横向压测（`deploy/tests/benchmark/run_bench.sh`）
- ✅ GET / POST 小包/大包对比压测
- ✅ Benchmark 结果文档化（`results/final_summary.csv` + `asio_uring_benchmark.md`）

### 存储
- ✅ MariaDB（mariadb-connector-c）
- ✅ MongoDB（mongo-c-driver）
- ✅ Redis（hiredis-vip）
- ✅ Protobuf 序列化（`coor.proto`）

### 待办
- ⚠️ 移除手写 UringIoBackend（Step 9 独立 PR）— 待 asio_uring 在 native Linux 验证后执行
- ⚠️ native Linux 性能验证 — 当前所有 benchmark 在 WSL2 上运行，性能数据有噪声
- ✅ asio_uring 大包测试已覆盖 4KB + 64KB，三档横向对比完成。64KB 场景 io_uring 延迟碾压 epoll（c100: -86%, 2.32ms vs 16.78ms），Stdev 为 ev 的 1/50

---

## 联系方式
- Gitee：chenjiayi/thunder
