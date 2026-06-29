# Thunder 连接建立压测 — HTTP / WebSocket

> 日期: 2026-06-29 | 环境: Ubuntu 24.04, Intel N100, lo 网卡 | 工具: Python raw socket

## 目的

验证"单机连接建立 5,000/秒"这一性能目标是否合理。

## 方法

8 线程 × 10 秒，每次新建 TCP 连接，发完请求等响应后关闭，统计 QPS。

| 测试 | 目标 | 方法 |
|------|------|------|
| 裸 TCP | Thunder HelloHttp :27006 | connect → close（不走任何协议） |
| HTTP 短连接 | Thunder HelloHttp :27006 | `POST /hello/hello`, Connection: close |
| WS 握手 | Thunder hello_ws :27010 | HTTP Upgrade → 等 101 响应 → close |

## 结果

### 裸 TCP

| 端点 | QPS | 说明 |
|------|:---:|------|
| `127.0.0.1:27006` | **13,422** | Thunder accept() 上空，无协议解析 |

### HTTP 短连接

| 端点 | QPS | vs 裸 TCP |
|------|:---:|:---:|
| `http://127.0.0.1:27006/hello/hello` | **11,070** | -18%（picohttpparser + JSON + protobuf） |

### WS 握手

| 端点 | QPS | vs 裸 TCP |
|------|:---:|:---:|
| `ws://127.0.0.1:27010/hello/shake` | **3,240** | -76%（HTTP Upgrade + SHA1 + Session） |

### 延迟推算

| 场景 | 单连接延迟 | 推算方式 |
|------|:---:|------|
| 裸 TCP | ~0.07ms | 1/13422 |
| HTTP 短连接 | ~0.09ms | 1/11070 |
| WS 握手 | ~0.31ms | 1/3240 |

### 开销分解

```
裸 TCP:     13,422 ─────────────────────────── accept() 上限
HTTP:       11,070 ─── -2352 (-18%) ───────── +picohttpparser + CJsonObject + protobuf
WS:          3,240 ──── -7830 (-71%) ───────── +HTTP Upgrade + SHA1 + Base64 + Session
```

Thunder 事件循环的 accept() 在 lo 网卡上限约 13K，每加一层协议开销递减。WS 建连最重，但实际业务中连接建立是低频操作，高频路径在 keep-alive 复用。

WS 比 HTTP 低 71%，差在 HTTP Upgrade 头解析 + SHA1 + Base64（计算 `Sec-WebSocket-Accept`）。

---

## Keep-Alive 吞吐量（长连接复用）

> wrk -t4 -c100 -d10s, POST + keep-alive, 本机实测。

短连接测**建连速度**（每次新 TCP），keep-alive 测**持续吞吐**（复用连接）。两个维度合起来才是 Thunder 完整的连接能力。

| 维度 | 指标 | 数值 | 说明 |
|------|------|:---:|------|
| 建连速度 | 裸 TCP | 13,422 conn/s | Thunder accept()，无协议 |
| 建连速度 | HTTP 短连接 | 11,070 conn/s | +HTTP 解析 |
| 建连速度 | WS 握手 | 3,240 conn/s | +Upgrade+SHA1+Session |
| **吞吐量** | **HTTP keep-alive** | **71,417 req/s** | wrk POST, 本机实测 |
| 延迟 P50 | keep-alive | 0.76ms | wrk 报告 |
| 延迟 P99 | keep-alive | 26.1ms | wrk 报告 |

```
           短连接 (建连速度)               Keep-Alive (持续吞吐)
           ────────────────────           ────────────────────
裸 TCP      13,422 conn/s                    —
HTTP        11,070 conn/s                    71,417 req/s
WS           3,240 conn/s                    —
```

## 5,000/秒 可行性

| 条件 | QPS | 达到 5K? |
|------|:---:|:---:|
| 笔记本 lo + 裸 TCP | 13,422 | ✅ |
| 笔记本 lo + HTTP 短连接 | 11,070 | ✅ |
| 笔记本 lo + WS 握手 | 3,240 | ❌ 单 Worker |
| 生产 16 核 + 4 Worker | 估算 13,000+ | ✅ |

> 笔记本单 Worker + lo 虚拟网卡，Thunder HTTP 短连接已跑到 11,070（超过 5K），瓶颈不在 Thunder。
> WS 握手单 Worker 3,240，生产 4 Worker 线性扩展即可到 13K。
> lo 虚拟网卡不经过硬件 offload，物理网卡可进一步提升。

## 代码

| 文件 | 用途 |
|------|------|
| `/tmp/thunder_bench.py` | Thunder HTTP + WS 综合压测脚本 |

## 参考

- [10-vs-nginx-benchmark-20260610.md](10-vs-nginx-benchmark-20260610.md) — wrk keep-alive 吞吐量对比
- [11-io-backend-comparison.md](11-io-backend-comparison.md) — IO 后端性能对比
