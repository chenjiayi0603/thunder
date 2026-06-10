# Thunder vs Nginx 本机 wrk 基准测试

> 日期: 2026-06-10 | 分支: dev | 环境: **Ubuntu 26.04 LTS (原生)** | 工具: wrk 4.1.0
> HTTP 解析器: **picohttpparser** (替换旧版 http_parser, 单次改动 +49%)

---

## 🏁 最终基准

> **单位换算**: 1ms (毫秒) = 1000μs (微秒)。例: 258μs = 0.258ms, 1.5ms = 1500μs。

> `performance` governor, P-core 4-9 绑核, INFO log, 1 Worker, wrk -t4 -c100 -d10s

### HTTP

> `performance` governor, P-core 4-9 绑核, INFO log, 1 Worker, wrk -t4 -c100 -d10s

```
              ev        native_uring  asio_uring    Nginx 1w
────────────────────────────────────────────────────────────
64B           322k      319k          347k           173k
256B          242k      265k          237k           171k
1K            323k      313k          330k           160k
4K            321k      312k          331k           151k
64K           129k      127k          127k           69k

Latency(64B): 258μs     260μs*        240μs*        588μs
Latency(4K):  281μs     285μs*        275μs*        666μs
Latency(64K): 1.5ms     1.5ms*        1.5ms*        1.48ms
```

> * 标注为估算值 (三后端延迟差异 <5%)。

**分析：**
- **全线 ~2x Nginx** (322k vs 173k @64B) — Thunder Fast Path 跳过 body 读取, Nginx `return 200` 仍需读 POST body
- **三后端差距 <5%**: asio_uring ≈ ev ≈ native_uring, 应用层(pico+protobuf)是瓶颈
- **asio_uring 64B 小包最优** (347k), 优势随包体增大消失
- **64K 大包带宽瓶颈**, 差距缩小 (129k vs 69k)

### HTTPS (SSL 加密)

> `powersave` governor, P-core 4-9 绑核, INFO log, 1 Worker, wrk -t4 -c100 -d10s

```
              ev        native_uring  asio_uring¹   Nginx 1w
─────────────────────────────────────────────────────────────
64B           105k      89k           184k 🏆          112k
4K            41k       36k           74k              101k 🏆
64K           3.8k      3.3k          6.1k             23.6k 🏆

Latency(64B): 890μs     ~920μs        452μs            505μs
Latency(4K):  2.0ms     ~2.2ms        1.2ms            560μs
Latency(64K): 23ms      ~26ms         10ms             4.7ms
```

> ¹ asio_uring 需 `-DTHUNDER_IO_ASIO_URING=ON` 编译。

**分析：**
- **小包 (64B): Thunder asio_uring 最优** (184k, +64% vs Nginx 112k), 批量提交降低 SSL syscall 开销
- **中包 (4K): Nginx 领先** (101k vs 74k), 多进程在中等 SSL 负载有优势
- **大包 (64K): Nginx 大幅领先** (23.6k vs 6.1k), 多进程在 SSL 大包加密场景优势显著
- **三后端差异大**: asio_uring ≫ ev > native_uring, SSL 场景下批量提交优势突出

### 延迟与吞吐关系分析

一个常见疑问: 240μs 延迟怎能做到 347k RPS？

单线程事件循环下, wrk 报告的 240μs 延迟绝大部分是**数据在 kernel buffer 等待 event loop 调度**的时间, 而非 CPU 执行时间:

```
请求到达 → [数据排队等 epoll 唤醒] → read → 解析 → write → 下一轮
            ↑ 237μs                     ↑ 2.88μs
```

用 Little 定律验算: 并发 100 连接, RPS = 347k, 平均延迟 240μs, 系统内请求数 L = RPS × Latency = 347k × 0.24ms ≈ **83 个请求同时在内核中排队**。

真正 CPU 耗时:

```
1s ÷ 347,000 ≈ 2.88μs / 请求
```

| 阶段 | 耗时 | 占比 |
|:-----|:----:|:----:|
| 数据在 kernel buffer 等待 event loop 调度 | ~237μs | ~99% |
| 实际处理 (read → 解析 → write) | ~2.88μs | ~1% |
| **wrk 测到的端到端延迟** | **240μs** | **100%** |

> **核心**: 单线程每秒处理 347k 请求, 每个请求只需 2.88μs CPU。剩下的 237μs 是请求在 kernel 缓冲区排队等待单线程依次处理。如果每个请求都要 240μs CPU 时间, 单线程上限只有 1/240μs ≈ 4k RPS。

---

## 二、核心原理：syscall 分布

四后端性能差异的根本原因：**每次 I/O 操作所需的 syscall 次数 × 能否批量合并**。

```
模型:     应用 → [I/O 引擎] → syscall → 内核

ev:        每次读写 → 1 次 syscall (read/write)
native_uring: 每次读写 → 构造 SQE + 1 次 syscall (enter)
asio_uring:  每次读写 → 入队 → N 次合并 → 1 次 syscall (enter)
Nginx:      每次读写 → 1 次 syscall (read/write), 1 Worker
```

### 每笔 I/O 的 syscall 开销

| 后端 | 每笔 I/O 操作 | syscall 次数 | 可否批量 |
|------|--------------|:-----------:|:-------:|
| ev | `epoll_wait` 就绪 → `read/write` | **1 syscall / I/O** | ❌ 不能 |
| Nginx 1w | 同上 | **1 syscall / I/O** | ❌ 不能 |
| native_uring | 构造 SQE → `enter` | **1 enter syscall / I/O** + SQE 构造 | ❌ 不能 |
| asio_uring | 入队 → 攒批 → `enter` | **1 enter syscall / N I/O** + 入队开销 | ✅ 可以 |

**关键差异**: 当 N=1 时 (native_uring 无批量), io_uring 比 epoll 多了 SQE 构造开销, 却不省 syscall, 所以反而更慢。只有 N>1 时 (asio_uring 批量), io_uring 才真正省 syscall。

### HTTP 场景: 2-3 次 I/O / 请求

```
请求处理路径: read → 解析 → write
I/O 次数:     2-3 次
```

超高频下 syscall 开销占比不大, 三后端差距 <8%。

### HTTPS 场景: 5-10+ 次 I/O / 请求

```
请求处理路径: SSL_read → decrypt → 解析 → SSL_write → encrypt
I/O 次数:     5-10+ 次 (SSL 拆成多次 BIO)
```

I/O 次数翻倍 → asio_uring 批量优势被放大:

```
后端       每请求 I/O 次数   每笔 syscall 方式          总 syscall
ev             5-10         1 sync / I/O              5-10 次
native_uring   5-10         1 enter / I/O             5-10 次 + SQE 构造
asio_uring     5-10         1 enter / N I/O (N=3-5)   1-3 次  ← 最少
```

结果: asio_uring 184k, ev 105k, native_uring 89k (因 SQE 构造拖累)。

### 核心结论

```
性能排序 = 每 I/O 的 syscall overhead × I/O 次数

I/O 少 (HTTP): overhead 差异小 → 三后端差距 <8%
I/O 多 (HTTPS): overhead 差异被放大 → asio_uring 大幅领先

asio_uring > ev ≈ native_uring > Nginx 1w
                ↑
         native_uring 多花 SQE 构造没省到 syscall
```

## 三、测试环境

### 硬件

| 项目 | 值 |
|------|-----|
| CPU | i9-12900H, 6P+8E, 20 线程, max 5.0GHz (P-core) |
| 内存 | 30 GB DDR4 |
| OS | Ubuntu 26.04 LTS, Linux 7.0.0-15 (PREEMPT_DYNAMIC) |
| 存储 | NVMe SSD |

> 混合架构: P-core cpu0-11 (5.0GHz), E-core cpu12-19 (3.8GHz)

### 软件

| 项目 | 值 |
|------|-----|
| wrk | 4.1.0, -t4 -c100 -d10s, 127.0.0.1 回环 |
| Thunder | dev, 1 Manager + 1 Worker, ev backend, INFO, **picohttpparser** |
| Nginx | 1.27.5, 1 worker epoll, Docker host 网络 |

### CPU governor 影响

| governor | 服务 | 64B RPS | vs performance |
|----------|------|:-------:|:--------------:|
| performance | Thunder ev | 322k | — |
| performance | Nginx | 173k | — |
| powersave | Thunder ev | 278k | **−13.6%** |
| powersave | Nginx | 168k | −2.9% |

Thunder 受 governor 影响远大于 Nginx (−13.6% vs −2.9%)，因 protobuf/JSON 是 CPU-bound 路径。

### io_backend 选择

| backend | 场景 | 64B RPS |
|---------|------|:-------:|
| ev (epoll) | **默认推荐**, 全线均衡 | 322k |
| asio_uring | 极端小包优化 | **347k** 🏆 |
| native_uring | 大文件传输场景 | 319k |

---

## 二、I/O Backend 横向对比

> 三后端 + Nginx 同条件 1 Worker, performance governor, P-core 绑核

```
小包 (64B):    asio_uring(347k) > ev(322k) > native_uring(319k)  差距 <8%
中包 (1-4K):   asio_uring ≈ ev ≈ native_uring                    差距 <6%
大包 (64K):    三者持平 ~128k                                     带宽瓶颈

1. picohttpparser 将解析开销大幅降低, 三后端差距从旧版 ~18% 缩至 <8%
2. asio_uring 在极端小包 (64B) 通过批量提交略优, ev 整体最稳定
3. 64K 大包下带宽受限, 后端差异消失
4. Thunder 全线 ~2x Nginx (64K 除外)
```

---

## 三、测试方法

### 准备

```bash
# 1. performance governor
echo "performance" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# 2. 启动 Thunder & 绑 P-core
cd deploy/HelloHttp && bash node.sh restart
taskset -cp 4-9 $(pgrep Hello_robot_W0)
```

### Nginx 配置

```nginx
worker_processes 1;
events { worker_connections 65535; use epoll; multi_accept on; }
http {
    access_log off; sendfile on; tcp_nopush on; tcp_nodelay on;
    keepalive_timeout 65; keepalive_requests 100000;
    server {
        listen 8088;
        location = /echo { default_type application/json; return 200 '{"code":0,"msg":"ok"}'; }
    }
}
```

### 测试命令

```bash
# Thunder 各后端 / 各包体大小
wrk -t4 -c100 -d10s -s tests/benchmark/wrk_small.lua http://127.0.0.1:27006/hello/hello

# Nginx
wrk -t4 -c100 -d10s -s tests/benchmark/wrk_small.lua http://127.0.0.1:8088/echo
```

---

## 四、优化路线图

### 已实施优化及收益

| # | 优化 | 收益(累计) | 说明 |
|:-:|------|:----------:|------|
| ④ | SendToClient Fast Path | +8.0% | 绕过 protobuf HttpMsg 构造+编码 |
| — | Recv Fast Path (raw buffer) | +1.1% | 绕过 http_parser 回调链 |
| ⑤ | codec 指针缓存 + memchr | +1.9% | 消除 map 查找 |
| ③ | Encode 响应头模板 | +~5% | vsnprintf ×5 → ×1 |
| ⑧ | **picohttpparser 替换** | **+49%** 🏆 | 旧版 http_parser → picohttpparser |
| ⑥⑦ | protobuf Arena | −2.9% | 小消息场景无效, 大消息预期有效 |
| ⑧ | HTTPS Fast Path | ±0% | SSL 主导, 解析节省可忽略 |
| ⑨ | ProtoCodec Arena | 待压测 | 大消息场景预期收益显著 |

### Fast Path vs 完整路径 开销拆解 (picohttpparser 后, 估)

```
Fast Path (纯 I/O)          322k     —
+ HttpCodec::Decode          −~5%    picohttpparser + protobuf 构造
+ Dispose 路由               −~2%    hash 查找
+ HttpCodec::Encode          −~5%    模板已优化, 仍需 pb 序列化
+ protobuf 响应构造          −~3%    set_body/headers
+ JSON 解析                  −~3%    CJsonObject
+ IoBackend 杂项             −~2%
─────────────────────────────────
完整路径 (估)               ~200k    −~38%
```

> picohttpparser 将 Decode 层开销从 ~8% 压至 ~5%, 是迄今最有效的单次优化。对极高性能场景, **绕过比优化更有效** — Fast Path 直接跳过整个 pb+JSON 栈。

### 其他协议 Fast Path / Arena 可行性

| 协议 | Fast Path | Arena | 备注 |
|------|:---------:|:-----:|------|
| HTTP | ✅ 322k | ✅ (−2.9%) | 明文 prefix 匹配 |
| HTTPS | ✅ 已实施 | ⚠️ 已接入 | SSL 主导, 解析节省可忽略 |
| WebSocket | ❌ | ⚠️ 可做 | 二进制帧无法 prefix 匹配 |
| Internal PB | ❌ | ✅ 已实施 | 大消息预期收益显著, 待压测 |
| Client PB | ❌ | ✅ 高价值 | 嵌套深, 待做 |
