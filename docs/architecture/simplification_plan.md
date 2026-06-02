# Thunder 架构简化方案

> 日期: 2026-06-01 | 基于当前代码分析

---

## 一、当前架构复杂度诊断

### 核心代码规模

| 模块 | 文件 | 行数 | 复杂度 |
|------|------|------|--------|
| Manager.cpp | 1 | 2,732 | 太高 — 多进程管理 |
| Worker.cpp | 1 | 6,020 | 极高 — IPC+事件+协议 |
| IoBackend 实现 | 4 种 | 2,000+ | 冗余 — 只需一种 |
| 配置文件 | 18 个 JSON | 607 | 可合并 |
| 部署脚本 | 7 个 node.sh | 705 | 大量重复 |
| 第三方库 | 14 个 | — | 依赖过多 |

### TOP 5 复杂度来源

1. **Manager-Worker 多进程模型 (40%)** — fork+socketpair+shm+SIGCHLD, ~8000行
2. **四种 IoBackend 并行维护 (20%)** — ev/uring/asio_uring/dpdk, 11个虚函数
3. **每个服务独立配置+脚本 (15%)** — 6服务x3脚本=18文件
4. **编解码器类型爆炸 (10%)** — 9种CODEC各自独立实现
5. **14个第三方库 (10%)** — 编译慢、版本碎片、CVE面大
6. **Plugin 热加载 (5%)** — dlopen, 每个.so独立编译

---

## 二、简化方案 (按收益排序)

### P0: 去掉多进程 → 单进程多线程 (收益 ~40% 代码)

**现状**: Manager fork Worker, socketpair IPC, shm 路由同步

**问题**:
- Manager.cpp 2732行 (进程生命周期、SIGCHLD、路由shm、配置shm)
- Worker.cpp 6020行 (socketpair IPC、路由增量更新、事件循环)
- 每个服务 2 进程，调试需 attach 两次

**方案**: Worker 内联到 Manager，用线程池替代多进程

**收益**:
- 消除 Manager.cpp (2732行)
- Worker.cpp 缩减至 ~1500行 (去IPC/shm部分)
- 去掉 socketpair、shm_open、fork、SIGCHLD
- 调试: 2进程 → 1进程
- 部署: \`./node.sh start\` → \`./app\`

**风险**: 进程隔离消失，一个 Worker 崩溃影响全局
**缓解**: std::jthread + try/catch, 或 systemd Restart=always

---

### P1: IoBackend 精简 → 只保留一种 (收益 ~20%)

**现状**: EvIoBackend / AsioUringIoBackend / NativeUringIoBackend / DpdkIoBackend

**方案**: 只保留 AsioUringIoBackend

**理由**:
- io_uring 是 Linux 未来方向
- asio_uring 高并发延迟碾压 ev (P99 4.58ms vs 10.34ms)
- asio_uring 零锁零线程切换，架构更简洁
- Kernel 5.1+ 普及率已足够

**收益**:
- 消除 IoBackend 抽象层 (11 虚函数)
- 消除 3 种实现 (~1500行)
- 消除 io_backend 配置项
- 消除 per-backend 配置文件 (Hello_ev.json 等)

---

### P2: 配置文件合并 (收益 ~10%)

**现状**: 一个服务 4-5 个 JSON，每种 io_backend 一份

**方案**: 一个服务一个 YAML，扁平配置

**收益**: 18 JSON → 6 YAML，消除 Cmd 配置文件

---

### P3: 脚本统一 → 一个启动器 (收益 ~5%)

**现状**: 7 个 node.sh (705行) + 6 个 script_func.sh

**方案**: 单个 thunderd 二进制，读 YAML 配置启动

**收益**: 705行 shell → ~100行二进制

---

### P4: 砍掉不必要的第三方库 (收益 ~5%)

| 库 | 替代 |
|----|------|
| libev | asio 内置 (io_uring) |
| curl | asio http client |
| c-ares | asio resolver |
| cryptopp | OpenSSL (已链接) |
| log4cplus | spdlog (header-only) |

**收益**: 14库 → 9库, 编译 -30%, CVE面 -35%

---

### P5: 插件静态链接 (收益 ~3%)

**现状**: dlopen .so 动态加载

**方案**: 业务模块直接编译进可执行文件

**收益**: 消除 dlopen, 编译期类型检查, 启动更快

---

## 三、简化路线图

| Phase | 时间 | 行动 | 删除代码 |
|-------|------|------|---------|
| P0 | 1-2周 | 多进程→单进程 | ~4000行 |
| P1 | 1周 | IoBackend 精简 | ~1500行 |
| P2+P3 | 3天 | 配置+脚本统一 | ~1000行 |
| P4 | 1周 | 砍第三方库 | 依赖-5 |
| P5 | 1周 | 插件静态化 | ~200行 |

---

## 四、简化前后对比

| 指标 | 前 | 后 | 变化 |
|------|-----|-----|------|
| 核心代码行数 | ~12,000 | ~5,000 | **-58%** |
| IoBackend 实现 | 4 种 | 1 种 | **-75%** |
| 配置文件 | 18 JSON | 6 YAML | **-67%** |
| 脚本文件 | 13 .sh | 1 binary | **-92%** |
| 第三方库 | 14 | 9 | **-36%** |
| 进程/服务 | 2 | 1 | **-50%** |
| 编译时间 | ~5min | ~2min | **-60%** |
| gdb attach | 2次 | 1次 | **-50%** |

---

## 五、不简化项 (理由)

| 保留 | 理由 |
|------|------|
| Raft 3 节点集群 | 集群能力核心 |
| Protobuf 序列化 | 二进制比 JSON 快 10x |
| C++20 协程 StepCo20 | 异步编程核心抽象 |
| FastPath HTTP 解析 | 性能核心 (30项测试) |
| 共享内存路由 | 单进程后简化为内存指针 |
