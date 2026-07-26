# Thunder 架构全景

> 🟢 **入门级** — 读完这篇你就能画出 Thunder 的完整数据流。
> 适合：刚跑通 Quick Start，想理解"这个框架到底怎么工作的"。

---

## Thunder 是什么

一个 **C++20 单线程事件循环 + 多进程 + 协程** 的高性能网关框架。
它不是 Web 服务器（不是 Nginx），不是 RPC 框架（不是 gRPC），
而是**网关层的基础设施**——你可以用插件把它变成 API 网关、游戏接入层、实时消息代理。

```
你的代码 (.so / Lua) 跑在 Thunder 的 Worker 里，
Thunder 帮你搞定网络 IO、协议解析、服务发现、热更新。
```

---

## 一句话架构

```
                          ┌─────────────┐
                          │ etcd ×3     │  注册 · 配置 · 灰度权重
                          └──────┬──────┘
                                 │ watch
  ┌──────────┐          ┌───────┴───────┐          ┌──────────┐
  │ Worker 0 │ ←fork── │    Manager     │ ──fork→ │ Worker N │
  │ 事件循环  │         │  守护 + 热更新  │         │ 事件循环  │
  │ + .so插件 │         └───────────────┘         │ + .so插件 │
  │ + Lua VM  │                                   │ + Lua VM  │
  └─────┬─────┘                                   └─────┬─────┘
        │ epoll / io_uring                              │
        ▼                                               ▼
  ┌─────────────────────────────────────────────────────────┐
  │                   客户端请求                              │
  │         HTTP · HTTPS · WebSocket · WSS · Protobuf       │
  └─────────────────────────────────────────────────────────┘
```

**三个关键角色：**

| 角色 | 做什么 | 类比 |
|:---|:---|:---|
| **Manager** | fork Worker 进程、watch etcd 变更、触发热更新 | systemd / supervisord |
| **Worker** | 事件循环 + 运行插件 + 处理请求 | Nginx worker process |
| **etcd** | 服务注册、配置中心、canary 权重存储 | Consul / ZooKeeper |

---

## 一条请求的完整路径

### Fast Path（匹配已知路由前缀）

```
客户端 → TCP recv
       → picohttpparser (SSE4.2 SIMD 解析 HTTP 头)
       → 前缀匹配 "/hello/echo" ✅
       → 跳过 Protobuf 解码
       → 你的插件 AnyMessage()
       → yyjson 构造响应
       → TCP send
       
  耗时: ~220 μs (64B payload, asio_uring)
  QPS:  ~235k / core
```

### Normal Path（需要 Protobuf 编解码）

```
客户端 → TCP recv
       → HTTP 解析
       → Protobuf 解码 (MsgHead + MsgBody)
       → 你的插件
       → Protobuf 编码
       → TCP send

  QPS: ~162k / core
```

> 📖 深度阅读：[protocols-overview.md](16-protocols-overview.md) · [picohttpparser 分析](../architecture/16-protocols-overview.md#附录性能基准数据)

---

## 核心设计决策

| 决策 | 为什么 | 代价 |
|:---|:---|:---|
| **单线程事件循环** | 零锁、零竞争、极简 | 单核 CPU（多进程补偿）|
| **多进程 (1 Manager + N Worker)** | 利用多核 + 故障隔离（插件崩只崩一个 Worker）| 进程间需 IPC |
| **C++20 协程** | 异步代码线性化，告别状态机 | 学习曲线 |
| **io_uring 批量提交** | N 次 IO → 1 次 syscall，TLS 延迟砍半 | Linux 5.1+ |
| **etcd (非 K8s DNS)** | 热配置推送 + canary 灰度 + 版本回滚 | 需维护 etcd 集群 |
| **hostNetwork (非 NodePort)** | 数据面零 K8s 组件 | Pod 不能漂移 |
| **dlopen SO 热更新** | 不重启进程，连接不断 | SO 需兼容 ABI |

> 📖 深度阅读：[FAQ.md](../FAQ.md) · [架构设计](01-architecture-design.md) · [io_uring 设计](asio-uring-backend.md)

---

## IO 后端一览

Thunder 支持插拔式 IO 后端，编译时选择：

| Backend | 底层 | 特点 | 适用场景 |
|:---|:---|:---|:---|
| `ev` | epoll | 最轻量，每 IO 一次 syscall | 小 payload，高并发短连接 |
| `asio_uring` | io_uring | 批量提交，延迟最低 | 大 payload，TLS 密集 |
| `native_uring` | io_uring 原始接口 | 无批量优势，演示用 | 学习/对比 |
| `dpdk` | 内核旁路 | 计划中 | 极低延迟场景 |

> 📖 深度阅读：[IoBackend 对比](../performance/11-io-backend-comparison.md) · [AsioUring 设计](asio-uring-backend.md)

---

## 插件模型

```
Worker 进程
  ├── HelloHttp_ModuleHello.so   ← 你的业务插件 (C++)
  ├── HelloHttp_ModuleLua.so     ← Lua 脚本宿主
  │     └── echo.lua             ← 你的 Lua 脚本 (热更新, 不停机)
  └── HelloHttp_ModuleOrder.so   ← 另一个业务插件
```

**两种扩展方式：**

| | C++ .so 插件 | Lua 脚本 |
|:---|:---|:---|
| 性能 | 原生 C++ | LuaJIT，接近原生 |
| 热更新 | etcd 触发 → Worker 优雅重启 → dlopen 新 .so | etcd 触发 → ReloadScript()，不动进程 |
| 适用 | 高性能路径、复杂协议解析 | 业务逻辑、频繁变更 |
| 部署 | NFS 共享 / Docker 镜像内 | etcd 存储，push 即生效 |

> 📖 深度阅读：[SO 热更新](08-so-module-hot-reload-via-etcd.md) · [Lua 模块](09-luajit-module-support.md) · [NFS 存储](20-plugin-lua-nfs-storage.md)

---

## 性能快照

| 指标 | Thunder | Nginx / 旧方案 |
|:---|:---:|:---:|
| HTTP 1KB RPS (单核) | **232k** | Nginx 191k (+21%) |
| HTTP 64B 延迟 | **220 μs** | Nginx 466 μs |
| HTTPS 4KB 延迟 | **247 μs** | Nginx 824 μs |
| 协程切换开销 | 纳秒级 | 线程微秒级 |
| Work-stealing 加速比 | **543 ns/op** | 旧单队列线程池 1374 ns/op |

> 📖 完整数据：[Thunder vs Nginx](../performance/10-vs-nginx-benchmark-20260610.md) · [性能总览](../performance/)

---

## 阅读下一步

| 你想... | 读这篇 |
|:---|:---|
| 深入理解整体架构 | [01-architecture-design.md](01-architecture-design.md) 🔴 |
| 理解 etcd 怎么用 | [02-etcd-designed.md](02-etcd-designed.md) 🟡 |
| 部署到 K8s | [04-thunder-on-k8s.md](04-thunder-on-k8s.md) 🟡 |
| 理解协程怎么写 | [10-coroutine-access-patterns.md](10-coroutine-access-patterns.md) 🔴 |
| 理解 io_uring 怎么接 | [asio-uring-backend.md](asio-uring-backend.md) 🔴 |
| 理解热更新机制 | [08-so-module-hot-reload-via-etcd.md](08-so-module-hot-reload-via-etcd.md) 🟡 |
| 看性能测试数据 | [../performance/](../performance/) 🟢 |
| 看设计 FAQ | [../FAQ.md](../FAQ.md) 🟢 |

---

> 🟢 = 入门级 · 🟡 = 进阶级 · 🔴 = 专家级
