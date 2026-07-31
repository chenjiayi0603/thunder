# Thunder vs Nginx：性能对比

> 日期: 2026-07-31 | 环境: Ubuntu 26.04 LTS | 工具: wrk 4.1.0
> 网卡: 192.168.3.61 (enp0s31f6, 1GbE)

---

## 测试环境

| 项目 | 值 |
|------|-----|
| CPU | i9-12900H, 6P+8E, 20 线程 |
| OS | Ubuntu 26.04 LTS, Linux 7.0.0-22 |
| governor | performance |
| 绑核 | 服务 P-core 4-9, wrk E-core 12-19 |
| Worker | 1 |
| wrk | -t4 -c100 -d10s --latency |

## 测试方法

**完全对等**: POST 变长二进制 body (不解析) → 固定返回 `{"code":0,"msg":"ok"}` (24B)。

```
客户端 → POST N 字节 (Content-Type: octet-stream)
Thunder → /hello/raw → ModuleRaw → SendToClientFast
Nginx   → /          → return 200
```

两端均不解析 body 内容，无 JSON 解析、动态构造、文件 IO、sendfile。

---

## HTTP 基准

| Body | ev | native_uring | asio_uring | Nginx 1w |
|-----:|----:|----:|----:|----:|
| 64 B | 183k / 214μs | 180k / 198μs | 193k / 254μs | **252k / 392μs** |
| 1 KB | 143k / 485μs | 214k / 218μs | 182k / 289μs | **256k / 386μs** |
| 4 KB | 153k / 238μs | 217k / 222μs | **221k / 201μs** | 253k / 392μs |
| 16 KB | 206k / 249μs | 203k / 189μs | 203k / 225μs | **256k / 387μs** |
| 64 KB | 193k / 198μs | 189k / 207μs | 216k / 216μs | **256k / 388μs** |

**分析:**

- Nginx 全场景吞吐领先 (252-256k)，且不受请求 body 大小影响
- Thunder 三后端在 143k-221k 范围，P50 延迟整体优于 Nginx (198-485μs vs 386-392μs)
- asio_uring 在 4KB 拉近差距至 12%，native_uring 在 1KB/4KB 表现最佳
- ev 在 1KB/4KB 吞吐偏低 (143k/153k)，大包恢复 (206k/193k)

## HTTPS (SSL) 基准

| Body | ev | native_uring | asio_uring | Nginx SSL 1w |
|-----:|----:|----:|----:|----:|
| 64 B | 74k / 0.95ms | 101k / 716μs | **142k / 323μs** | 161k / 581μs |
| 1 KB | 109k / 364μs | 144k / 339μs | 137k / 355μs | **165k / 567μs** |
| 4 KB | 112k / 353μs | 138k / 372μs | 113k / 334μs | **167k / 568μs** |
| 16 KB | 74k / 1.0ms | 131k / 372μs | 118k / 345μs | **168k / 567μs** |
| 64 KB | 108k / 347μs | **146k / 327μs** | 135k / 324μs | 167k / 568μs |

**分析:**

- Nginx SSL 全场景稳定 ~165k，不受请求 body 影响
- asio_uring 64B 与 Nginx 仅差 12% (142k vs 161k)，P50 占优 (323μs vs 581μs)
- native_uring 64KB 差距最小 (146k vs 167k, -12%)
- ev 是三个后端中最弱的，64B/16KB 只有 74k
- 三后端 P50 均优于 Nginx，ev 除外 (16KB 1.0ms)

## HTTP → HTTPS 衰减

| Body | ev | native_uring | asio_uring | Nginx |
|-----:|----:|----:|----:|----:|
| 64 B | -59% | -44% | -26% | -36% |
| 1 KB | -24% | -33% | -25% | -36% |
| 4 KB | -27% | -36% | -49% | -34% |
| 16 KB | -64% | -35% | -42% | -34% |
| 64 KB | -44% | -23% | -38% | -35% |

Nginx SSL 衰减稳定在 34-36%。Thunder 各后端表现不一: asio_uring 小包衰减最小 (-26%), native_uring 64KB 衰减最小 (-23%), ev 波动最大 (-24~-64%)。

---

## 选型

| 场景 | 推荐 | 原因 |
|------|:---:|------|
| HTTP 低延迟 | **Thunder** | 三后端 P50 均优于 Nginx |
| HTTP 高吞吐 | Nginx | 稳定 253k, 领先 15-77% |
| HTTPS 小包 | **asio_uring** | 142k, P50 323μs, 距 Nginx 仅 12% |
| HTTPS 大包 | **native_uring** | 146k, 距 Nginx 12% |
| 需要热更新/多协议/灰度 | **Thunder** | Nginx 不具备 |

---

> ⚠ 单机压测。真正网卡对比需两台独立机器。
