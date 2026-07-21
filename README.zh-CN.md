# Thunder ⚡

**高性能 C++20 网关框架。异步为核，运行时可扩展。**

[![License](https://img.shields.io/badge/license-AGPL--3.0%20%2B%20Commercial-blue)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-%2300599C?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/20)
[![Platform](https://img.shields.io/badge/platform-Linux%20x86__64-orange)](https://kernel.org)

```
HTTP / HTTPS / WebSocket → picohttpparser + io_uring → Protobuf RPC → 后端逻辑

  单核 235k RPS · P50 延迟 220 μs · work-stealing 543 ns/op（旧队列 1374 ns）
```

---

## ⚡ 快速开始

```bash
# 构建
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target thirdparty_deploy -j1
cmake --build build -j1 && cmake --install build

# 运行（Docker Compose）
./deploy.sh up
curl http://127.0.0.1:27006/hello/hello -d '{"option":"Echo","data":"hi"}'
# → {"code":0,"msg":"ok","data":"hi"}

# 测试
./deploy.sh test unit       # C++ gtest + Python pytest（~45s）
./deploy.sh test e2e        # 全量集成测试（~3 min）
```

> 📖 **刚接触 Thunder？** 从 [`QUICKSTART.md`](QUICKSTART.md) 开始 — 涵盖灰度部署、Lua & SO 热加载、K8s 环境搭建。

---

## 🎯 为什么选择 Thunder？

Thunder 专为**延迟敏感、不能停机**的场景设计 — API 网关、游戏后端、实时代理。

| 设计选择 | 取舍之道 |
|:---|:---|
| **每 Worker 单线程事件循环** | 无锁、无竞态。多核通过多进程实现。 |
| **C++20 协程（`co_await`）** | 异步代码像同步一样写。单线程支持 100 万+ 并发协程。 |
| **io_uring 批量提交** | N 次 I/O 操作 → 1 次 `io_uring_enter` 系统调用。TLS 延迟从 803μs 降至 402μs。 |
| **K8s HostNetwork** | 零 kube-proxy 跳转。客户端直达 Worker 进程。 |
| **优雅排空 + dlopen 热切换** | 更新 `.so` 插件不丢一个连接。 |
| **etcd 原生服务网格** | 实时配置推送、灰度路由、版本化回滚 — 不需要 ConfigMap 重启。 |

> 🧠 **深入阅读**：性能数据 → [`docs/performance/`](docs/performance/) · 架构设计 → [`docs/architecture/01-architecture-design.md`](docs/architecture/01-architecture-design.md) · 常见问题 → [`docs/README.md#核心设计问答`](docs/README.md#核心设计问答)

---

## 🚀 功能

| 分类 | 能力 |
|:---|:---|
| **协议** | HTTP/1.1 · HTTPS (TLS 1.3) · WebSocket · WSS · 内部 Protobuf RPC |
| **I/O 后端** | `ev` (epoll) · `asio_uring` · `native_uring` · DPDK（规划中） |
| **解析器** | picohttpparser（SSE4.2 SIMD，比 http_parser +49% RPS）· yyjson arena 分配器 |
| **协程** | `co_await` Redis · `co_await` MySQL · `co_await` 跨节点 RPC |
| **线程池** | Work-stealing（Go LRQ 风格）· 无锁 MPMC 队列 · 动态扩缩 |
| **脚本** | 每 Worker 独立 LuaJIT VM · Lua 热加载（不重启进程） |
| **插件** | `.so` 动态加载 · etcd 触发优雅切换 · NFS 共享制品 |
| **服务网格** | etcd 注册中心 · lease 心跳 · CAS 槽位分配 |
| **灰度** | 加权路由（`v1=70% v2=30%`）· 一行回滚 · 按节点类型控制 |
| **管理后台** | Web 控制台 → 插件管理 · 节点拓扑 · etcd 浏览器 |
| **部署** | Docker Compose · 裸金属 · Kubernetes（hostNetwork + StatefulSet etcd） |

---

## 📊 性能

*单核绑核，`wrk -t4 -c100 -d10s`，i9-12900H。完整报告 → [`docs/performance/`](docs/performance/)*

### HTTP — Thunder vs Nginx

| 载荷 | Thunder ev | Thunder asio_uring | Nginx 1w | 提升 |
|----:|:---:|:---:|:---:|:---:|
| 1 KB | 229k RPS | **232k RPS** | 191k RPS | **+21%** |
| 4 KB | 216k RPS | **223k RPS** | 184k RPS | **+21%** |
| 64 B | 232k RPS | **235k RPS** | 214k RPS | **+10%** |

| 载荷 | Thunder asio_uring | Nginx |
|----:|:---:|:---:|
| 64 B | **220 μs** | 466 μs |
| 4 KB | **332 μs** | 543 μs |

### HTTPS — TLS + io_uring 批量提交

| 载荷 | Thunder uring | Nginx SSL |
|----:|:---:|:---:|
| 64 B | **402 μs** | 752 μs |
| 4 KB | **247 μs** | 824 μs |

### WebSocket Echo（长连接）

| 载荷 | 连接数 | RPS | P50 | P99 |
|----:|:---:|:---:|:---:|:---:|
| 64 B | 10 | 46k | 192 μs | 549 μs |
| 1 KB | 10 | 15k | 564 μs | 1.7 ms |

---

## 🧬 架构

```
 Manager（fork/重启 Worker，监听 etcd 配置变更）
   ├── Worker 0 ──── io_uring / epoll 事件循环
   │    ├── HTTP Fast Path（picohttpparser + 前缀匹配 → 零拷贝）
   │    ├── .so 模块（动态 dlopen，热切换）
   │    ├── Lua VM（LuaJIT，每 Worker 独立，脚本热加载）
   │    └── 协程（co_await MySQL / Redis / 跨节点 RPC）
   ├── Worker N ──── ...
   └── Work-stealing 线程池（卸载 CPU 密集型任务）

 etcd 集群（3 节点）
   ├── 服务注册 + lease 心跳
   ├── 配置存储（推送到 Manager → 优雅重启 Worker）
   └── 灰度权重（加权路由，即时回滚）
```

**设计原则**：无共享 Worker · 零拷贝快速路径 · 故障隔离（插件崩溃 → 仅 1 个 Worker 重启）· 运行时可扩展（Lua + .so + etcd 配置）

---

## 🔌 部署插件

Thunder 支持**双模式部署**：Push（exec+tar）+ Pull（Manager 从 admin-web HTTP GET）。Push 是旧链路，Pull 自 #159 起为默认。

```cpp
// code/HelloHttp/src/ModuleHello/ModuleHello.cpp
#include "cmd/Module.hpp"

class ModuleHello : public net::Module {
    bool AnyMessage(const net::tagMsgShell& shell, const HttpMsg& msg) override {
        net::SendToClient(shell, msg, R"({"code":0,"msg":"hello from plugin"})");
        return true;
    }
};
MUDULE_CREATE(core::ModuleHello);
```

```bash
# 1. 构建所有 SO 模块 → deploy/{type}/plugins/
./deploy.sh build

# 2. 上传 .so 到 admin-web 制品库（→ MinIO / PVC）
curl -X PUT --data-binary @ModuleHello.so \
  http://192.168.3.61:30090/api/plugins/HelloHttp/ModuleHello.so

# 3. 下发 — 写 etcd version + so_url → Manager Pull 下载 + 优雅重启
curl -X POST -H "Content-Type: application/json" \
  -d '{"filename":"ModuleHello.so"}' \
  http://192.168.3.61:30090/api/plugins/HelloHttp/deploy

# → 不重启。不丢连接。新连接使用更新后的 .so。
```

| 步骤 | Push 模式（旧，#2） | Pull 模式（默认，#159） |
|------|------|------|
| 上传 | → 本地 PVC | → 本地 PVC + MinIO |
| 下发 | exec+tar 推每个 Pod | etcd bump（version + so_url） |
| Pod 接收 | kubectl cp 通过 K8s API | Manager 轮询 → HTTP GET admin-web → 下载 |
| Pod 重启 | ❌ SO 丢失 | ✅ Manager 启动时自动重新下载 |

---

## 📂 仓库结构

```
code/Net/          核心：Manager、Worker、I/O 后端、编解码器、协程
code/HelloHttp/    HTTP 网关 + 插件 + Lua 模块
code/HelloHttps/   HTTPS 网关
code/HelloWs/      WebSocket 网关
code/Interface/    Protobuf API 网关 → Logic 后端
code/Logic/        业务逻辑节点
deploy/            构建产物、配置文件、Dockerfile
docs/              设计文档、性能基准、常见问题
k8s/               Kubernetes 清单 + 运维手册
tests/             pytest 端到端、冒烟、混沌测试
```

---

## 📖 文档

| 章节 | 适合 |
|:---|:---|
| [`QUICKSTART.md`](QUICKSTART.md) | 构建、部署、灰度、热加载 |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | 开发环境、代码风格、PR 流程、提交规范 |
| [`docs/README.md`](docs/README.md) | 架构 FAQ、设计索引、阅读路径 |
| [`docs/architecture/`](docs/architecture/) | 子系统深入（etcd、协程、io_uring、work-stealing…） |
| [`docs/performance/`](docs/performance/) | 可复现性能基准（Thunder vs Nginx，I/O 后端对比） |
| [`k8s/k8s-manual.md`](k8s/k8s-manual.md) | K8s 集群搭建、CNI、HPA、多数据中心 |
| [`k8s/comparison-openim.md`](k8s/comparison-openim.md) | 与 OpenIM 的部署策略对比 |

---

## 📜 许可证

| 使用场景 | 许可证 | 费用 |
|:---|:---|:---:|
| 开源、学习、内部工具 | [AGPL v3](LICENSE.AGPL) | 免费 |
| 闭源商业产品 | [Commercial](LICENSE.COMMERCIAL) | 付费 |
| SaaS / 云服务 | [Commercial](LICENSE.COMMERCIAL) | 付费 |

> 商业授权咨询 → 联系作者。

---

<p align="center">
  <sub>基于 C++20、libev、io_uring 构建，对延迟极致追求。</sub>
</p>
