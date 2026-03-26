# Thunder

`Thunder` 是一个基于 C++20 的分布式异步集群服务框架，提供 Center 注册发现、Worker 并发处理、HTTP 与内部二进制协议接入、可插拔模块（`.so`）等能力。

项目面向“多节点、可扩展、可脚本化联调”的服务端场景，仓库内同时提供构建脚本、部署脚本和联调/压测脚本。

## 主要特性

- 基于事件驱动的异步网络模型，支持高并发连接处理。
- 多进程 Worker 架构，支持插件动态加载（`Cmd*.so`、`Module*.so`）。
- 支持 HTTP 编解码、内部二进制协议与多种编解码器扩展。
- Center 集群支持 Raft 选主与主从语义下的注册/上报流程。
- 内置 C++20 协程 Step 体系（`StepCo20`）与 Awaitable 能力。
- 提供部署脚本、联调脚本、压测脚本和 Center 管理 CLI。

## 目录概览

```text
.
├── code/                 # 核心源码（Net/Center/Logic/Interface/Hello/Proto/3party）
├── deploy/               # 安装产物、节点配置、启停脚本、测试脚本
├── docs/                 # 架构设计与专题文档
├── cmake/                # CMake 选项与构建说明
├── INSTALL.md            # 构建/安装主文档
└── README.md
```

## 能力概览
| 方向 | 说明 |
|------|------|
| **路由** | 中心节点支持注册、发现与路由 |
| **RPC / 异步** | 状态机、协程、远程过程调用（匿名函数等） |
| **IO** | 可自定义信号处理等 |
| **编解码** | 多种编解码器，可扩展 |

## 快速开始

首次建议按根目录 `INSTALL.md` 的“一键”流程执行。

### 1) 拉取子模块并构建安装

在仓库根目录执行：

```bash
git submodule update --init --recursive \
  && cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  && cmake --build build --target thirdparty_deploy -j1 \
  && cmake --build build -j1 \
  && cmake --install build
```

默认安装前缀为 `deploy/`。更多构建选项见 `cmake/BUILD.md`。

### 2) 启动节点

在 `deploy/` 目录使用统一脚本：

```bash
./nodes.sh restart all
```

常用命令：

```bash
./nodes.sh start all
./nodes.sh stop all
./nodes.sh restart Hello
./nodes.sh status
```

## 一键联调与冒烟

### Hello 节点 HTTP 用例

在 `deploy/` 下：

```bash
./tests/test_helloserver.sh
```

该脚本会启动 Hello 并验证 `POST /hello/hello` 的典型 JSON 用例（如 `Echo`、`TestHelloPoolCpu`、`TestHelloPoolBlock`）。

### Interface 链路联调（Center -> Logic -> Interface）

在 `deploy/` 下：

```bash
./tests/test_interfaceserver.sh
```

默认流程包含：

- 启动 3 个 Center（Raft）
- 启动 Logic 并等待注册
- 启动 Interface
- 执行 `GenKey -> VerifyKey` HTTP 冒烟

## 压测示例（wrk）

仓库提供 `wrk` 脚本压测 Hello 接入：

```bash
cd deploy
./tests/test_helloserver_wrk.sh
```

可覆盖参数示例：

```bash
HELLO_HOST=127.0.0.1 HELLO_PORT=27006 HELLO_PATH=/hello/hello \
  WRK_THREADS=4 WRK_CONNECTIONS=100 WRK_DURATION=10s \
  ./tests/test_helloserver_wrk.sh
```

示例结果见 `deploy/tests/wrk_test_result.md`（当前样例约 `54k req/s`）。

## 配置说明

- 节点配置示例：`deploy/Hello/conf/HelloTemplate.json`
- 网络/节点基础入口：`code/Net/src/main.cpp`、`code/Net/src/labor/Manager.cpp`、`code/Net/src/labor/Worker.cpp`
- 默认部署和启停脚本：`deploy/nodes.sh`

## 文档导航

- 构建与安装：`INSTALL.md`
- 部署与脚本：`deploy/deploy.md`
- Center CLI 使用：`deploy/centercli/README_cn.md`

## 依赖与环境

- CMake >= 3.20
- C++20 编译器
- OpenSSL 开发包
- 子模块第三方依赖（见 `.gitmodules` 与 `code/3party/readme.md`）

## 许可证

本项目遵循仓库内 `LICENSE`。
