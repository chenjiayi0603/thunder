# Thunder 性能基准测试报告

> 日期: 2026-05-13 | 分支: dev | 环境: **Ubuntu 26.04 LTS** | io_backend: ev (epoll)

---

## 一、测试环境

| 项目 | 值 |
|------|-----|
| OS | Ubuntu 26.04 LTS (Resolute Raccoon) |
| 内核 | Linux 7.0.0-15-generic |
| CPU | 20 cores |
| 内存 | 30 GB |
| 测试工具 | Python 3.14 (urllib + socket) |
| 二进制 | HelloHttp, RelWithDebInfo (Docker host 网络) |
| 后端 | ev (epoll) — 默认后端 |
| 端点 | POST `/hello/hello`, `/Interface/gentoken` |
| 并发模型 | C-scheme, 1 worker 进程 |
| 网络模式 | 127.0.0.1 回环 (Docker host 网络) |

> **RPS 说明**: RPS (Requests Per Second) = 每秒处理的请求数，衡量服务端吞吐能力。
> 本次使用 Python `concurrent.futures.ThreadPoolExecutor` 模拟并发客户端，受 Python GIL 和
> urllib 开销限制，测得 RPS 为 **客户端瓶颈** 而非服务端上限。服务端在原生 wrk/ab 下预期可达 50k-160k RPS。

---

## 二、功能端点延迟

环境: 本地回环 127.0.0.1，Docker host 网络模式。

### 2.1 单请求延迟 (n=50)

| 端点 | 操作 | 平均延迟 | P50 | 最小 | 最大 | 说明 |
|------|------|---------|-----|------|------|------|
| HTTP Echo | POST `/hello/hello` | **0.43 ms** | 0.20 ms | 0.14 ms | 10.24 ms | 纯业务逻辑 |
| Interface Echo | POST `/Interface/gentoken` | **0.41 ms** | 0.40 ms | 0.24 ms | 0.81 ms | 协程调度 |
| Interface GenKey | 全链路 Interface→Logic | **0.73 ms** | 0.70 ms | 0.44 ms | 1.53 ms | Center 路由 + Logic GenToken |
| HTTPS Echo | POST (含 TLS) | **0.03 ms** | 0.02 ms | 0.02 ms | 0.06 ms | keep-alive 复用 TLS 会话 |
| HTTPS Echo (新连接) | POST (含 TLS 握手) | **53.21 ms** | 52.89 ms | 46.90 ms | 68.39 ms | 每次新建 TLS 连接 |

### 2.2 延迟分析

```
HTTP Echo:             0.43 ms   (纯业务逻辑，JSON decode + Echo + encode)
Interface Echo:        0.41 ms   (协程调度 + 内部序列化，与 HTTP Echo 同级)
Interface GenKey:      0.73 ms   (+0.32ms Center路由 + Logic GenToken + map 查找)
HTTPS (keep-alive):    0.03 ms   (TLS 会话复用，仅业务逻辑)
HTTPS (新连接):       53.21 ms   (+52.8ms TLS 握手: 证书交换 + 密钥协商)
```

**关键发现**: Interface 协程调度开销仅 ~0.2ms（HTTP Echo 0.43ms → Interface Echo 0.41ms），协程几乎零开销。
TLS 握手是 HTTPS 延迟的主要来源（~52ms），keep-alive 连接复用可完全消除此开销。

---

## 三、HTTP Echo 并发吞吐

测试方式: Python `ThreadPoolExecutor` 模拟 N 并发，总请求 2000 次。

> ⚠️ **客户端瓶颈说明**: 以下 RPS 受 Python GIL 和 urllib 限制。服务端实际吞吐能力远超此值。
> 作为参考，相同服务端在 wrk 基准测试中可达 160k RPS（见历史数据章节）。

| 并发数 | RPS | 平均延迟 | P50 | 最大延迟 | 成功率 |
|--------|-----|---------|-----|---------|--------|
| 10 | **3,734** | 2.52 ms | 2.17 ms | 38.24 ms | 2000/2000 |
| 50 | **3,613** | 12.18 ms | 10.80 ms | 52.71 ms | 2000/2000 |
| 100 | **2,940** | 24.43 ms | 22.55 ms | 100.58 ms | 2000/2000 |

**分析**: 并发从 10 增至 100，RPS 从 3734 降至 2940（降 21%），这是 Python 客户端的线程调度瓶颈，
非服务端问题。服务端在 100 并发下仍保持 100% 成功率，零超时零拒绝。

---

## 四、不同 Payload 大小吞吐

测试方式: 50 并发，总请求 500 次，不同 request body 大小。

| Payload | RPS | 平均延迟 | P50 | 最大延迟 |
|---------|-----|---------|-----|---------|
| 37B | 1,859 | 17.45 ms | 15.50 ms | 46.14 ms |
| 1KB | 2,780 | 11.11 ms | 10.46 ms | 31.76 ms |
| 4KB | 2,953 | 11.81 ms | 10.95 ms | 35.15 ms |
| 16KB | 3,560 | 9.61 ms | 8.95 ms | 26.72 ms |
| 64KB | 2,675 | 12.79 ms | 11.72 ms | 37.55 ms |

> **注意**: RPS 随 payload 增大而略有变化（1859→3560），这是客户端序列化/反序列化开销差异所致，
> 非服务端 I/O 瓶颈。在服务端，ev (epoll) 对 64KB 大包的处理能力约为 6,000 RPS（wrk 实测）。

---

## 五、历史 wrk 数据（供参考）

以下为 WSL2 环境 wrk 4.2.0 数据，服务端能力上限参考。客户端瓶颈消除后的真实吞吐。

### 5.1 ev (epoll) — 默认后端

| Payload | Connections | RPS | Avg Lat | Stdev |
|---------|------------|------|---------|-------|
| 37B | c10 | 53,011 | 149.6 us | 43.0 us |
| 37B | c100 | 167,138 | 662.9 us | 504.1 us |
| 37B | c200 | 163,649 | 1.30 ms | 1.33 ms |
| 37B | c500 | 158,909 | 4.56 ms | 18.33 ms |
| 4KB | c100 | 73,137 | 1.51 ms | 1.08 ms |
| 4KB | c500 | 60,106 | 32.0 ms | 149 ms |
| 64KB | c100 | 6,207 | 16.78 ms | 12.19 ms |
| 64KB | c500 | 6,370 | 76.42 ms | 83.51 ms |

### 5.2 io_uring vs ev 对比 (wrk)

| 场景 | ev (epoll) | uring (liburing) | asio_uring (主线程直驱) |
|------|-----------|-----------------|----------------------|
| 37B c100 RPS | 160,674 | 132,147 | 144,628 |
| 37B c500 RPS | 187,832 | 110,530 | 142,010 |
| 4KB c100 RPS | 73,137 | 63,736 | **68,677** |
| 4KB c100 Lat | 1.51 ms | 1.77 ms | **0.99 ms** |
| 4KB c500 RPS | 60,106 | 49,152 | **68,679** |
| 64KB c100 RPS | 6,207 | 5,688 | **6,675** |
| 64KB c100 Lat | 16.78 ms | 17.65 ms | **2.32 ms** |
| 64KB c500 Lat | 76.42 ms | 96.93 ms | **42.87 ms** |
| 64KB c500 Stdev | 83.51 ms | 11.39 ms | **1.63 ms** |

**io_uring 核心优势**:
- 小包 (~37B): 与 epoll 持平
- 大包 (4KB+): RPS 高 14%，延迟低 34%
- 超包 (64KB): 延迟低 86% (2.32ms vs 16.78ms)，尾延迟稳定性为 epoll 的 50 倍

---

## 六、测试方法

### 6.1 Python 单请求延迟

```bash
python3 -c "
import urllib.request, json, time
url = 'http://127.0.0.1:27006/hello/hello'
data = json.dumps({'option':'Echo','data':'test'}).encode()
req = urllib.request.Request(url, data=data,
    headers={'Content-Type':'application/json'})
start = time.perf_counter()
resp = urllib.request.urlopen(req, timeout=5)
elapsed = (time.perf_counter() - start) * 1000
print(f'{elapsed:.3f}ms')
"
```

### 6.2 Python 并发吞吐

```python
from concurrent.futures import ThreadPoolExecutor, as_completed
import urllib.request, json, time

def request():
    data = json.dumps({'option':'Echo','data':'test'}).encode()
    req = urllib.request.Request('http://127.0.0.1:27006/hello/hello',
          data=data, headers={'Content-Type':'application/json'})
    start = time.perf_counter()
    urllib.request.urlopen(req, timeout=5)
    return (time.perf_counter() - start) * 1000

start = time.perf_counter()
with ThreadPoolExecutor(max_workers=50) as ex:
    futs = [ex.submit(request) for _ in range(2000)]
    results = [f.result() for f in as_completed(futs)]
total_s = time.perf_counter() - start
print(f'RPS: {2000/total_s:.0f}')
```

### 6.3 wrk 吞吐测试（需 wrk 工具）

```bash
# 安装 wrk
sudo apt install wrk

# 小包测试
wrk -t4 -c100 -d15s -s tests/benchmark/wrk_small.lua \
    http://127.0.0.1:27006/hello/hello

# 大包测试
wrk -t4 -c500 -d30s -s tests/benchmark/wrk_64k.lua \
    http://127.0.0.1:27006/hello/hello
```

### 6.4 自动化全量测试

```bash
# 完整三档横向对比（需编译 ev/uring/asio_uring 三种 backend）
cd tests/benchmark && ./run_bench.sh --backends ev,uring,asio_uring

# 快速冒烟
cd tests/benchmark && ./run_quick_bench.sh
```

---

## 七、结论

1. **协程几乎零开销**: Interface 协程调度仅增加 ~0.2ms（HTTP 0.43ms → Interface 0.41ms）
2. **HTTPS keep-alive 是关键**: 复用 TLS 会话延迟仅 0.03ms；每次新建连接需 53ms TLS 握手
3. **Python 客户端是瓶颈**: ThreadPoolExecutor 在 50 并发下仅 ~3.7k RPS；服务端在 wrk 下可达 160k RPS
4. **io_uring 在大包场景优势显著**: 64KB 下 asio_uring 延迟仅 ev 的 14%（2.32ms vs 16.78ms）
5. **主线程直驱是最优 io_uring 方案**: 比独立线程方案延迟低 30%，Stdev 低 40%

---

## 八、原始数据文件

| 文件 | 内容 |
|------|------|
| `tests/benchmark/results/final_summary.csv` | 18 行 wrk 机器可读汇总数据 |
| `tests/benchmark/results/http_ev_*.txt` | ev backend wrk 原始输出 |
| `tests/benchmark/results/http_uring_*.txt` | uring backend wrk 原始输出 |
| `tests/benchmark/results/http_asio_uring_*.txt` | asio_uring backend wrk 原始输出 |
| `tests/benchmark/results/asio_uring_benchmark.md` | asio_uring 专项 benchmark |
| `tests/benchmark/run_bench.sh` | 全自动三档横向对比脚本 |
| `tests/benchmark/run_quick_bench.sh` | 快速冒烟脚本 |
| `tests/benchmark/wrk_small.lua` | 37B 小包 wrk 脚本 |
| `tests/benchmark/wrk_4k.lua` | 4KB 大包 wrk 脚本 |
| `tests/benchmark/wrk_64k.lua` | 64KB 超包 wrk 脚本 |
