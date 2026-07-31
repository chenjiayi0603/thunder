# Thunder ⚡

**C++20 异步应用网关框架 — 热更新 .so 不丢连接，多协议 (HTTP/WS/MQTT/TCP)，灰度路由，三 IO 后端可选。**

[![License](https://img.shields.io/badge/license-AGPL--3.0%20%2B%20Commercial-blue)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-%2300599C?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/20)
[![Platform](https://img.shields.io/badge/platform-Linux%20x86__64-orange)](https://kernel.org)

---

## 一句话：Thunder 是什么

Thunder 是一个**单线程事件循环 + 多进程 + C++20 协程**的网关框架。你可以用 `.so` 插件或 Lua 脚本把它变成 API 网关、游戏接入层、IoT Broker——Thunder 负责网络 IO、协议解析、服务发现、热更新。

```
你的代码 (.so / Lua) 跑在 Thunder Worker 里，
Thunder 负责高性能 IO、分布式路由、零停机更新。
```

对标：如果你用过 Nginx + OpenResty + Lua，Thunder 把这三者做成了一个 C++20 原生的统一框架。

### 为什么选 Thunder 而不是 Nginx

| | Nginx | Thunder |
|---|---|---|
| **热更新业务代码** | ❌ 改配置 reload，连接断开 | ✅ dlopen .so，连接不丢 |
| **多协议** | HTTP/stream 分开 | ✅ HTTP/HTTPS/WS/WSS/MQTT/TCP 统一 |
| **灰度路由** | 需要 Lua / 外部服务 | ✅ etcd 权重 canary，秒级回滚 |
| **自定义协议** | 需要写 C 模块 | ✅ 写 .so 插件即可 |
| **协程** | ❌ | ✅ `co_await` Redis/MySQL/跨节点 RPC/线程池卸载 |
| **线程池** | 多 worker 进程 | ✅ Work-Stealing 多核线性扩展 (2.53x) |
| **共享内存 IPC** | ❌ | ✅ Manager ↔ Worker 零拷贝通信, fd 迁移 |
| **存储驱动** | 依赖外部模块 | ✅ 内置 Redis/MySQL/MongoDB Operator |
| **Lua 脚本** | OpenResty 额外安装 | ✅ 内置 LuaJIT，热加载 <1ms，不重启进程 |
| **纯 HTTP 转发性能** | ~253k RPS | ~221k RPS (小包 Nginx 快 12%) |
| **P50 延迟** | 386-392μs | 198-254μs (低 35-49%) |

> 结论：只需要做 HTTP 反向代理 → 选 Nginx。需要写业务逻辑、多协议接入、协程访问存储、热更新不丢连接 → 选 Thunder。

---

## 🚀 15 秒快速开始

```bash
git clone --recurse-submodules https://github.com/chenjiayi0603/thunder.git
cd thunder
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTHUNDER_IO_ASIO_URING=ON
cmake --build build -j$(nproc) && cmake --install build

# Docker Compose 一键启动
./deploy.sh test compose --quick

# 测试
curl http://127.0.0.1:27006/hello/hello -d '{"option":"Echo"}'
# → {"code":0,"msg":"ok"}
```

---

## 🧬 架构一览

```
                              ┌─────────────────┐
                              │   etcd 集群 ×3   │  ← 服务注册 · 配置 · 灰度权重
                              └────────┬────────┘
                                       │ Watch (毫秒级推送)
              ┌────────────────────────┼────────────────────────┐
              │                        │                        │
    ┌─────────▼─────────┐    ┌────────▼────────┐    ┌─────────▼─────────┐
    │   Manager 进程     │    │   Manager 进程   │    │   Manager 进程     │
    │  (守护 + 热更新)   │    │  (守护 + 热更新) │    │  (守护 + 热更新)   │
    └──┬─────────────┬──┘    └──┬────────────┬──┘    └──┬─────────────┬──┘
       │fork         │fork      │fork         │fork     │fork         │fork
  ┌────▼───┐   ┌────▼───┐  ┌───▼────┐  ┌───▼────┐  ┌──▼────┐  ┌───▼────┐
  │Worker 0│   │Worker 1│  │Worker 0│  │Worker 1│  │Worker0│  │Worker 1│
  │epoll/  │   │epoll/  │  │epoll/  │  │epoll/  │  │epoll/ │  │epoll/  │
  │io_uring│   │io_uring│  │io_uring│  │io_uring│  │iouring│  │io_uring│
  └───┬────┘   └───┬────┘  └───┬────┘  └───┬────┘  └──┬────┘  └───┬────┘
      │            │           │           │          │           │
      └────────────┼───────────┼───────────┼──────────┼───────────┘
                   │           │           │          │
          ┌────────▼───────────▼───────────▼──────────▼────────┐
          │              客户端请求 (hostNetwork)              │
          │      HTTP · HTTPS · WebSocket · WSS · Protobuf    │
          └───────────────────────────────────────────────────┘
```

**一条请求的完整路径：**

```
客户端 → TCP → picohttpparser(SIMD) → 前缀匹配 → 插件 AnyMessage() → yyjson → TCP
         └─ Fast Path (~4.3μs CPU) ─┘              └─ Normal Path (Protobuf 编解码) ─┘
```

---

## 🎯 架构设计决策

| 决策 | 为什么 | 代价 |
|:---|:---|:---|
| **单线程事件循环** | 零锁、零竞争、极简 | 单核 CPU（多进程补偿）|
| **多进程 (1 Manager + N Worker)** | 利用多核 + 故障隔离（插件崩只崩一个 Worker）| 进程间需 IPC |
| **C++20 协程 (`co_await`)** | 异步代码线性化，告别回调地狱 | 学习曲线 |
| **io_uring (asio + native)** | 批量提交 N 次 IO → 1 次 syscall，TLS 延迟砍半；native 直通内核无用户态事件循环 | Linux 5.1+ |
| **hostNetwork (非 NodePort)** | 数据面零 K8s 组件 | Pod 不能漂移 |
| **etcd (非 K8s DNS)** | 热配置推送 + 灰度权重 + 秒级回滚 | 需维护 etcd 集群 |
| **dlopen SO 热更新** | 不重启进程，连接不断 | SO 需兼容 ABI |
| **Work-Stealing 线程池** | 每 Worker 独立队列，空闲偷取 | 比旧单队列快 2.5x |

---

## 📊 性能

*i9-12900H, Ubuntu 26.04, 1GbE 真实网卡, `wrk -t4 -c100 -d10s`*

公平对比: POST 变长二进制 (不解析) → 固定返回 `{"code":0,"msg":"ok"}`。两端完全对等。

### HTTP

| Body | ev | native_uring | asio_uring | Nginx 1w |
|-----:|----:|----:|----:|----:|
| 64 B | 183k / 214μs | 180k / 198μs | 193k / 254μs | **252k / 392μs** |
| 1 KB | 143k / 485μs | 214k / 218μs | 182k / 289μs | **256k / 386μs** |
| 4 KB | 153k / 238μs | 217k / 222μs | **221k / 201μs** | 253k / 392μs |
| 64 KB | 193k / 198μs | 189k / 207μs | 216k / 216μs | **256k / 388μs** |

### HTTPS

| Body | ev | native_uring | asio_uring | Nginx SSL |
|-----:|----:|----:|----:|----:|
| 64 B | 74k / 0.95ms | 101k / 716μs | **142k / 323μs** | 161k / 581μs |
| 1 KB | 109k / 364μs | 144k / 339μs | 137k / 355μs | **165k / 567μs** |
| 4 KB | 112k / 353μs | 138k / 372μs | 113k / 334μs | **167k / 568μs** |
| 64 KB | 108k / 347μs | **146k / 327μs** | 135k / 324μs | 167k / 568μs |

> Nginx 吞吐全场景领先 (HTTP ~253k, HTTPS ~165k)，且不受请求 body 大小影响。
> Thunder 三后端 P50 延迟优于 Nginx，HTTPS asio_uring 距 Nginx 仅 12% (142k vs 161k)。
> 📖 完整报告：[`docs/performance/20-real-nic-benchmark.md`](docs/performance/20-real-nic-benchmark.md)

---

## 🚀 功能矩阵

| 分类 | 能力 |
|:---|:---|
| **协议** | HTTP/1.1 · HTTPS (TLS 1.3) · WebSocket · WSS · 内部 Protobuf RPC · MQTT 3.1.1 |
| **I/O 后端** | `ev` (epoll) · `asio_uring` (io_uring 批量提交) · `native_uring` |
| **HTTP 解析** | picohttpparser（SSE4.2 SIMD, vs http_parser **+49% RPS**）|
| **协程** | `co_await` Redis · `co_await` MySQL · `co_await` 跨节点 RPC · `co_await` 线程池卸载 |
| **线程池** | Work-Stealing（Go LRQ 风格）· 每 Worker 独立队列 · 空闲偷取 · 三级分发 |
| **脚本** | 每 Worker 独立 LuaJIT VM · Lua 热加载 (<1ms, 不重启进程) |
| **插件** | `.so` 动态加载 · etcd 触发优雅切换 · ABI 版本校验 |
| **服务发现** | etcd 注册中心 · lease 心跳 · CAS 槽位分配 · Watch 实时推送 |
| **灰度** | 加权路由 (`v1=70% v2=30%`) · etcd 权重键 · 不杀 Pod 秒级回滚 |
| **健康检查** | `GET /health` → `{"status":"ok"}` · K8s httpGet probe |
| **管理后台** | Web 控制台 → 插件管理 · 节点拓扑 · etcd 浏览器 |
| **部署** | Docker Compose · 裸金属 · Kubernetes (hostNetwork + StatefulSet) |
| **可观测** | `/metrics` 端点 (Prometheus) · 分级日志 (TRACE→FATAL) |

---

## 🔌 插件 — 一行宏注册

```cpp
// code/HelloHttp/src/ModuleHello/ModuleHello.cpp
#include "cmd/Module.hpp"

class ModuleHello : public net::Module {
    bool AnyMessage(const net::tagMsgShell& shell, const HttpMsg& msg) override {
        net::SendToClient(shell, msg, R"({"code":0,"msg":"hello"})");
        return true;
    }
};
MUDULE_CREATE(core::ModuleHello);  // ← 自动导出 ABI 版本 + create()
```

```bash
cmake --build build && cmake --install build
# .so 安装到 deploy/HelloHttp/plugins/
```

**热更新**：

  1. 修改代码 → 重编 .so (`cmake --install` → `deploy/HelloHttp/plugins/xxx.so`)
  2. 上传: 通过 admin-web (curl PUT 或 Web 界面) → MinIO `artifacts` 桶 + 本地 `/app/data/artifacts/`
  3. 下发: 点击"下发" (或 POST `/api/plugins/{Type}/deploy`) → etcd 写入 `so_url` 和版本号
     (不移动文件、不复制，只改 etcd 通知)
  4. 各节点 Manager Watch 感知版本变化 → HTTP Pull MinIO 下载 .so
  5. Worker dlopen 加载新 .so，旧连接排空不丢。

---

## 📂 仓库结构

```
code/Net/          核心：Manager、Worker、I/O 后端、编解码器、协程
code/HelloHttp/    HTTP 网关 + 插件 + Lua 模块
code/HelloMqttBroker/  MQTT 3.1.1 Broker
code/Interface/    Protobuf API 网关 → Logic 后端
code/Logic/        业务逻辑节点
deploy/            构建产物、配置文件、Dockerfile
docs/architecture/ 设计文档（etcd · 协程 · io_uring · work-stealing…）
docs/performance/  可复现基准（Thunder vs Nginx · I/O 后端对比）
k8s/               Kubernetes 清单 + 运维手册
tests/             pytest E2E · 混沌测试 · 冒烟测试
```

---

## 📖 文档

| 章节 | 适合 |
|:---|:---|
| [`QUICKSTART.md`](QUICKSTART.md) | 构建、部署、灰度、热加载 |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | 开发环境、代码风格、PR 流程 |
| [`docs/architecture/00-overview.md`](docs/architecture/00-overview.md) | 架构全景，读完能画出完整数据流 |
| [`docs/architecture/01-architecture-design.md`](docs/architecture/01-architecture-design.md) | 进程模型 · 事件循环 · C++20 协程 |
| [`docs/architecture/02-etcd-designed.md`](docs/architecture/02-etcd-designed.md) | etcd 服务发现 · NodeID 分配 · vs CoreDNS 对比 |
| [`docs/performance/10-vs-nginx-benchmark.md`](docs/performance/10-vs-nginx-benchmark.md) | Thunder vs Nginx 完整基准 |
| [`docs/api.md`](docs/api.md) | HTTP API 接口文档 |

---

## 📜 许可证

| 使用场景 | 许可证 | 费用 |
|:---|:---|:---:|
| 开源、学习、内部工具 | [AGPL v3](LICENSE.AGPL) | 免费 |
| 闭源商业产品 | [Commercial](LICENSE.COMMERCIAL) | 付费 |
| SaaS / 云服务 | [Commercial](LICENSE.COMMERCIAL) | 付费 |

