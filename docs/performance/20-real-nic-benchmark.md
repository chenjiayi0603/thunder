# Thunder vs Nginx：性能对比

> 日期: 2026-07-31 | 环境: Ubuntu 26.04 LTS | 工具: wrk 4.1.0
> 网卡: 192.168.3.61 (enp0s31f6, 1GbE) | Hello 构建时间: Jul 31 14:04

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

**公平对比** — 两端行为完全对等：POST 变长二进制 body (不解析)，返回固定 24B JSON。

```
客户端 → POST N 字节随机二进制 (Content-Type: octet-stream)
Thunder → /hello/raw → ModuleRaw → SendToClientFast → {"code":0,"msg":"ok"}
Nginx   → /          → return 200 '{"code":0,"msg":"ok"}'
```

请求 body 仅改变 TCP 段大小/分片行为，两端均不解析 body 内容。
**不含 JSON 解析、动态构造、文件 IO、sendfile 等不对等因素。**

---

## HTTP 基准

| Body | Thunder (asio_uring) | Nginx 1w | |
|-----:|-----:|-----:|:--:|
| 64 B | 193k RPS / 254μs | 246k RPS / 395μs | Nginx +27% |
| 1 KB | 182k RPS / 289μs | 212k RPS / 447μs | Nginx +16% |
| 4 KB | 221k RPS / 201μs | 189k RPS / 492μs | **Thunder +17%** |
| 16 KB | 116k RPS / 291μs | 70k RPS / 1.4ms | **Thunder +66%** |
| 64 KB | 53k RPS / 755μs | 37k RPS / 2.7ms | **Thunder +44%** |

**分析:**

- 小包 (≤1KB): Nginx 领先 16-27%，源于更轻量的 HTTP 状态机
- 大包 (≥4KB): Thunder 反超，4KB +17%，16KB +66%，64KB +44%
- Thunder P50 延迟全场景优于 Nginx，大包优势尤为明显 (755μs vs 2.7ms)

## HTTPS (SSL) 基准

| Body | Thunder (asio_uring) | Nginx SSL 1w | |
|-----:|-----:|-----:|:--:|
| 64 B | 116k RPS / 366μs | 165k RPS / 574μs | Nginx +42% |
| 1 KB | 95k RPS / 327μs | 156k RPS / 611μs | Nginx +64% |
| 4 KB | 90k RPS / 486μs | 125k RPS / 698μs | Nginx +39% |
| 16 KB | 29k RPS / 1.9ms | 75k RPS / 1.3ms | Nginx +155% |
| 64 KB | 8.9k RPS / 5.8ms | 25k RPS / 3.6ms | Nginx +180% |

**分析:**

- Nginx SSL 全场景领先，大包 +155-180%
- Thunder SSL 衰减严重: HTTP→HTTPS 吞吐降 40-80%
- Nginx SSL 衰减平稳: HTTP→HTTPS 降 30-35%
- Thunder HTTPS 大包 (≥16KB) 吞吐骤降，疑似 SSL record 拼包/缓冲策略差异

## HTTP → HTTPS 衰减

| Body | Thunder | Nginx |
|-----:|-----:|-----:|
| 64 B | 193k → 116k (-40%) | 246k → 165k (-33%) |
| 1 KB | 182k → 95k (-48%) | 212k → 156k (-26%) |
| 4 KB | 221k → 90k (-59%) | 189k → 125k (-34%) |
| 16 KB | 116k → 29k (-75%) | 70k → 75k (+7%) |
| 64 KB | 53k → 8.9k (-83%) | 37k → 25k (-33%) |

Nginx SSL 衰减稳定在 30-35%；Thunder 从 4KB 起急剧恶化。

> ⚠ Thunder SSL 大包衰减根因待 profiling (session 复用 / BIO 阻塞 / TLS record 策略)。

---

## 选型

| 场景 | 推荐 | 原因 |
|------|:---:|------|
| HTTP 小包 (≤1KB) | Nginx | 轻量 HTTP 状态机 +17-27% |
| HTTP 大包 (≥4KB) | **Thunder** | 吞吐 +17-66%，延迟 2-3x 优于 Nginx |
| HTTPS 任意大小 | Nginx | SSL 路径成熟，Thunder 大包衰减严重 |
| 需要热更新/多协议/灰度 | **Thunder** | Nginx 不具备这些能力 |

---

## Nginx 为什么更快 (小包 HTTP)

```
Thunder /hello/raw:

  epoll_wait → readv → HttpCodec::Decode (picohttpparser + HttpMsg proto)
    → SerializeAsString → MsgBody → ParseFromString → Dispose(HttpMsg)
      → mapModule.find → ModuleRaw::AnyMessage (虚函数)
        → SendToClientFast → pSendBuff::Write → FlushSendBuf → ::send


Nginx /:

  epoll_wait → recv → HTTP 状态机 (原地, 零分配) → location 匹配
    → writev(fd, iovec, 2)
```

差距来自 Thunder 多协议网关框架的固定开销：proto 序列化/反序列化 + 模块分发虚函数。
每层纳秒级，在 200k+ RPS 下累计可测。换来了热更新 .so 零停机、多协议、灰度等 Nginx 做不到的能力。

---

> ⚠ 单机压测。真正网卡对比需两台独立机器。
> ev / native_uring 后端数据待补 (当前仅 asio_uring)。
