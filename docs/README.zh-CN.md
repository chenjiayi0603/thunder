# Thunder — C++20 高性能多协议网关与微服务框架

[![License](https://img.shields.io/badge/license-AGPL_v3_%2B_商业许可-blue)](../LICENSE)
[![Release](https://img.shields.io/badge/release-v0.9.5-green)](https://github.com/chenjiayi0603/thunder/releases)

Thunder 是一个基于 C++20 协程的自研高性能网关框架，支持 HTTP / HTTPS / WebSocket / 自定义协议接入，内置 etcd 服务注册与配置中心、插件化扩展体系，单核 **23 万+ RPS**。

```
客户端 (HTTP/HTTPS/WS)
  │
  ▼
Worker (事件循环 + .so 插件 + Lua 虚拟机)
  │
  ▼
etcd 服务网格 (3 节点)
  │
  ▼
Logic 节点 (C++20 协程，水平扩展)
```

## 核心特性

| 特性 | 说明 |
|------|------|
| **多协议网关** | HTTP/1.1、HTTPS/TLS、WebSocket、WSS、Protobuf RPC，策略模式支持无限扩展新协议 |
| **极致性能** | 64B HTTP 单核 23.5 万 RPS（比 Nginx 高 10%），HTTPS 延迟 402μs（比 Nginx 低 1.9×） |
| **自研 Work-Stealing 线程池** | Go LRQ 风格，双 WorkerDeque（SPMC ring buffer），Power of Two Choices 分发，偷取批量 1 次 CAS，4 worker 吞吐比旧单队列快 **2.53×** |
| **C++20 协程** | 所有异步 IO（MySQL、Redis、RPC）统一 `co_await` 写法，同步风格异步执行 |
| **etcd 服务网格** | 服务注册发现、配置热更新、TTL 健康检查、元数据分配 |
| **多进程隔离** | Manager 父进程 + Worker 数据子进程，子进程崩溃不影响其他进程，Manager 自动重启 |
| **热更新零中断** | .so 插件通过 etcd 版本触发 GracefulRestart（排空连接 → 新 Worker 接管）；Lua 脚本热加载（LuaJIT，不重启 VM） |
| **可插拔 I/O** | epoll、asio_uring、native_uring 多后端，换后端只改一行配置 |
| **SIMD HTTP 解析** | picohttpparser SSE4.2 加速，16 字节/周期并行扫描 |
| **进程间通信** | 共享内存 SPSC 无锁队列同步路由（150 万 QPS，延迟 0.15ms）；SPMC 原子版本号同步配置 |

## 性能概览

*i9-12900H, 1 worker, wrk -t4 -c100 -d10s*

| 指标 | Thunder | Nginx | 对比 |
|------|:------:|:-----:|:----:|
| HTTP 64B 吞吐 | **235k RPS** | 214k RPS | +10% |
| HTTP 64B 延迟 | **220 μs** | 466 μs | 2.1× 更低 |
| HTTPS 64B 延迟 | **402 μs** | 752 μs | 1.9× 更低 |
| HTTPS 4KB 延迟 | **247 μs** | 824 μs | 3.3× 更低 |

### 线程池性能

| 指标 | 旧（单 MPMC 队列） | 新（Work-Stealing） |
|------|:------:|:------:|
| 4 worker 吞吐 | 1,373 ns/op | **543 ns/op (2.53×)** |
| 端到端延迟 avg | 1,394 ns | **700 ns (1.99×)** |
| payload 64B | — | **2.58× 加速** |

## 架构

```
Manager
  ├── fork / 重启 Worker
  └── 接收 etcd 插件更新
       │
Worker 0..N (每进程一个事件循环)
  ├── I/O: epoll / asio_uring / native_uring
  ├── HTTP 快路径 (picohttpparser + 前缀匹配，跳过 PB)
  ├── C++20 协程 (co_await MySQL / Redis / RPC)
  ├── Work-Stealing 线程池 (阻塞 IO / CPU 密集型 offload)
  ├── .so 插件 + LuaJIT VM
  └── 共享内存 → Manager (路由同步、配置下发)
```

## 线程池设计要点

Thunder 的协程不能做阻塞操作（会卡死事件循环）。磁盘 IO、CPU 密集计算、同步 SDK 调用需要 offload 到线程池。

| 维度 | 协程（事件循环） | 线程池 |
|------|:--:|:--:|
| 适合任务 | 微秒级（解析、路由、小 IO） | 毫秒级（connect、sleep、压缩） |
| 上下文切换 | 0（函数调用级） | ~μs（线程切换） |
| 内存 | ~几 KB/协程 | ~MB/线程 |
| 分工 | 99% 快速路径 | 1% 阻塞路径 |

## 许可

**AGPL v3 + 商业许可 双重许可**（参考 Redis 7.4+ 模式）

| 使用场景 | 许可 | 费用 |
|---------|------|:--:|
| 个人学习、开源项目 | AGPL v3 | 免费 |
| 公司内部工具 | AGPL v3 | 免费 |
| 闭源商业产品 | 商业许可 | 付费 |
| SaaS / 云服务 | 商业许可 | 付费 |

> [English README](../README.md) | [设计文档](./architecture/)
