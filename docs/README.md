# Thunder Documentation

Thunder 是一个**高性能 C++20 集群异步服务框架**，采用**单线程事件循环 + 协程 + 线程池**架构，适用于高并发、低延迟的服务端场景（网关、微服务、游戏后端）。

本页是文档总览 + 快速索引。设计问答请见 [FAQ.md](FAQ.md)。

## 项目概述

### 核心特性

| 特性 | 说明 |
|------|------|
| 事件驱动 | 基于 libev / io_uring 的事件循环模型 |
| C++20 协程 | 用 `co_await` 替代传统状态机，业务代码线性化 |
| 线程池 offload | CPU 密集 / 阻塞 IO 任务卸载到线程池，不阻塞事件循环 |
| 多进程 | 单机多 Worker 进程，每个进程独立事件循环 |
| 编解码层 | 12 种 codec 可插拔（HTTP/HTTPS/WSS/Protobuf/自定义）|
| 注册中心 | 基于 etcd 的服务注册与发现（CAS slot 分配 + lease 保活）|

### 架构图

```
                        ┌─────────────────────────┐
                        │     etcd 注册中心        │
                        │  /thunder/slot/{0~255}   │
                        │  /thunder/registry/      │
                        └──────────┬──────────────┘
                                   │
     ┌──────────┐     ┌────────────┴────────────┐     ┌──────────┐
     │ Worker 1 │ ──→ │        Center            │ ←── │ Worker N │
     │ (进程)   │     │    (节点发现/路由)        │     │ (进程)   │
     └────┬─────┘     └─────────────────────────┘     └────┬─────┘
          │                                                 │
     ┌────┴─────────────────────────────────────────────────┴────┐
     │                  事件循环 (libev / io_uring)                │
     │  ┌─────────────────────────────────────────────────────┐  │
     │  │  协程 co_await                                      │  │
     │  │  ├── RedisCoHelper (异步 Redis)                      │  │
     │  │  ├── MySqlAwaitable (异步 MySQL)                     │  │
     │  │  └── MakePoolOffloadAwaiter (线程池 offload)         │  │
     │  └─────────────────────────────────────────────────────┘  │
     └───────────────────────────────────────────────────────────┘
```

## 文档地图

```
docs/
├── architecture/   设计文档 —— 框架内部原理与各子系统设计
├── performance/    核心基准 —— Thunder vs Nginx · IO 后端选型（2 篇）
├── quality/        质量验证 —— 内存安全与单元测试覆盖
├── reference/      参考资料 —— 通用工具笔记（非 Thunder 自身设计）
└── FAQ.md          核心设计问答 —— 11 个 Q&A + 性能速查 + 项目亮点
```

## 阅读路径

1. **零基础入门** → [架构全景](architecture/00-overview.md) 🟢 (169 行，读完画出完整数据流)
2. **看整体架构** → [架构设计](architecture/01-architecture-design.md) 🔴 → [Manager-Worker IPC](architecture/06-manager-worker-ipc.md) 🟡 → [etcd 设计](architecture/02-etcd-designed.md) 🟡
3. **看性能** → [Thunder vs Nginx 基准](performance/10-vs-nginx-benchmark-20260610.md) → [IoBackend 四后端对比](performance/11-io-backend-comparison.md)
4. **看具体子系统** → 按下方索引表
5. **常见疑问** → [FAQ.md](FAQ.md)

---

## Architecture — 设计文档

> 🟢 = 入门级 · 🟡 = 进阶级 · 🔴 = 专家级

| 文档 | 主题 | 级别 |
|------|------|:---:|
| [00 架构全景](architecture/00-overview.md) | 新人第一站：完整数据流 + 设计决策速览 | 🟢 |
| [01 核心架构](architecture/01-architecture-design.md) | 进程模型 · 事件循环 · C++20 协程 | 🔴 |
| [02 etcd 设计](architecture/02-etcd-designed.md) | 服务注册 / 发现 / 配置中心 | 🟡 |
| [03 node_id 分配](architecture/03-node-id-allocation.md) | 节点 ID 分配机制 | 🟡 |
| [04 k8s 部署](architecture/04-thunder-on-k8s.md) | Kubernetes 部署方案 | 🟡 |
| [05 优雅重启](architecture/05-graceful-restart.md) | Worker SIGTERM 排空与自动重启 | 🟡 |
| [06 Manager-Worker IPC](architecture/06-manager-worker-ipc.md) | 多进程交互机制 | 🟡 |
| [07 路由下发](architecture/07-upstream-route-filter.md) | 路由按需下发设计 | 🟡 |
| [08 SO/Lua 热更新](architecture/02-etcd-designed.md#配置-key-完整结构--solua-热更新) | SO/Lua 模块 via etcd 热更新 (已合并至 02) | 🟢 |
| [09 LuaJIT 支持](architecture/09-luajit-module-support.md) | LuaJIT 模块支持 | 🟡 |
| [10 协程访问模式](architecture/10-coroutine-access-patterns.md) | C++20 协程访问模式 | 🔴 |
| [11 Lua 跨节点发送](architecture/11-lua-send-to-node-type.md) | Lua 跨节点类型发送 | 🟡 |
| [12 Work-Stealing 线程池](architecture/12-work-stealing-threadpool.md) | 工作窃取线程池设计 | 🔴 |
| [13 io_uring 后端设计](architecture/asio-uring-backend.md) | AsioUring 详细设计 + 两套实现对比 | 🔴 |
| [14 ShmRingQueue 设计](architecture/14-shmringqueue-design.md) | 共享内存无锁环形队列 | 🔴 |
| [15 HTTPS Codec](architecture/15-https-codec.md) | HTTPS 编解码器实现与运维 | 🟡 |
| [16 协议全景](architecture/16-protocols-overview.md) | 协议编解码器全景分析 | 🟢 |
| [17 K8s Canary 路由](architecture/17-k8s-canary-routing.md) | K8s 灰度路由完整设计 | 🔴 |
| [18 Admin Web 重设计](architecture/18-admin-web-redesign.md) | admin-web Go 重写设计文档 | 🔴 |
| [19 entrypoint 与 Compose](architecture/19-entrypoint-and-docker-compose-canary.md) | entrypoint + Docker Compose canary | 🟡 |
| [20 插件 Lua NFS 存储](architecture/20-plugin-lua-nfs-storage.md) | 插件 + Lua + NFS 存储方案 | 🟡 |
| [21 数据面](architecture/21-data-plane.md) | 网络 I/O · Raft 共识 · 共享内存路由 | 🔴 |
| [22 运维内幕](architecture/22-operations-internals.md) | 连接管理 · 插件 · 性能 · S2S 路由 · 配置参考 | 🔴 |

## Performance — 基准数据

| 文档 | 主题 |
|------|------|
| [10 Thunder vs Nginx](performance/10-vs-nginx-benchmark-20260610.md) | 本机 wrk 基准测试 — 吞吐 + 延迟 vs Nginx |
| [11 IoBackend 四后端对比](performance/11-io-backend-comparison.md) | 四种 IO 后端横向对比 — 选型参考 |

> 其他基准数据已合并到对应设计文档末尾（附录 A/B）：
> picohttpparser → [16 协议全景](architecture/16-protocols-overview.md) · 线程池队列 + Work-Stealing → [12 线程池](architecture/12-work-stealing-threadpool.md) · uring 对比 → [13 io_uring](architecture/asio-uring-backend.md) · ShmRingQueue → [14 共享内存](architecture/14-shmringqueue-design.md) · TCP/WS 连接 → [21 数据面](architecture/21-data-plane.md)

## Quality — 质量验证

| 文档 | 主题 |
|------|------|
| [04 共享内存 IPC 验证](quality/04-shm-ring-queue.md) | ShmRingQueue + LoaderConfigVersionData 内存安全与单测覆盖 |

## Reference — 参考资料

> 与 Thunder 自身设计无关的通用工具笔记。

| 文档 | 主题 |
|------|------|
| [01 Sanitizer 验证](reference/01-asan-lsan.md) | ASan / LSan 编译选项功能验证 |
| [网关部署](reference/gateway-deployment.md) | hostNetwork vs NodePort 深度分析 |

---

## 核心设计问答 → [FAQ.md](FAQ.md)

> 单线程事件循环的设计理由？协程 vs 线程？AsyncTask 为什么 final_suspend？
> ThreadPool vs parallel_for？co_await 执行过程？多进程通信？
>
> 全部 11 个 Q&A + 性能速查 + 项目亮点 → **[`FAQ.md`](FAQ.md)**
