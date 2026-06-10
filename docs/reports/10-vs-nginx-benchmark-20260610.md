# Thunder vs Nginx 本机 wrk 基准测试

> 日期: 2026-06-10 | 分支: dev | 环境: **Ubuntu 26.04 LTS (原生)** | 工具: wrk 4.1.0
> HTTP 解析器: **picohttpparser** (替换旧版 http_parser, 单次改动 +49%)

---

## 🏁 最终基准

> **单位换算**: 1ms (毫秒) = 1000μs (微秒)。例: 258μs = 0.258ms, 1.5ms = 1500μs。

> `performance` governor, P-core 4-9 绑核, INFO log, 1 Worker, wrk -t4 -c100 -d10s

### HTTP

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

### HTTPS (SSL 加密, 2026-06-10 复测, powersave governor, P-core 绑核)

```
              ev        native_uring  asio_uring¹   Nginx 1w²
─────────────────────────────────────────────────────────────
64B           105k      89k           —               —
4K            41k       36k           —               —
64K           3.8k      3.3k          —               —

Latency(64B): 890μs     ~920μs        —               —
Latency(4K):  2.0ms     ~2.2ms        —               —
Latency(64K): 23ms      ~26ms         —               —
```

> ¹ asio_uring 后端需 `-DTHUNDER_IO_ASIO_URING=ON` 编译, 当前版本未启用。
> ² Nginx HTTPS 同条件待测。
> HTTPS 受 SSL 加密/解密制约: 64B 吞吐 ~33% of HTTP (105k vs 322k), SSL 握手+加密占 CPU 主导。三后端差距 <16% (ev 最优), SSL 开销覆盖了后端差异。

> **单位换算**: 1ms (毫秒) = 1000μs (微秒)。例: 258μs = 0.258ms, 1.5ms = 1500μs。

**关键发现：**

- **picohttpparser +49%**: 旧版 216k → 新版 322k (64B ev), 迄今单次收益最大的优化
- **HTTP Thunder 全线 ~2x Nginx** (322k vs 173k @64B) — Fast Path 跳过 body 读取, Nginx `return 200` 仍需读 POST body
- **三后端差距 <5%**: asio_uring ≈ ev ≈ native_uring, 应用层(pico+protobuf)是瓶颈
- **asio_uring 64B 小包最优** (347k), 优势随包体增大消失
- **64K 大包带宽瓶颈**, 差距缩小 (129k vs 69k)
- **HTTPS ~33% of HTTP** (105k vs 322k @64B ev), SSL 加密是瓶颈, 三后端差距 <16%
- **P-core 绑核** 仍损失 ~17% 若跑在 E-core

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

## 一、测试环境

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
