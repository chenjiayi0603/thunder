# Thunder

Thunder 是一个基于 C++20 协程的高性能网关与分布式服务框架，支持 HTTP / HTTPS / WebSocket 多协议接入，通过 Protobuf RPC 将请求路由到后端 Logic 节点，运行时可通过 Lua 脚本和 `.so` 插件热更新扩展行为——全部运行在单线程事件循环中，**单核 23 万+ RPS**。

```
客户端 (HTTP / HTTPS / WS)
        │
        ▼
   Worker (事件循环 + .so 插件 + Lua VM)
        │  io_uring / epoll
        ▼
   etcd 服务网格
        │
   ┌────┴────┐
 LOGIC     LOGIC     (C++20 协程，水平扩展)
```

---

## 构建

**环境要求**: CMake ≥ 3.20, GCC 12+ 或 Clang 15+, OpenSSL 头文件, Docker + Compose

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target thirdparty_deploy -j1   # 编译三方库，首次约 10~20 分钟
cmake --build build -j1                               # 编译主工程
cmake --install build                                 # 安装到 deploy/
```

> 必须 `-j1`——多线程编译会因磁盘 IO 瓶颈卡死。

三方库已就绪后的日常编译：

```bash
cmake --build build -j1 && cmake --install build
```

可选选项：

```bash
-DTHUNDER_IO_ASIO_URING=ON    # 启用 asio io_uring 后端 (Linux 5.1+)
-DTHUNDER_LUAJIT=ON           # 启用 LuaJIT
```

---

## 运行

```bash
./deploy.sh up          # 启动 Docker 集群（3 节点 etcd + MySQL + Redis + 全部服务）
./deploy.sh status      # 查看容器状态与监听端口
./deploy.sh down        # 停止并清理
```

等待约 15 秒，所有服务进入 healthy 状态后：

```bash
curl http://127.0.0.1:27006/hello/hello -d '{"option":"Echo","data":"hi"}'
# → {"code":0,"msg":"ok","data":"hi"}
```

**服务端口：**

| 服务 | 协议 | 端口 |
|------|------|------|
| HelloHttp | HTTP | 27006 |
| HelloHttps | HTTPS | 27443 |
| HelloWs | WebSocket | 27010 |
| Interface | HTTP | 27008 |
| Logic | 内部 S2S | 16068 |
| Admin | HTTP | 8090 |
| etcd | HTTP | 2379 |
| Redis | TCP | 6379 |
| MySQL | TCP | 3306 |

---

## 测试

```bash
./deploy.sh test unit      # C++ gtest (382 例) + Python pytest，零外部依赖，约 45 秒
./deploy.sh test e2e       # Docker E2E: compose up → 25+ pytest 用例 → compose down，约 3 分钟
./deploy.sh test           # unit + e2e
./deploy.sh clean          # 清理构建产物与 Docker 状态
```

冒烟测试（需集群已启动）：

```bash
./tests/test_smoke.sh      # HTTP / HTTPS / WS / Interface→Logic / etcd，9 项检查
```

---

## 性能

*i9-12900H, 1 worker, `wrk -t4 -c100 -d10s`, INFO 日志, P-core 绑定。完整报告见 [`docs/performance/10-vs-nginx-benchmark-20260610.md`](docs/performance/10-vs-nginx-benchmark-20260610.md)*

### HTTP 吞吐对比 Nginx 1.x

| 载荷 | Thunder ev | Thunder asio_uring | Nginx | 对比 |
|-----:|:----------:|:------------------:|:-----:|:----:|
| 64 B | 232k RPS | **235k RPS** | 214k RPS | **+9–10%** |
| 1 KB | 229k RPS | 232k RPS | 191k RPS | **+20–21%** |
| 4 KB | 216k RPS | 223k RPS | 184k RPS | **+17–21%** |

### HTTP 延迟（asio_uring 后端）

| 载荷 | Thunder ev | Thunder asio_uring | Nginx | 对比 |
|-----:|:----------:|:------------------:|:-----:|:----:|
| 64 B | 424 µs | **220 µs** | 466 µs | **低 2.1 倍** |
| 4 KB | 457 µs | **332 µs** | 543 µs | **低 1.6 倍** |

### HTTPS 延迟（TLS）

| 载荷 | Thunder ev | Thunder uring | Nginx | 对比 |
|-----:|:----------:|:-------------:|:-----:|:----:|
| 64 B | 803 µs | **402 µs** | 752 µs | **低 1.9 倍** |
| 4 KB | 1.23 ms | **247 µs** | 824 µs | **低 3.3 倍** |

> HTTPS 吞吐受限于 SSL-CPU；Thunder uring 延迟优势来自 `io_uring` 将多次 OpenSSL BIO 调用批量为一次 `io_uring_enter`，而非每次 BIO 读写触发一次 syscall。

### WebSocket Echo（长连接）

| 载荷 | 10 连接 RPS | p50 | p99 |
|-----:|:----------:|:----:|:----:|
| 64 B | 46,765 | 192 µs | 549 µs |
| 1 KB | 15,888 | 564 µs | 1.7 ms |
| 4 KB | 4,881 | 1.8 ms | 5.9 ms |

---

## Thunder 为什么快

### 零拷贝快路径

匹配已知路由前缀的请求完全跳过 Protobuf + JSON 解码：

```
普通路径:  收包 → 解析 → pb 解码 → 处理 → pb 编码 → 发送   (~162k RPS)
快路径:    收包 → 前缀匹配 → 处理 → memcpy 模板 → 发送    (~236k RPS)
```

### picohttpparser — SIMD HTTP 解析

用 `picohttpparser` 替换 `http_parser`，单头文件 SSE4.2 加速解析器，每周期扫描 16 字节。**实测提升：+49% RPS。**

### 可插拔 I/O 后端

| 后端 | 优势 | 说明 |
|------|------|------|
| `ev` (epoll) | 默认，开销最低 | 适合小中载荷 |
| `asio_uring` | 延迟敏感、大载荷 | 批量提交：N 次 IO → 1 次 syscall |
| `native_uring` | 原生 io_uring | Demo 后端，无批量优势 |
| `dpdk` | 规划中 | 内核旁路 |

### 多进程 Worker 模型

每个 Worker 是独立的操作系统进程。插件崩溃只会影响该 Worker，Manager 透明重启。优雅重启先排空进行中的连接，再关闭旧 Worker。

### Work-Stealing 线程池

协程将阻塞操作（磁盘 IO、CPU 密集计算）offload 到自研 work-stealing 线程池（Go LRQ 风格）。每个 worker 持有双 SPMC ring buffer（256 slot）：`_submit_deques` 通过 Power of Two Choices 接收提交的任务，`_local_deques` 暂存偷来的任务。空闲 worker 用一次 CAS 偷走忙碌 worker 一半任务（`steal_into`）。全局 MPMC 队列兜底。

| 指标 | 旧（单 MPMC 队列）| 新（Work-Stealing）|
|------|:------:|:------:|
| 吞吐（4 worker）| 1,373 ns/op | **543 ns/op** |
| 加速比 | — | **2.53 倍** |
| 端到端延迟 avg | 1,394 ns | **700 ns** |
| 端到端延迟 P50 | 1,586 ns | **1,227 ns** |
| 64B 载荷优势 | — | **2.58 倍** |

设计文档：[`docs/architecture/23-work-stealing-threadpool.md`](docs/architecture/23-work-stealing-threadpool.md)  
基准测试：[`docs/performance/04-work-stealing-bench.md`](docs/performance/04-work-stealing-bench.md)

### C++20 协程

所有异步 IO — MySQL、Redis、跨节点 RPC — 统一用 `co_await`：

```cpp
net::AsyncTask HandleRequest(net::StepCo20& step) {
    auto rows = co_await db.Query("SELECT * FROM orders WHERE user_id=?", userId);
    bool cached = co_await cache.Set("orders:" + id, rows.toJson(), 300);
    co_await step.SendToInternalByNodeTypeAsync("LOGIC", head, body);
    step.Response(200, buildResponse(rows));
}
```

---

## 特性

| 特性 | 说明 |
|------|------|
| HTTP/1.1 | picohttpparser, keep-alive, chunked transfer |
| HTTPS / TLS | OpenSSL, SNI |
| WebSocket (WS / WSS) | Upgrade, ping/pong, 分片, TLS 变体 |
| 内部 Protobuf RPC | 节点间二进制传输 |
| MySQL 客户端 | `co_await db.Query(...)` — 非阻塞, 事件循环安全 |
| Redis 客户端 | `co_await cache.Get/Set/HSet(...)` — 异步 hiredis |
| Work-Stealing 线程池 | Go LRQ 风格, 双 SPMC deque, P2C 分发, 2.53 倍加速 |
| Lua 脚本 | LuaJIT, 热加载, 每 Worker 独立 VM |
| `.so` 插件热切换 | 通过 etcd watch + 优雅重启实现零停机部署 |
| etcd 服务网格 | 注册、发现、配置推送、TTL 健康检查 |
| Admin 管理后台 | 插件管理、节点拓扑、etcd 浏览器 |

---

## 架构

```
┌─────────────────────────────────────────────────────────────────┐
│                         网关节点                                 │
│                                                                 │
│  Manager                                                        │
│    ├── fork / 重启 Workers                                      │
│    ├── 从 etcd 接收插件更新                                      │
│    └── 优雅排空 + 热切换                                         │
│                                                                 │
│  Worker 0..N  (每个一个事件循环)                                  │
│    ├── I/O 后端: ev / asio_uring / native_uring                 │
│    ├── HTTP 快路径 (picohttpparser + 前缀匹配)                   │
│    ├── 编解码链: HTTP → Protobuf → 响应                          │
│    ├── .so 模块 (动态加载)                                       │
│    ├── Lua VM (LuaJIT, 每 Worker)                                │
│    └── C++20 协程                                                │
│         ├── co_await MySQL / Redis                              │
│         ├── co_await HTTP 上游                                  │
│         └── co_await 跨节点 PB RPC                               │
└───────────────────────────┬─────────────────────────────────────┘
                            │ 内部 Protobuf (TCP)
┌───────────────────────────▼─────────────────────────────────────┐
│                        Logic 节点                                │
│  与 Worker 相同架构; 通过 etcd 水平扩展。                          │
└───────────────────────────┬─────────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────────┐
│                    etcd 集群 (3 节点)                             │
│         服务注册 · 配置中心 · Leader 选举                          │
└─────────────────────────────────────────────────────────────────┘
```

| 原则 | 实现 |
|------|------|
| 无共享 | 每 Worker 一进程，无跨 Worker 锁 |
| 零阻塞 | `co_await` 处理所有 IO；线程池处理 CPU 密集型任务 |
| 尽可能零拷贝 | 快路径 + yyjson arena 分配器 |
| 故障隔离 | 插件崩溃只影响单个 Worker；Manager 自动重启 |
| 运行时可扩展 | Lua 处理逻辑；`.so` 处理热路径；etcd 管理配置 |

---

## 写一个插件

```cpp
// code/HelloHttp/src/ModuleHello/ModuleHello.cpp
#include "cmd/Module.hpp"
#include "util/CJsonObject.hpp"

class ModuleHello : public net::Module {
public:
    bool AnyMessage(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg) override {
        util::CJsonObject req(oInHttpMsg.body());
        std::string action;
        req.Get("option", action);

        util::CJsonObject rsp;
        rsp.Add("code", 0);
        rsp.Add("action", action);
        net::SendToClient(stMsgShell, oInHttpMsg, rsp.ToString());
        return true;
    }
};

MUDULE_CREATE(core::ModuleHello);
```

构建并部署：

```bash
./deploy.sh build-so HelloHttp_ModuleHello

# 通过 Admin API 提取到 Worker（触发优雅热切换）
curl -X POST http://localhost:8090/api/so-extract -F "file=ModuleOrder.so"
```

---

## Kubernetes

```bash
kubectl apply -f k8s/
kubectl -n thunder rollout status deployment --timeout=120s

# NodePort: HTTP=30006  Interface=30008  HTTPS=30043  WS=30010  Admin=30090
```

---

## 仓库结构

```
code/
├── Net/          # 核心: Manager, Worker, I/O 后端, 编解码, 协程
├── Center/       # 集群协调 (遗留, 部署中已由 etcd 替代)
├── Logic/        # 示例 Logic 节点
├── HelloHttp/    # HTTP 网关节点 + 示例插件 + Lua 模块
├── HelloHttps/   # HTTPS 网关节点
├── HelloWs/      # WebSocket 网关节点
├── Interface/    # Protobuf 网关节点
├── Proto/        # .proto 定义
└── Util/         # JSON (yyjson), 日志, 数据库辅助

deploy/           # 编译产物, 节点配置, 启动脚本
docs/             # 文档索引见 docs/README.md
├── architecture/ # 设计文档
├── performance/  # 基准报告
├── quality/      # 内存安全与单元测试验证
└── reference/    # 工具, 依赖分析, FAQ
k8s/              # Kubernetes 部署清单
tests/            # pytest E2E, 冒烟脚本, 基准脚本
```

---

## 许可

Thunder 采用 **双重许可**：

| 使用场景 | 许可 | 费用 |
|---------|------|:--:|
| 个人学习、开源项目 | [AGPL v3](LICENSE.AGPL) | 免费 |
| 公司内部工具、测试环境 | [AGPL v3](LICENSE.AGPL) | 免费 |
| **闭源商业产品** | **[商业许可](LICENSE.COMMERCIAL)** | **付费** |
| **SaaS / 云服务** | **[商业许可](LICENSE.COMMERCIAL)** | **付费** |

> 详细条款见 [`LICENSE`](LICENSE)。商业许可咨询请联系作者。
