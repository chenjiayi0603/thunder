# Thunder vs Nginx 基准测试 (loopback)

> ⚠ **本测试为 loopback (127.0.0.1) 回环数据，非真实网卡。** 真实网卡数据见 [`20-real-nic-benchmark.md`](20-real-nic-benchmark.md)
>
> 日期: 2026-06-12 | 环境: Ubuntu 26.04 LTS, Linux 7.0 | 工具: wrk 4.1.0
> HTTP 解析器: picohttpparser (替换旧 http_parser, +49%)

---

## 测试环境

| 项目 | 值 |
|------|-----|
| CPU | i9-12900H, 6P+8E, 20 线程, P-core max 5.0GHz, **45W 散热稳态 ≈ 4.0GHz** |
| 内存 | 30 GB DDR4 |
| OS | Ubuntu 26.04 LTS, Linux 7.0.0-22 |
| governor | performance |
| 绑核 | 服务绑 P-core 4-9, wrk 绑 E-core 12-19 |
| log | INFO |
| Worker | 1 |

### 测试命令

```bash
# Thunder
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
taskset -c 12-19 wrk -t4 -c100 -d10s -s wrk_64b.lua http://127.0.0.1:27006/hello/raw

# Nginx (1 worker, epoll, return 200, access_log off, 绑 4-9)
taskset -c 12-19 wrk -t4 -c100 -d10s -s wrk_64b.lua http://127.0.0.1:8088/echo
```

---

## HTTP 基准

> 端点 `/hello/raw` (Receive Fast-Path)

```
              ev        native_uring  asio_uring    Nginx 1w
────────────────────────────────────────────────────────────
64B           232k      203k          235k           214k
256B          230k      201k          236k           212k
1K            229k      200k          232k           191k
4K            216k      190k          223k           184k
64K           69k       47k           48k ⚠          81k

Latency(64B): 424μs     427μs         220μs          466μs
Latency(4K):  457μs     440μs         332μs          543μs
Latency(64K): 1.44ms    1.42ms        18.6ms ⚠       1.22ms
```

**分析:**
- 小中包 Thunder 领先 Nginx ~9-10%（64B: asio 235k vs nginx 214k）
- asio_uring 延迟优势显著：64B 220μs vs ev 424μs（批量提交降低排队）
- asio_uring 64K 异常 ⚠：48k + 18.6ms 延迟，大包劣化为遗留问题
- 64K Nginx 领先（81k vs 69k）：内核 sendfile/缓冲路径优势
- native_uring 全线最弱（−13%）：批量=1 时 SQE 构造是纯开销

---

## HTTPS (SSL) 基准

> 端点 `/hello/raw`, 同证书同绑核

```
              ev        native_uring  asio_uring    Nginx-ssl 1w
─────────────────────────────────────────────────────────────
64B           141k      133k          133k           149k
4K            97k       95k           92k            132k
64K           14.2k     15.6k         15.3k          32.2k

Latency(64B): 803μs     394μs         402μs          752μs
Latency(4K):  1.23ms    218μs         247μs          824μs
Latency(64K): 7.27ms    662μs         667μs          3.14ms
```

**分析:**
- 三后端吞吐基本持平（64B: 133~141k，差距 <6%）— SSL 加解密是 CPU 瓶颈
- uring 系延迟显著占优：64K 0.66ms vs ev 7.27ms（11 倍）
- Nginx HTTPS 全线领先（64B +6%, 4K +36%, 64K +2.3 倍）

---

## 延迟与吞吐关系

220μs 延迟如何做到 235k RPS？用 Little 定律验算：

```
请求到达 → [数据排队等 event loop] → read → 解析 → write → 下一轮
            ↑ ~216μs                  ↑ ~4.3μs
```

| 阶段 | 耗时 | 占比 |
|:-----|:----:|:----:|
| kernel buffer 排队等调度 | ~216μs | ~98% |
| 实际处理 (read→解析→write) | ~4.3μs | ~2% |
| wrk 端到端延迟 | 220μs | 100% |

L = RPS × Latency = 235k × 0.22ms ≈ **52 个请求在内核排队**。ev 424μs vs asio 220μs 的差距在排队深度（ev 逐个 syscall，asio 批量收割），不是 CPU 路径差异。

---

## syscall 分布

四后端性能差异根本原因：每次 I/O 的 syscall 次数 × 能否批量合并。

```
ev:           每次读写 → 1 次 syscall (read/write)
native_uring: 每次读写 → 构造 SQE + 1 次 syscall (enter)
asio_uring:   每次读写 → 入队 → N 次合并 → 1 次 syscall (enter)
Nginx:        每次读写 → 1 次 syscall (read/write)
```

| 后端 | 每笔 I/O | syscall 次数 | 可否批量 |
|------|---------|:-----------:|:-------:|
| ev | epoll_wait → read/write | 1 / I/O | ❌ |
| Nginx 1w | 同上 | 1 / I/O | ❌ |
| native_uring | 构造 SQE → enter | 1 enter / I/O + SQE 构造 | ❌ |
| asio_uring | 入队 → 攒批 → enter | 1 enter / N I/O + 入队开销 | ✅ |

### HTTP 场景 (2-3 次 I/O/请求)

吞吐 asio_uring ≈ ev（应用层是瓶颈），批量收割显著降低排队延迟（220μs vs 424μs）。

### HTTPS 场景 (5-10+ 次 I/O/请求)

吞吐三后端持平（SSL 加解密是 CPU 瓶颈），批量优势体现在延迟（uring 系 64K 0.66ms vs ev 7.27ms）。

---

## 选型结论

```
HTTP  吞吐: asio_uring ≈ ev > Nginx > native_uring
HTTPS 吞吐: Nginx > ev ≈ native_uring ≈ asio_uring
延迟:       uring 系 ≪ ev ≈ Nginx

ev:      默认推荐，吞吐均衡稳定
uring:   延迟敏感场景（HTTP asio_uring 64K 劣化待修）
Nginx:   HTTPS 大包 + 内核路径成熟度
```
