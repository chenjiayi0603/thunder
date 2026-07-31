# Thunder vs Nginx 基准测试 (loopback)

> ⚠ **本测试为 loopback (127.0.0.1) 回环数据。** 真实网卡数据见 [`20-real-nic-benchmark.md`](20-real-nic-benchmark.md)
>
> ⚠ **测试方法**：Thunder 实际使用 `wrk_echo_*.lua` (POST `/hello/hello` Echo, JSON 解析+构造)，
> 与 Nginx `return 200` 固定响应 **不完全对等**。表格中"大小"为请求 body 中文案长度（非精确字节）。
> 公平对比数据以 `20-real-nic-benchmark.md` 为准。
>
> 日期: 2026-06-12 | 环境: Ubuntu 26.04 LTS, Linux 7.0 | 工具: wrk 4.1.0

---

## 测试环境

| 项目 | 值 |
|------|-----|
| CPU | i9-12900H, 6P+8E, 20 线程 |
| OS | Ubuntu 26.04 LTS, Linux 7.0.0-22 |
| governor | performance |
| 绑核 | 服务 P-core 4-9, wrk E-core 12-19 |
| Worker | 1 |

---

## HTTP 基准

> 端点: Thunder `/hello/hello` (Echo), Nginx `return 200` (172.0.0.1)

```
              ev        native_uring  asio_uring    Nginx 1w
────────────────────────────────────────────────────────────
64B           232k      203k          235k           214k
256B          230k      201k          236k           212k
1K            229k      200k          232k           191k
4K            216k      190k          223k           184k
64K           69k       47k           48k ⚠          81k

P50(64B):     424μs     427μs         220μs          466μs
P50(4K):      457μs     440μs         332μs          543μs
P50(64K):     1.44ms    1.42ms        18.6ms ⚠       1.22ms
```

---

## HTTPS (SSL) 基准

```
              ev        native_uring  asio_uring    Nginx-ssl 1w
─────────────────────────────────────────────────────────────
64B           141k      133k          133k           149k
4K            97k       95k           92k            132k
64K           14.2k     15.6k         15.3k          32.2k

P50(64B):     803μs     394μs         402μs          752μs
P50(4K):      1.23ms    218μs         247μs          824μs
P50(64K):     7.27ms    662μs         667μs          3.14ms
```

---

## loopback vs 真实网卡

| 环境 | HTTP 64B (Thunder vs Nginx) | 原因 |
|------|:---:|------|
| loopback | Thunder +10% | 无硬件中断, io_uring 批量提交抵消 Thunder CPU 开销 |
| 真实网卡 | Nginx +30% | 硬件中断/DMA 弱化批量优势, Thunder CPU 开销暴露 |

> 📖 真实网卡数据 → [`20-real-nic-benchmark.md`](20-real-nic-benchmark.md)

---

## 分析

- loopback 无硬件中断/DMA/NIC offloading，纯 CPU 对决，Thunder io_uring 批量提交优势直接体现
- loopback 掩盖了 Thunder 调用链长 (proto 序列化/反序列化 + 虚函数分发) 的 CPU 开销
- 真实网卡加上硬件等待后，批量优势弱化，CPU 开销差距浮出水面
- 因此 loopback 数据不能作为生产环境参考，Thunder 实际性能以真实网卡数据为准
