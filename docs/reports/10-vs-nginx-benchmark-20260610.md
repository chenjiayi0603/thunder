# Thunder vs Nginx 本机 wrk 基准测试

> 首测: 2026-06-10 | **当前基准: 2026-06-12 复测 (#70 修复后)** | 分支: dev | 环境: Ubuntu 26.04 LTS (原生) | 工具: wrk 4.1.0
> HTTP 解析器: **picohttpparser** (替换旧版 http_parser, 单次改动 +49%)
>
> 旧版报告的 322k/347k 数据已废弃: 该数据测于 #70 (Worker 60s 误杀) 修复前, 且在当前散热条件下
> 物理不可复现 (见"热墙"说明)。本文只保留 2026-06-12 修复后、条件可复验的数据。

---

## 🏁 最终基准

> **单位换算**: 1ms = 1000μs。

### HTTP 测试结果 (2026-06-12, #70 修复后)

> `performance` governor, P-core 4-9 绑核, INFO log, 1 Worker, wrk -t4 -c100 -d10s (wrk 绑 E-core 12-19)
> 端点 `/hello/raw` (Receive Fast-Path), 全程 0 Worker 误杀

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

> ⚠️ **热墙 = 本机稳态上限 (已实验确认)**: 负载启动 **6 秒内** package 即冲到 95°C, P-core 锁 4.0GHz
> (max 5.0, turbo 未禁用, 纯散热限制)。空闲降温至 66°C 后连跑 5 轮: 239/233/233/235/233k,
> 波动 <3% — **232~239k 是本机 (i9-12900H 45W 笔记本) 可重复的稳态均值**, 与是否"烤机一天"无关。
> 复现更高数值需改善散热或换机, 不存在"冷机即可回 322k"。

**分析:**
- **小中包 Thunder 领先 Nginx ~9-10%** (64B: asio 235k / ev 232k vs nginx 214k)。
  旧报告"2x Nginx"不成立: 一是旧 Nginx 173k 未绑核偏低, 二是 322k 本身不可复现
- **asio_uring 延迟优势显著**: 64B 220μs vs ev 424μs (批量提交降低排队), 吞吐与 ev 持平
- **asio_uring 64K 异常** ⚠: 48k + 18.6ms 延迟 (ev 的 13 倍)。#70 修复后不再崩溃,
  大包劣化为独立遗留问题 (见 issus #70 衍生事项)
- **64K 大包 Nginx 领先** (81k vs 69k): 内核 sendfile/缓冲路径优势
- **native_uring 全线最弱** (−13%): 批量=1 时 SQE 构造是纯开销 (原理见下文 syscall 分析)

### HTTPS (SSL 加密) 测试结果 (2026-06-13 复测, #70 修复后)

> `performance` governor, P-core 4-9 绑核, INFO log, 1 Worker, wrk -t4 -c100 -d10s (wrk 绑 E-core 12-19)
> 端点 `/hello/raw`, nginx-ssl 同证书同绑核, 全程 0 Worker 误杀
> 旧数据 (asio 184k 🏆 / ev 105k) 测于 #70 误杀 + 孤儿双 Worker 时期, 不可复现, 已废弃

```
              ev        native_uring  asio_uring¹   Nginx-ssl 1w
─────────────────────────────────────────────────────────────
64B           141k      133k          133k           149k
4K            97k       95k           92k            132k
64K           14.2k     15.6k         15.3k          32.2k

Latency(64B): 803μs     394μs         402μs          752μs
Latency(4K):  1.23ms    218μs         247μs          824μs
Latency(64K): 7.27ms    662μs         667μs          3.14ms
```

> ¹ asio_uring 需 `-DTHUNDER_IO_ASIO_URING=ON` 编译。

- **三后端吞吐基本持平** (64B: 133~141k, 差距 <6%) — SSL 加解密是 CPU 瓶颈, 摊薄了 syscall 差异;
  旧数据 "asio +75%" 不可复现
- **uring 系延迟显著占优**: 64K 下 0.66ms vs ev 7.27ms (11 倍), 批量收割大幅降低 SSL 多次 BIO 的排队
- **Nginx HTTPS 全线领先** (64B +6%, 4K +36%, 64K +2.3 倍): OpenSSL 集成 + 内核路径成熟度优势,
  大包差距最大

### 延迟与吞吐关系分析

一个常见疑问: 220μs 延迟怎能做到 235k RPS？

单线程事件循环下, wrk 报告的延迟绝大部分是**数据在 kernel buffer 等待 event loop 调度**的时间:

```
请求到达 → [数据排队等 event loop] → read → 解析 → write → 下一轮
            ↑ ~216μs                  ↑ ~4.3μs
```

用 Little 定律验算 (asio_uring 64B): L = RPS × Latency = 235k × 0.22ms ≈ **52 个请求在内核排队**。
真正 CPU 耗时: 1s ÷ 235,000 ≈ **4.3μs / 请求**。

| 阶段 | 耗时 | 占比 |
|:-----|:----:|:----:|
| kernel buffer 排队等调度 | ~216μs | ~98% |
| 实际处理 (read → 解析 → write) | ~4.3μs | ~2% |
| **wrk 端到端延迟** | **220μs** | **100%** |

> 若每请求真需 220μs CPU, 单线程上限只有 ~4.5k RPS。ev 的 424μs vs asio 的 220μs
> 差在排队深度 (ev 逐个 syscall, asio 批量收割), 不是 CPU 路径差异。

---

## 二、核心原理：syscall 分布

四后端性能差异的根本原因：**每次 I/O 操作所需的 syscall 次数 × 能否批量合并**。

```
模型:     应用 → [I/O 引擎] → syscall → 内核

ev:           每次读写 → 1 次 syscall (read/write)
native_uring: 每次读写 → 构造 SQE + 1 次 syscall (enter)
asio_uring:   每次读写 → 入队 → N 次合并 → 1 次 syscall (enter)
Nginx:        每次读写 → 1 次 syscall (read/write), 1 Worker
```

### 每笔 I/O 的 syscall 开销

| 后端 | 每笔 I/O 操作 | syscall 次数 | 可否批量 |
|------|--------------|:-----------:|:-------:|
| ev | `epoll_wait` 就绪 → `read/write` | **1 syscall / I/O** | ❌ 不能 |
| Nginx 1w | 同上 | **1 syscall / I/O** | ❌ 不能 |
| native_uring | 构造 SQE → `enter` | **1 enter syscall / I/O** + SQE 构造 | ❌ 不能 |
| asio_uring | 入队 → 攒批 → `enter` | **1 enter syscall / N I/O** + 入队开销 | ✅ 可以 |

**关键差异**: 当 N=1 时 (native_uring 无批量), io_uring 比 epoll 多了 SQE 构造开销, 却不省
syscall, 所以反而更慢 (实测 −13%)。只有 N>1 时 (asio_uring 批量), io_uring 才真正省 syscall。

### HTTP 场景: 2-3 次 I/O / 请求

吞吐上 asio_uring ≈ ev (235k vs 232k, 应用层是瓶颈), 但批量收割显著降低排队延迟
(220μs vs 424μs)。

### HTTPS 场景: 5-10+ 次 I/O / 请求 (SSL 拆多次 BIO)

实测 (2026-06-13): **吞吐三后端持平** (SSL 加解密是 CPU 瓶颈, syscall 差异被摊薄),
批量优势体现在**延迟**: uring 系 64K 0.66ms vs ev 7.27ms (11 倍)。
理论预期的"批量提交吞吐领先"未兑现 — I/O 排队减少不等于 CPU 路径变快。

### 核心结论

```
HTTP  吞吐: asio_uring ≈ ev > Nginx > native_uring   (小中包; 64K Nginx 领先)
HTTPS 吞吐: Nginx > ev ≈ native_uring ≈ asio_uring   (全线)
延迟:       uring 系 ≪ ev ≈ Nginx                     (HTTP/HTTPS 均如此, 批量收割降排队)

选型: ev 默认推荐 (吞吐均衡稳定);
      uring 系延迟敏感场景 (HTTP asio 64K 劣化待修, 见 #70 衍生);
      native_uring HTTP 无优势; HTTPS 下与 asio 持平且延迟同样优秀
```

---

## 三、测试环境与方法

### 硬件

| 项目 | 值 |
|------|-----|
| CPU | i9-12900H, 6P+8E, 20 线程, max 5.0GHz (P-core), **45W 散热稳态 ≈ 4.0GHz** |
| 内存 | 30 GB DDR4 |
| OS | Ubuntu 26.04 LTS, Linux 7.0.0-22 |

> 混合架构: P-core cpu0-11 (HT), E-core cpu12-19。服务绑 P-core 4-9, wrk 绑 E-core 12-19。

### governor 与热墙

本机持续负载 6 秒内到达 95°C 热墙, P-core 锁 4.0GHz —— **热墙下 performance 与 powersave
差异基本消失** (均 ~4.0-4.1GHz)。governor 对比只在散热充裕的机器上有意义。

### 测试前检查清单 (#69/#70 教训, 必读)

1. **确认 log_level=INFO**: `grep log_level deploy/*/conf/*.json` + 抽查 Worker 日志无 TRACE 行
   (TRACE 在收包热路径有 2+ 条/请求落盘, ModuleRaw 掉 1.7x, ModuleHello 掉 5.8x)
2. **确认无 Worker 误杀**: 压测前后 `grep -c unresponsive log/*_robot.log` 应为 0 (#70 已修)
3. **杀进程顺序**: 先杀 Manager 再杀 Worker; 反序留孤儿监听, SO_REUSEPORT 双 Worker 吞吐虚高 ~40%
4. **压测沙箱 etcd 指向死地址** (如 127.0.0.1:1): 避免污染共享注册表 (槽位 255 上限)
5. **对照实验只改一个变量**; 用 Nginx 同机数据做对照组排除环境因素
6. **检查工作区未提交改动**: `git diff HEAD` 而非只比提交间 diff

### 启动与测试命令

```bash
# 1. performance governor
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# 2. 启动 & 绑核
cd deploy/HelloHttp && bash node.sh restart
taskset -cp 4-9 $(pgrep Hello_robot_W0)

# 3. Thunder 各后端 (conf 改 io_backend 后重启) / 各包体
taskset -c 12-19 wrk -t4 -c100 -d10s -s tests/benchmark/wrk_small.lua http://127.0.0.1:27006/hello/raw
# wrk_small=64B / wrk_256 / wrk_1k / wrk_4k / wrk_64k

# 4. Nginx 对照 (1 worker, epoll, return 200, access_log off; worker 同绑 4-9)
taskset -c 12-19 wrk -t4 -c100 -d10s -s tests/benchmark/wrk_small.lua http://127.0.0.1:8088/echo
```

---

## 四、优化路线图 (历史记录)

> 收益百分比为各优化合入当时的相对值, 绝对 RPS 以最新基准表为准。

| # | 优化 | 收益(当时) | 说明 |
|:-:|------|:----------:|------|
| ④ | SendToClient Fast Path | +8.0% | 绕过 protobuf HttpMsg 构造+编码 |
| — | Recv Fast Path (raw buffer) | +1.1% | 绕过 http_parser 回调链 |
| ⑤ | codec 指针缓存 + memchr | +1.9% | 消除 map 查找 |
| ③ | Encode 响应头模板 | +~5% | vsnprintf ×5 → ×1 |
| ⑧ | **picohttpparser 替换** | **+49%** 🏆 | 旧版 http_parser → picohttpparser |
| ⑥⑦ | protobuf Arena | −2.9% | 小消息场景无效, 大消息预期有效 |
| ⑧ | HTTPS Fast Path | ±0% | SSL 主导, 解析节省可忽略 |
| ⑨ | ProtoCodec Arena | 待压测 | 大消息场景预期收益显著 |

> 对极高性能场景, **绕过比优化更有效** — Fast Path 直接跳过整个 pb+JSON 栈。
> 各端点开销实测 (ev, 64B, 同条件): ModuleRaw (Fast Path) 236k → lua_echo 188k →
> ModuleHello (完整 pb+JSON) 162k; vs Nginx 212k 分别为 111% / 89% / 76%。

### 其他协议 Fast Path / Arena 可行性

| 协议 | Fast Path | Arena | 备注 |
|------|:---------:|:-----:|------|
| HTTP | ✅ 已实施 | ✅ (−2.9%) | 明文 prefix 匹配 |
| HTTPS | ✅ 已实施 | ⚠️ 已接入 | SSL 主导, 解析节省可忽略 |
| WebSocket | ❌ | ⚠️ 可做 | 二进制帧无法 prefix 匹配 |
| Internal PB | ❌ | ✅ 已实施 | 大消息预期收益显著, 待压测 |
| Client PB | ❌ | ✅ 高价值 | 嵌套深, 待做 |

---

## 五、#69/#70 事件复盘 (为什么旧数据作废)

完整调试过程见 `12-worker-60s-kill-debugging-20260612.md`, 问题详情见 issus-list #69/#70/#71/#72。

1. **#69 性能"骤降"**: `conf/Hello.json` 的 `log_level` 被误改 TRACE 且未提交 →
   收包热路径每请求 2+ 条落盘日志 → ModuleRaw 110k (单变量对照: TRACE 134k → INFO 230k, +71%)。
   旁证: 同期 Nginx 不降反升, 排除环境因素
2. **#70 Worker 60s 误杀**: 1d33a9e (优雅重启) 把 controlFd 心跳误路由进 `RecvFdFromWorker`
   吞掉 → 所有首代 Worker 出生 60s 整被 Manager SIGKILL (与负载/后端/包体无关, 纯空闲也死)。
   旧报告所有压测都在带病二进制上跑; "asio_uring 64K 崩溃"是 60s 死亡时钟落入 64K 时间窗的伪相关。
   已修复 (Manager.cpp IoRead 仅 dataFd 走 fd 接收) 并验证 (空闲 135s / 负载 90s 零误杀, ctest 335/335)
3. **322k 不可复现**: 热墙实验确认本机稳态 4.0GHz; 322k 需 ~5GHz 持续 10s, 当前散热条件物理不可达
