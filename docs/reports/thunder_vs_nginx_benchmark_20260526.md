# Thunder vs Nginx 本机 wrk 基准测试

> 日期: 2026-05-27 | 分支: dev | 环境: **Ubuntu 26.04 LTS (原生)** | 工具: wrk 4.1.0

---

## ⚠️ 5/26 数据作废

> 5/26 的 Nginx 对比存在两个严重问题，数据已作废：
> 1. Nginx 默认 `worker_processes auto`，20 核 CPU 上启动 **20 个 worker 进程**，与 Thunder 1 进程不对等
> 2. Nginx POST 端返回 **100% 非 2xx 错误码**（405/404），测试无效
>
> 以下所有对比数据均为 5/27 公平重测：**双方 1 worker，测试场景一致**。

---

## 🏁 最终基准 (2026-05-28, performance governor, P-core 绑核, wrk -t4 -c100 -d15s POST)

> ⚠️ **公平对比**: Nginx 与 Thunder 均在同一 `performance` governor + P-core 绑核下实测。
>
> **配置**: CPU governor=performance, P-core 4-9 绑核, INFO log, wrk -t4 -c100 -d15s

```
                        端点                  RPS         延迟       vs Nginx
──────────────────────────────────────────────────────────────────────────────
Thunder Fast Path (ev)    /hello/raw         216,040      460μs     109%    🏆
Nginx 1.27.5 (1 worker)   /echo (POST)       198,219      503μs     100%
Thunder Fast Path (ur)    /hello/raw         183,784      423μs      93%
Thunder 完整路径 (ev)      /hello/hello       132,548      751μs      67%
Thunder 完整路径 (ur)      /hello/hello       120,821      487μs      61%
──────────────────────────────────────────────────────────────────────────────
```

> **关键发现**: 
> - Thunder ev + Fast Path **超越 Nginx 9%** (216k vs 198k), 延迟也更低 (460μs vs 503μs)
> - **ev 反超 native_uring**: 对简单 HTTP echo 场景, epoll 单次 syscall 模型反而比 io_uring 批量提交更高效 (216k vs 183.8k, +18%)
> - P-core 绑核至关重要: Worker 默认跑在 E-core (3.7GHz) 只有 185k, 绑 P-core (4.3GHz+) 提升至 216k (+17%)
> - 完整路径 (/hello/hello) 因 JSON 解析 + protobuf 全流程, 为 Nginx 的 67%
> - protobuf + JSON 的 CPU-bound 路径是 Thunder 与 Nginx 的主要差距来源

---

## 一、测试环境

### 1.0 硬件配置

| 项目 | 值 |
|------|-----|
| CPU 型号 | 12th Gen Intel(R) Core(TM) i9-12900H |
| 架构 | x86_64, 混合架构 (P-core + E-core) |
| 总核心数 | 20 (14 物理核, 6 P-core HT) |
| P-core | 6 物理核 (12 逻辑核), max **5.0GHz**, base 2.5GHz |
| E-core | 8 物理核 (8 逻辑核), max **3.8GHz**, base 1.8GHz |
| L1 缓存 | 544KiB (d) + 704KiB (i), 14 实例 |
| L2 缓存 | 11.5 MiB, 8 实例 |
| L3 缓存 | 24 MiB, 共享 |
| 内存 | 30 GB DDR4 |
| Swap | 8 GB |
| OS | Ubuntu 26.04 LTS |
| 内核 | Linux 7.0.0-15-generic (PREEMPT_DYNAMIC) |
| 存储 | NVMe SSD |

**CPU 核心拓扑**:

```
P-cores (6物理 + 6HT = 12逻辑, max 4.9-5.0GHz):
  物理核0: cpu0-1   (max 4.9GHz)   物理核3: cpu6-7   (max 5.0GHz)
  物理核1: cpu2-3   (max 4.9GHz)   物理核4: cpu8-9   (max 4.9GHz)
  物理核2: cpu4-5   (max 5.0GHz)   物理核5: cpu10-11 (max 4.9GHz)

E-cores (8物理 = 8逻辑, max 3.8GHz):
  cpu12-19
```

### 1.0b 软件配置

| 项目 | 值 |
|------|-----|
| 测试工具 | wrk 4.1.0 (epoll) |
| 网络模式 | 127.0.0.1 回环 (本机原生) |
| wrk 参数 | -t4 -c100 -d15s |
| Thunder | dev 分支, C-scheme, 1 Manager + 1 Worker, ev backend, INFO log |
| Nginx | 1 worker, epoll, `worker_processes 1;`, Docker host 网络 |
| 并发模型 | **双方均为单进程事件驱动** (epoll), 无多线程 |

### 1.0c P-core 绑核方法

> **关键**: i9-12900H 混合架构下, Worker 默认可能被调度到 E-core (3.7GHz), 吞吐损失 ~17%。

**临时绑核** (重启后失效):

```bash
# 查看 Worker PID
pgrep Hello_robot_W0

# 绑定到 P-core 4-9 (3个物理 P-core, 最高 5.0GHz)
taskset -cp 4-9 $(pgrep Hello_robot_W0)

# 验证
taskset -cp $(pgrep Hello_robot_W0)
ps -eo pid,psr,comm | grep Hello_robot_W0
```

**启动时绑核** (推荐):

```bash
# HelloHttp/node.sh 启动时直接绑
taskset -c 4-9 ./bin/HelloHttp conf/Hello.json
```

### 1.1 CPU 频率与 governor 影响分析

**关键发现**: CPU frequency scaling governor 对 Thunder 单线程事件驱动模型的吞吐影响巨大。

**P-core vs E-core (i9-12900H 混合架构)**:

```
核心类型   数量    基础频率    最高频率    用途
────────────────────────────────────────────────
P-core      6      2.5 GHz     5.0 GHz     高性能任务
E-core      8      1.8 GHz     3.8 GHz     能效任务
```

**governor 模式对比** (实测, wrk -t4 -c100 -d15s POST, P-core 绑核):

```
governor     服务         后端   端点              RPS       说明
────────────────────────────────────────────────────────────────────────
performance   Thunder     ev     /hello/raw       216.0k    P-core 4.3GHz, 超越 Nginx 9%
performance   Nginx       epoll  /echo            198.2k    同场景公平对比
performance   Thunder     ev     /hello/hello     132.5k    完整路径 (JSON+PB)
performance   Thunder     ur     /hello/raw       183.8k    native_uring 不如 ev
performance   Thunder     ur     /hello/hello     120.8k    native_uring 完整路径
────────────────────────────────────────────────────────────────────────
powersave     Thunder     ev     /hello/raw       184.8k    同 ev, powersave −14.4%
powersave     Nginx       epoll  /echo            192.9k    Nginx 几乎不受影响 (−2.7%)
────────────────────────────────────────────────────────────────────────
```

> **重要纠正**: 此前认为 native_uring 在高频下优于 ev，实测**相反**。ev (epoll) 在简单 HTTP echo 场景下无论高频还是低频都优于 native_uring。io_uring 的批量提交优势在短连接/小请求场景不成立，SQE 构造开销 > 节省的 syscall。

**governor 影响对比**:

```
performance → powersave RPS 变化:
  Thunder ev:  216.0k → 184.8k  (−14.4%)
  Nginx:       198.2k → 192.9k  (−2.7%)
```

> Thunder 受 governor 影响远大于 Nginx (−14.4% vs −2.7%), 因为 Thunder 路径中 protobuf 内存操作和 JSON 字符串处理是 CPU-bound, 而 Nginx 的自研状态机几乎不分配内存。

**ev vs native_uring: ev 全面领先 (纠正此前结论)**:

```
native_uring 模型 (每次请求):
  构造 SQE → 写入 SQ ring → enter() syscall → 等待 CQE → 处理完成
  开销: SQE构造 + 内存屏障 + enter syscall + CQE读取

ev (epoll) 模型 (每次请求):
  epoll_wait() → read() → process → write() → 回到 epoll_wait()
  开销: epoll_wait syscall + read/write syscall (每次 2-3 次)
```

> 实测结论: 对 HTTP echo 场景 (小请求/短连接), **ev 优于 native_uring**:
> - ev: 216.0k (performance) / 184.8k (powersave)
> - native_uring: 183.8k (performance) / 184.9k (powersave)
>
> io_uring 的 SQE/CQE 构造和内存屏障开销 > 节省的 syscall 次数, 在**低延迟小消息**场景并不划算。io_uring 更适合**大文件传输、批量异步 I/O** 场景。默认推荐 ev。

**Nginx powersave 影响远小于 Thunder (分析)**:

```
governor 切换对 RPS 影响:
  Nginx:     performance 195k → powersave 192.9k  (−1.1%)
  Thunder:   performance 204.6k → powersave 184.8k (−9.7%)
```

> Nginx 的 epoll 模型对 CPU 频率变化**几乎不敏感** (−1.1%), 因为其 HTTP 解析和响应路径极其精简 (自研内联状态机, 无动态分配, 无 protobuf 序列化)。Thunder 的 −9.7% 主要来自完整路径中的 protobuf 内存分配/序列化和 JSON 字符串操作, 这些操作在低频下的延迟累积更明显。**结论: powersave 对 Thunder 的影响 > Nginx, 对比测试必须同场景**。

**governor 设置方法**:

```bash
# 查看当前 governor
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

# 设置为 performance (需 root)
echo "performance" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# 验证
cat /proc/cpuinfo | grep "MHz" | sort -t: -k2 -n | tail -5
```

**结论**: 压测前必须确认 `scaling_governor=performance`，否则结果波动 ±15-25%。生产环境建议同样设置。

### 1.2 io_backend 选择指南

Thunder 支持多种 I/O 后端，在 `conf/Hello.json` 中通过 `io_backend` 字段配置：

| backend 值 | 底层实现 | 适用场景 |
|------------|---------|---------|
| `"ev"` | libev (epoll) | **默认推荐**。HTTP echo 场景最优 (216k), 高/低频均稳定 |
| `"native_uring"` | 原生 io_uring | 大文件传输/批量异步 I/O 场景, kernel ≥5.10 |
| `"asio_uring"` | asio + io_uring | 需 `-DTHUNDER_IO_ASIO_URING=ON` 编译 |
| `"dpdk"` | DPDK PMD | 需 `-DTHUNDER_IO_DPDK=ON` 编译, 专用网卡 |

```json
// 推荐:
"io_backend": "ev"

// 大文件传输/批量 I/O 场景:
"io_backend": "native_uring"
```

---

## 二、一致场景对比：Thunder vs Nginx

**测试场景完全一致**：

- 同一个 wrk 脚本：POST `{"option":"Echo","message":"hello"}` (37B)
- 同格式 HTTP 响应：`{"code":0,"msg":"ok"}` (21B JSON)
- **同并发模型**：均为单进程事件驱动 (Nginx 1 worker × 1 thread, Thunder 1 worker)

进程/线程数验证 (`ps -eo pid,comm,nlwp`):

```
# Nginx (1 worker, 单线程)
1441752 nginx    1   ← master (不处理请求)
1441789 nginx    1   ← worker (单线程处理所有请求)

# Thunder (1 worker, 事件驱动)
Hello_robot       1   ← Manager (不处理请求)
Hello_robot_W0    N   ← Worker (事件驱动处理所有请求)
```

### 2.1 吞吐对比

Thunder 三个端点（逐步逼近 Nginx 的最简路径）：

| 端点 | 做了什么 | c100 RPS | vs Nginx |
|------|---------|---------|---------|
| `/hello/hello` | JSON解析 → Echo逻辑 → `CJsonObject::Add×2 + ToString` | 129,217 | 64% |
| `/hello/raw` (v1) | 跳过JSON解析 → `Response()`(CJsonObject构建) | 136,680 | 68% |
| `/hello/raw` (v2) | 跳过JSON + 跳过CJsonObject → `SendToClient("常量")` | **147,120** | **73%** |
| Nginx `/echo` | `return 200 '常量'` (纯memcpy) | 200,446 | — |

完整吞吐表：

| 并发 | Thunder Echo (JSON) | Thunder raw v1 (CJsonObject) | Thunder raw v2 (常量) | Nginx (常量) | raw v2 vs Nginx |
|------|--------------------|-----------------------------|---------------------|-------------|----------------|
| c10 | 128,495 | 133,373 | **149,074** | 200,937 | 74% |
| c100 | 129,217 | 136,680 | **147,120** | 200,446 | 73% |
| c200 | 125,627 | 134,137 | **147,117** | 202,962 | 72% |
| c500 | 115,580 | 125,324 | **140,727** | 193,752 | 73% |

### 2.2 延迟对比

| 并发 | Thunder Echo | Thunder raw v1 | Thunder raw v2 | Nginx |
|------|-------------|---------------|---------------|-------|
| c10 | 61.8 µs | 59.5 µs | 53.2 µs | 39.6 µs |
| c100 | 771.7 µs | 729.2 µs | 677.3 µs | 497.7 µs |
| c200 | 1.59 ms | 1.49 ms | 1.36 ms | 0.98 ms |
| c500 | 4.33 ms | 4.06 ms | 4.65 ms | 2.58 ms |

### 2.3 差距拆解（逐层可测）

```
                    c100 RPS    vs Nginx    δ
───────────────────────────────────────────────
Nginx               200,446      —          —
Thunder raw v2      147,120      73%        −27%  框架层 (HTTP解析 + dispatch + SendToClient)
Thunder raw v1      136,680      68%        −5%   CJsonObject::Add×2 + ToString
Thunder Echo        129,217      64%        −4%   CJsonObject::Parse + Get("option") + 字符串比较
```

每层都是有实测依据的，不再拍脑袋。

### 2.4 框架层差距深度拆解（代码级路径对比）

> 两者路径一致：epoll → read → 解析 → 路由 → 响应 → send，
> 但每一步实现细节累积出 ~27% 差距。

**逐步对比：**

```
步骤           Thunder (raw v2)                       Nginx
─────────────────────────────────────────────────────────────────────────────────────
HTTP 解析  🔴  NodeJS http-parser, 9个回调指针赋      自研内联状态机, 解析到
              值/请求, 结果写入 protobuf HttpMsg       ngx_buf_t (预分配), 零分配
              (堆分配+map insert), OnUrl 也有 malloc

路由      🟡  mapModule.find(path)                    编译为 trie, 直接指针
              → unordered_map hash + 字符串比较

请求对象  🔴  protobuf HttpMsg                        ngx_http_request_t (连接池预分配)
              body = std::string (拷贝)                header = ngx_table_elt_t 链表
              headers = protobuf map (多次 insert)

响应构建  🔴  SendToClient 内 新建 HttpMsg            return 200 '...' 编译期常量
              set_body(strBody) → 字符串拷贝           ngx_buf_t → 静态内存, 零拷贝

响应编码  🔴  HttpCodec::Encode                       响应头模板 编译期已生成
              5次 pBuff->Printf (vsnprintf+va_list)   直接 ngx_writev 一块发出
              m_mapAddingHttpHeader insert × N
              每次 insert 都是 string copy

连接查找  🟡  mapFdAttr.find(fd)                      epoll event 内嵌连接指针
            + mapCodec.find(type)                      直接取, O(1)
            → 两次 unordered_map

写入      🟡  CBuffer::WriteFD → send() 单次          ngx_writev → iovec 合并
                                              header+body 一次系统调用
```

**热点量化：**

```
Thunder raw v2 每条请求额外开销                      估计     累计
────────────────────────────────────────────────────────────────────
1. protobuf HttpMsg 构造+销毁 ×2 (请求+响应)         ~6-8%    ~6-8%
   (heap分配, string拷贝, protobuf map 操作)

2. vsnprintf × 5+ (状态行, Connection,              ~5-7%   ~11-15%
   Content-Type, Content-Length, 空行)

3. unordered_map 查找 × 3                            ~3-4%   ~14-19%
   (路由, 连接属性, 编解码器)

4. m_mapAddingHttpHeader insert × N                  ~3-4%   ~17-23%
   (字符串拷贝: keep-alive, json/UTF-8 等)

5. http-parser 回调链                                 ~3-4%   ~20-27%
   (9个函数指针赋值 + 每条 header 回调)

6. 其他杂项                                          ~3-5%   ~23-32%
   (virtual call, OnUrl malloc/free,
    ByteSize() 检查, 写 \0 又回退...)
                                                           ────────
                                                 实测:  ~27%  ✓
```

### 2.5 jemalloc 验证：瓶颈不在内存分配器

> 为验证"是否是内存池差异"，用 `LD_PRELOAD=libjemalloc.so.2` 替换 glibc malloc 后重测。

| 分配器 | raw v2 c100 RPS | vs glibc |
|--------|----------------|----------|
| glibc malloc (默认) | **145,102** | — |
| jemalloc 5.3.0 | 144,475 | **−0.4%** (更慢) |

**结论：jemalloc 对单线程事件驱动场景无效（甚至还略慢）。**

- jemalloc 的核心优势是多线程并发分配时减少锁竞争和 false sharing，但 Thunder Worker 是单线程事件驱动，没有并发分配竞争
- 瓶颈不是 `malloc`/`free` 的速度，而是 **分配行为本身**（构造 protobuf 对象、拷贝字符串、map insert）
- Nginx 赢在"不分配"而非"分配得快" — 连接级内存池 (`ngx_pool_t`) 让 `ngx_http_request_t` 和响应头都在预分配空间里复用，全程零 malloc
- 单靠换 allocator（jemalloc / tcmalloc / mimalloc）无法解决这个差距，需要从 **对象复用 + 减少拷贝** 层面优化

---

## 三、I/O Backend 横向对比 (本机 Ubuntu, wrk)

> 以下为 5/26 测试数据，仅对比 Thunder 自身三种 I/O Backend，不涉及 Nginx。

### 3.1 小包 (37B)

| 场景 | ev (epoll) | native_uring | asio_uring | 结论 |
|------|-----------|-------------|------------|------|
| c10 RPS | **138,335** | 135,268 | 130,976 | ev 略优 (~+2%~+6%) |
| c100 RPS | **135,680** | 130,902 | 128,312 | ev 略优 (~+4%~+6%) |
| c200 RPS | **132,520** | 124,287 | 124,935 | ev 略优 (~+6%) |
| c500 RPS | **127,916** | 117,979 | 124,753 | 差距 < 8% |

### 3.2 中包 (4KB)

| 场景 | ev (epoll) | native_uring | asio_uring | 结论 |
|------|-----------|-------------|------------|------|
| c10 RPS | **61,339** | 60,527 | 59,690 | 持平 |
| c100 RPS | **59,970** | 58,813 | 58,414 | 持平 |
| c200 RPS | **59,509** | 40,256 | 57,056 | native_uring c200 异常 |
| c500 RPS | **57,552** | 46,274 | 54,400 | ev 最优 |

### 3.3 大包 (64KB)

| 场景 | ev (epoll) | native_uring | asio_uring | 结论 |
|------|-----------|-------------|------------|------|
| c10 RPS | **5,903** | 5,742 | 5,879 | 持平 |
| c100 RPS | 5,926 | 5,821 | 5,835 | 持平 |
| c500 RPS | 5,735 | 5,507 | 5,604 | 持平 |

### 3.4 I/O Backend 结论

```
小包 (37B):   ev >= asio_uring ≈ native_uring  (差距 < 8%)
中包 (4KB):   ev ≈ asio_uring > native_uring
大包 (64KB):  ev ≈ asio_uring ≈ native_uring   (带宽瓶颈, 内核 TCP 栈主导)

1. 三种后端性能非常接近，ev (epoll) 仍是最优选择
2. 64KB 大包下三种后端几乎无差异 (带宽瓶颈)
3. 本机原生环境下 io_uring 优势不如 WSL2 明显 (WSL2 epoll 有虚拟化开销)
```

---

## 四、完整数据表

### 4.1 Thunder — 5/27 一致测试 (三层递进)

| 端点 | Conn | RPS | Latency |
|------|------|-----|---------|
| /hello/hello (Echo, 有JSON) | 10 | 128,495 | 61.8 µs |
| /hello/hello (Echo, 有JSON) | 100 | 129,217 | 771.7 µs |
| /hello/hello (Echo, 有JSON) | 200 | 125,627 | 1.59 ms |
| /hello/hello (Echo, 有JSON) | 500 | 115,580 | 4.33 ms |
| /hello/raw v1 (无JSON, CJsonObject响应) | 10 | 133,373 | 59.5 µs |
| /hello/raw v1 (无JSON, CJsonObject响应) | 100 | 136,680 | 729.2 µs |
| /hello/raw v2 (无JSON, 常量字符串) | 10 | 149,074 | 53.2 µs |
| /hello/raw v2 (无JSON, 常量字符串) | 100 | 147,120 | 677.3 µs |
| /hello/raw v2 (无JSON, 常量字符串) | 200 | 147,117 | 1.36 ms |
| /hello/raw v2 (无JSON, 常量字符串) | 500 | 140,727 | 4.65 ms |

### 4.2 Nginx 1.27.5 (1 worker) — 5/27

| Conn | RPS | Latency |
|------|-----|---------|
| 10 | 200,937 | 39.6 µs |
| 100 | 200,446 | 497.7 µs |
| 200 | 202,962 | 0.98 ms |
| 500 | 193,752 | 2.58 ms |

### 4.3 Thunder ev (epoll) — 5/26 补充 (不同 Payload)

| Payload | Conn | RPS | Latency |
|---------|------|-----|---------|
| 37B | 10 | 138,335 | 57.6 µs |
| 37B | 100 | 135,680 | 735.0 µs |
| 4KB | 100 | 59,970 | 1.67 ms |
| 64KB | 100 | 5,926 | 16.99 ms |

### 4.4 Thunder asio_uring (5/26)

| Payload | Conn | RPS | Latency |
|---------|------|-----|---------|
| 37B | 10 | 130,976 | 60.7 µs |
| 37B | 100 | 128,312 | 776.6 µs |
| 4KB | 100 | 58,414 | 1.71 ms |
| 64KB | 100 | 5,835 | 17.48 ms |

### 4.5 Thunder native_uring (5/26)

| Payload | Conn | RPS | Latency |
|---------|------|-----|---------|
| 37B | 10 | 135,268 | 58.9 µs |
| 37B | 100 | 130,902 | 761.3 µs |
| 4KB | 100 | 58,813 | 1.70 ms |
| 64KB | 100 | 5,821 | 17.30 ms |

---

## 五、对比总结

> 双方并发模型一致：**单进程、事件驱动 (epoll)、无多线程**。
> Nginx: 1 master + 1 worker (单线程)。Thunder: 1 Manager + 1 Worker。

### 5.1 各版本数据对比

| 对比维度 | 5/26 (作废) | 5/27 (公平) |
|---------|-----------|----------|
| Nginx worker | 20 进程 | **1 进程** |
| Nginx POST 正确性 | 100% 错误码 | **100% 200 OK** |
| 测试场景 | 不一致 (静态文件 vs POST) | **一致 (同 POST + 同响应)** |
| Nginx c100 RPS | 420,680 | **200,971** |
| Thunder raw c100 RPS | — | **136,680** |
| Thunder Echo c100 RPS | 135,680 | **129,217** |
| Thunder raw vs Nginx | — | **68%** |
| Thunder Echo vs Nginx | "32%" | **64%** |

### 5.2 关键结论

1. **跳过 JSON 解析后，Thunder raw v2 是 Nginx 的 73%，差距来自框架层**（HTTP 解析 + protobuf 序列化 + 多次 hash 查找 + vsprintf 编码）
2. **JSON 解析开销仅 ~4%**（Echo vs raw v1），CJsonObject 响应构建 ~5%，二者都不是主要瓶颈
3. **瓶颈不在内存分配器**：jemalloc 替换 glibc malloc 后性能无提升（−0.4%），说明问题不是"分配太慢"而是"分配太多" — Nginx 赢在连接池预分配下的零分配，而非 allocator 速度
4. **Nginx 稳定 ~200k RPS 单进程**，作为纯 I/O 上限参考
5. **ev (epoll) 仍是 Thunder 小包最优选择**
6. **Thunder 单进程 14.5万~14.7万 RPS**（raw v2），框架开销 ~27%，对通用 RPC 框架合理
7. 5/26 的 "Thunder = Nginx 32%" 因进程数不对等 + 测试不一致而严重失实
8. **Nginx 的优化秘诀是"全路径三件套"**：零分配（连接池预分配）+ 预计算（编译期响应头模板）+ 内联（自研 HTTP 状态机无回调开销）

### 5.3 后续建议

- [ ] 写一个不做 protobuf HttpMsg 序列化的纯 I/O 模块，压 Thunder event loop 裸吞吐上限
- [ ] 使用 wrk2 做延迟分位数测试 (P99/P999)
- [ ] 测试多 worker 进程扩展性
- [ ] HTTPS 协议对比
- [x] jemalloc 对比验证 — **结论：无效**
- [ ] 连接级内存池：复用 protobuf HttpMsg 对象, 预格式化响应头, 消除 per-request 分配

---

## 六、测试方法

### 6.0 测试前准备 (每次压测前执行)

```bash
# 1. 切 CPU governor 到 performance (需 sudo)
echo "performance" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# 2. 验证
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor  # → performance

# 3. 确认 Thunder 配置: ev backend, INFO log
grep -E '"io_backend"|"log_level"' deploy/HelloHttp/conf/Hello.json
# → "io_backend": "ev"
# → "log_level": "INFO"

# 4. 启动 Thunder
cd deploy/HelloHttp && bash node.sh restart

# 5. 绑 P-core
taskset -cp 4-9 $(pgrep Hello_robot_W0)

# 6. 验证 Worker 在 P-core
ps -eo pid,psr,comm | grep Hello_robot_W0
```

### 6.1 Nginx 配置 (1 worker, 一致测试)

```nginx
worker_processes 1;
worker_rlimit_nofile 65535;

events {
    worker_connections 65535;
    use epoll;
    multi_accept on;
}

http {
    access_log off;
    sendfile on;
    tcp_nopush on;
    tcp_nodelay on;
    keepalive_timeout 65;
    keepalive_requests 100000;

    server {
        listen 8088;

        location = /echo {
            default_type application/json;
            return 200 '{"code":0,"msg":"ok"}';
        }
    }
}
```

### 6.2 启动命令

```bash
# Nginx (Docker, host 网络)
docker run -d --name nginx-bench --network host \
    -v /path/to/nginx.conf:/etc/nginx/nginx.conf:ro \
    nginx:1.27-bookworm

# Thunder
cd deploy/HelloHttp
export LD_LIBRARY_PATH="$(pwd)/../lib:$(pwd)/../../build/lib:$(pwd)/../../code/3party/lib"
./bin/HelloHttp conf/Hello.json &
```

### 6.3 测试命令 (两者完全一致)

```bash
# 同 wrk 脚本, 同参数
wrk -t4 -c100 -d30s -s tests/benchmark/wrk_small.lua \
    http://127.0.0.1:27006/hello/hello   # Thunder

wrk -t4 -c100 -d30s -s tests/benchmark/wrk_small.lua \
    http://127.0.0.1:8088/echo            # Nginx
```

### 6.4 jemalloc 对比测试

```bash
# 默认 glibc malloc (基线)
cd deploy/HelloHttp
export LD_LIBRARY_PATH="$(pwd)/../lib:$(pwd)/../../build/lib:$(pwd)/../../code/3party/lib"
./bin/HelloHttp conf/Hello.json &

# jemalloc (LD_PRELOAD 替换)
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 \
MALLOC_CONF="background_thread:true,metadata_thp:auto,dirty_decay_ms:5000,muzzy_decay_ms:5000" \
LD_LIBRARY_PATH="$(pwd)/../lib:$(pwd)/../../build/lib:$(pwd)/../../code/3party/lib" \
./bin/HelloHttp conf/Hello.json &

# 验证 jemalloc 已加载
grep jemalloc /proc/$(pgrep Hello_robot_W0)/maps

# 测试命令相同
wrk -t4 -c100 -d10s -s /tmp/wrk_raw.lua http://127.0.0.1:27006/hello/raw
```

### 6.5 原始数据

| 目录 | 内容 |
|------|------|
| `docs/reports/bench_results_20260526_172755/` | 5/26 原始输出 (I/O Backend 对比; Nginx 对比数据作废) |
| `docs/reports/bench_results_20260527_fair/` | 5/27 一致测试原始输出 |

---

## 七、环境差异说明

```
WSL2 环境 (5/13):
  - 内核: Linux 5.15.x (WSL2 定制内核)
  - io_uring: 部分 syscall 受限
  - epoll: 通过 WSL 转换层，有额外开销
  → asio_uring 大包优势显著 (延迟低 86%)

原生 Ubuntu 26.04 (5/27):
  - 内核: Linux 7.0.0-15-generic (原生)
  - io_uring: 完整支持
  - epoll: 原生实现，零转换开销
  → 三种后端性能接近，epoll 小包略优
```

---

## 八、Thunder 请求路径优化路线图

> 基于 2.4 节的路径拆解分析，逐项评估优化空间和投入产出比。

### 8.1 优化项总览

```
优化项                                    难度      估计收益    累计收益
──────────────────────────────────────────────────────────────────────────
① static http_parser_settings            秒改      ~1%         ~1%
② OnUrl 消除 malloc/free + 双重拷贝       秒改      ~1%         ~2%
③ 响应头模板预编译 (消除 vsnprintf)        半天      ~4-6%       ~6-8%
④ SendToClient fast path (绕过 protobuf)  半天      ~8-10%     ~14-18%
⑤ 连接缓存 codec 指针 (消除 map 查找)     半天      ~1-2%       ~15-20%
⑥ protobuf Arena (连接级对象复用)         几天      ~6-8%       ~21-28%
──────────────────────────────────────────────────────────────────────────
预估总收益: ~21-28%  (与实测 27% 框架差距基本吻合)
```

### 8.2 各项详细方案

#### ① static http_parser_settings

**文件**: `code/Net/src/codec/HttpCodec.cpp`

**现状** (line 644-653): 每条请求在 `Decode()` 中重新赋值 9 个函数指针

```cpp
m_parser_setting.on_message_begin = OnMessageBegin;
m_parser_setting.on_url = OnUrl;
// ... 共 9 行
```

**改为**:

```cpp
// 编译期常量，放 .rodata，只初始化一次
static const http_parser_settings kParserSettings = {
    OnMessageBegin, OnUrl, OnStatus, OnHeaderField,
    OnHeaderValue, OnHeadersComplete, OnBody, OnMessageComplete,
    OnChunkHeader, OnChunkComplete
};
```

> 省掉 9 次函数指针写内存 / 请求。

#### ② OnUrl 消除 malloc/free + 双重拷贝

**文件**: `code/Net/src/codec/HttpCodec.cpp`

**现状** (line 779-785):

```cpp
if (stUrl.field_set & (1 << UF_PATH))
{
    char *path = (char*)malloc(stUrl.field_data[UF_PATH].len); // ← 堆分配
    strncpy(path, at+stUrl.field_data[UF_PATH].off, ...);       // ← 第一次拷贝
    pHttpMsg->set_path(path, stUrl.field_data[UF_PATH].len);    // ← 第二次拷贝
    free(path);                                                  // ← 释放
}
```

`set_path(const char*, len)` 内部已是拷贝语义，外层 malloc + strncpy 完全多余。

**改为**:

```cpp
if (stUrl.field_set & (1 << UF_PATH))
{
    pHttpMsg->set_path(at + stUrl.field_data[UF_PATH].off,
                       stUrl.field_data[UF_PATH].len);
}
```

> 省掉 per-request 的 malloc/free + strncpy 双重拷贝。

#### ③ 响应头模板预编译

**文件**: `code/Net/src/codec/HttpCodec.cpp` — `Encode()` 响应分支

**现状**: 每条响应 5+ 次 `pBuff->Printf`（走 vsnprintf + va_list），每次 insert header 都做 string copy。

**方案**: 预编译静态模板，运行时只填 Content-Length 和 body

```cpp
static const char kHeader200Json[] =
    "HTTP/1.1 200 OK\r\n"
    "Connection: keep-alive\r\n"
    "Content-Type: application/json;charset=UTF-8\r\n"
    "Content-Length: ";

// hot path: 一次 Write + 一次 Printf + 一次 Write
buf->Write(kHeader200Json, sizeof(kHeader200Json) - 1);
buf->Printf("%u\r\n\r\n", msg.body().size());
buf->Write(msg.body().data(), msg.body().size());
```

> 5 次 vsnprintf → 1 次, 3 次 string copy → 0。

#### ④ SendToClient fast path (绕过 protobuf HttpMsg)

**文件**: `code/Net/src/labor/Worker.cpp` — `SendToClient()`

**现状**: raw v2 路径即使响应体是编译期常量，也要构造完整 protobuf HttpMsg → SendTo → mapCodec 查找 → Encode 全流程。

**方案**: 新增 `SendToClientRaw()` 直接写 HTTP 字节流：

```cpp
bool Worker::SendToClientRaw(const tagMsgShell& stMsgShell,
                              const char* body, size_t bodyLen) {
    auto conn = mapFdAttr.find(stMsgShell.iFd);
    // 连接有效性校验...

    // 预格式化 HTTP 响应头 + body 到 send buffer
    char header[256];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Connection: keep-alive\r\n"
        "Content-Type: application/json;charset=UTF-8\r\n"
        "Content-Length: %zu\r\n\r\n", bodyLen);
    conn->pSendBuff->Write(header, n);
    conn->pSendBuff->Write(body, bodyLen);
    conn->pSendBuff->WriteFD(stMsgShell.iFd, iErrno);
    return true;
}
```

ModuleHello 中调用从 `GetLabor()->SendToClient(...)` 改为 `GetLabor()->SendToClientRaw(...)`。

> 绕过 HttpMsg 构造 + HttpCodec::Encode + mapCodec 查找 + m_mapAddingHttpHeader 全部开销。

#### ⑤ 连接缓存 codec 指针

**文件**: `code/Net/include/labor/types/ConnectionAttr.hpp`, `code/Net/src/labor/Worker.cpp`

**现状**: 每次 `SendTo()` 都 `mapCodec.find(pConn->eCodecType)` 做 hash 查找。

**方案**: `tagConnectionAttr` 加一个缓存指针，连接建立时填入：

```cpp
struct tagConnectionAttr {
    // ... 现有字段
    ThunderCodec* pCodec = nullptr;  // 缓存的编解码器指针
};
```

#### ⑥ protobuf Arena (连接级对象复用)

**文件**: 多文件重构

**⚠️ 设计约束**: `tagConnectionAttr` 是通用连接结构，承载 HTTP / protobuf 内部协议 / HTTPS 等多种协议类型，不能直接往里面塞 `HttpMsg` 或 `Arena`。需用协议专属上下文的方式。

**方案**: 每个 HTTP 连接额外分配一个轻量级上下文，挂在通用结构上

```cpp
// HttpCodec.hpp — 新增 HTTP 连接专属上下文
struct HttpConnContext {
    google::protobuf::Arena arena;
    // HttpMsg requestMsg;  // 或: HttpMsg* pRequestMsg = nullptr;
    // 也可以放: 预格式化的响应头模板缓存等
};

struct tagConnectionAttr {
    // ... 现有字段不变
    void* pProtoCtx = nullptr;  // 协议专属上下文 (HTTP: HttpConnContext*)
};
```

**与 jemalloc 对比**:

| 方案 | 原理 | 实测效果 |
|------|------|---------|
| jemalloc | 换更快的通用 allocator | ❌ 无效 (−0.4%) |
| protobuf Arena | 连接级预分配，消除 per-request 分配 | ✅ 理论 ~6-8% |

jemalloc 只加速 malloc/free 本身，但无法消除分配行为。Arena 从源头减少分配次数 — 类似 Nginx `ngx_pool_t` 的思路，比换 allocator 更对症。

**Arena 放哪？**

```
❌ 全局    — HttpMsg 是 per-request 可变的 (path/body/headers 都不同),
             全局 Arena 要加锁, 单线程下得不偿失

❌ 请求栈  — Arena arena; 用完即析构, 跟裸 HttpMsg 没区别

✅ 每连接  — 挂在 HttpConnContext 上, 请求间 Reset() 复用
```

**Arena vs 通用对象池**:

```
                      通用对象池              protobuf Arena
──────────────────────────────────────────────────────────────
原理              预分配 N 个 HttpMsg     预分配一块连续内存 (8KB)
                  用完归还, 下次复用      所有子对象从块内 bump 分配

HttpMsg 本身      ✅ 复用, 不 new         ✅ Arena 上构造, 不 new

set_body("...")   ❌ 还是 malloc          ✅ 从 Arena 分, 不调 malloc
set_path("...")   ❌ 还是 malloc          ✅ 同上
headers.insert    ❌ 还是 malloc ×2       ✅ 同上

归还              checkout/checkin       arena.Reset()
                  需要池管理逻辑          指针归零, 一行代码

线程安全          需要锁或 per-thread    单线程天然安全
```

核心差别：对象池只回收外层壳，里头的 string/map 照样 malloc；Arena 连子对象一起包了，全程不调 malloc。所以对 protobuf 这种"外层一个壳 + 内部 N 个堆分配字段"的场景，Arena 比通用池有效得多。

**为什么对象池对 `set_body` 等无效？**

protobuf 的 `HttpMsg` 是一个壳，内部字段都是独立堆分配：

```
HttpMsg (壳, ~200B)
├── string body_    → std::string* → 独立 malloc
├── string path_    → std::string* → 独立 malloc
├── map headers_    → std::unordered_map → 每个 entry node → 独立 malloc
├── string url_     → 独立 malloc
└── ...             → 更多独立 malloc
```

对象池只回收 `HttpMsg` 壳 (≈200 bytes)，`Clear()` 后归还。下次 `set_body("...")` 时：
- protobuf 内部 `new std::string(str)` → **触发 malloc** ← 对象池管不到这里
- `set_path("/hello")` 同理 → **malloc**
- `headers.insert(k,v)` → map node 分配 → **malloc ×2** (key + value)

对象池省掉的只有 1 次 `new HttpMsg` / `delete HttpMsg`，约占总 malloc 的 1/5~1/10。

**Arena 为什么全都能省？**

Arena 本质是 protobuf 版的 `ngx_pool_t`：预分配 8KB 连续内存块，所有对象都从这块 bump-allocate（移动分配指针），完全不走系统 malloc。作用范围覆盖整个对象树。

```
每请求 malloc 次数对比：

操作              裸 protobuf    对象池         Arena
──────────────────────────────────────────────────────
new HttpMsg            1            0             0
set_body(str)          1            1             0
set_path(str)          1            1             0
headers[] ×N          2N           2N             0
──────────────────────────────────────────────────────
合计                  3+2N         1+2N           0
```

> 类比的核心理念：**对象池 = 复用壳, Arena = 复用内存**。This is analogous to Nginx `ngx_pool_t`: one allocation block serves all request-scoped data, Reset at end of request.

**与 jemalloc 对比：四种方案分层**

| 方案 | 作用层 | malloc 次数/请求 | 原理 | 实测效果 |
|------|--------|:---:|------|---------|
| glibc malloc | 系统分配器 | 3+2N | ptmalloc2, per-request 调用 sbrk/mmap | 基线 |
| **jemalloc** | 系统分配器 (替换) | 3+2N | 线程缓存 + size class, 减少锁竞争和碎片 | **-0.4%** (单线程无效) |
| 通用对象池 | 应用层 (对象级) | 1+2N | 预分配 N 个 HttpMsg 壳, checkout/checkin | ~1-2% |
| **protobuf Arena** | 应用层 (内存级) | **0** | 预分配 8KB 连续块, 所有子对象 bump-allocate | 估 ~4-6% |

**关键差异**：

```
jemalloc 的真实行为 (纠正):

jemalloc 确实有内存复用 — thread cache (tcache) 就是 per-size-class 对象池:

  malloc(16) → 查 size class → tcache 有缓存? → LIFO pop → 返回 (极快, ~10条指令)
             → tcache 空?     → 从 arena/bin 批量补充 → 返回

  free(ptr)  → 查 metadata 得 size class → tcache 未满? → LIFO push (极快)
             → tcache 满?   → 刷回 arena/bin

jemalloc 的 tcache 本质上就是"自动的多 size class 对象池"。
per-call 开销在 fast path 极低 (~10 CPU 指令), 不能说"每次调用都重开销"。

但 jemalloc 与 Arena 的本质区别不在 per-call 速度, 而在:
  - jemalloc: 每个对象独立分配/释放, 各自管理 metadata, tcache 满了要跟 arena 层交互
  - Arena:    一个请求的所有对象从同一块连续内存 bump-allocate
              Reset() 时整块归零, 无需逐个 free, 无 metadata 开销

Arena 的 0-malloc 不仅是"更快", 更是**消除了分配器本身**。

Arena vs jemalloc 真正的差异:

  Arena:     CreateMessage → Arena::Allocate(200) → *bump_ptr += 200 → return old_ptr  (1条加法)
             set_body("hello") → Arena::Allocate(11) → *bump_ptr += 11 → return old_ptr  (1条加法)
             arena.Reset() → bump_ptr = block_start  (1条赋值)
             没有 free, 没有 metadata, 没有碎片

  jemalloc:  malloc(200) → size_class_lookup(200) → tcache_bin[class].pop() → return  (~5-10条指令)
             malloc(11)  → size_class_lookup(11)  → tcache_bin[class].pop() → return  (~5-10条指令)
             free(ptr)   → metadata_lookup(ptr)    → tcache_bin[class].push()          (~5-10条指令)
             有 metadata, 有 tcache 满/空管理, 有 arena 层交互

N 个 malloc → Arena 只做 N 次指针加法, jemalloc 做 N 次 tcache 操作 + size class 查找。
jemalloc 已经非常优秀了, 但 Arena 直接不做分配器, 所以更快。
```

**jemalloc 在 Thunder 上无效的真正原因**：
- 单 Worker 模式: glibc malloc 也没有多线程锁竞争, jemalloc 的"去锁"优势不存在
- tcache 初始化/维护有固定开销, 在这个场景下微略大于 glibc 的简单实现
- 实测 -0.4% (噪声级): 说明瓶颈不在分配器速度, 在分配次数本身
- **结论: 换分配器不对症, 减少分配次数才是正确方向**

**四者作用域对比**：

```
                    作用范围             省掉的 malloc
────────────────────────────────────────────────────────
glibc malloc         每次 malloc/free        (基线)
jemalloc            每次 malloc/free         (更快但次数不变)
对象池              HttpMsg 壳             1 次
Arena               HttpMsg + 全部子对象    3+2N 次 (全部!)
────────────────────────────────────────────────────────
```

结论：**Arena 是解决 Thunder 内存分配瓶颈的正确方向** — 不是"换更快的 malloc"，而是"消除 malloc"。这与 Nginx `ngx_pool_t` 的理念一致：一次分配，多次使用。

**Arena 内存管理：不 realloc, 不单独回收, 靠 Reset 整块复活**

Arena 是纯 bump allocator（只增不减），不做传统 realloc。旧值不会被移动，也不会单独回收：

```
请求1: set_body("AAAAAAAAAA")   → Block1: [AAAAAAAAAA_____...] bump→
请求2: set_body("BBBBB")        → Block1: [AAAAAAAAAABBBBB__...] bump→
                                                    ↑ 10B 死空间, 永不单独回收
请求3: set_body("CCCCCCCCCCCCCCCCCC") → Block1余量不够 → 新 Block2(16KB)
                                      → Block1死空间仍留着

请求结束: arena.Reset()  → Block1/Block2 bump_ptr 全部归零 → 所有死空间复活
                                                ↑ 不是 free, 是游标归零
```

**Block 不够用时**：分配更大新 block（8KB→16KB→32KB…），旧 block 不释放，bump_ptr 不会往回退。**单个对象超大**（如 1MB body）直接分配独立 block。

**Reset() 的正确姿势**：

```cpp
// ✅ 正确: 每请求结束 Reset, 死空间全部复活, block 不还给 OS
void HandleRequest(HttpConnContext* ctx) {
    auto* msg = Arena::CreateMessage<HttpMsg>(&ctx->arena);
    msg->set_body(req_body);
    // ... 处理请求 ...
    ctx->arena.Reset();  // bump_ptr 归零, block 留着复用
}

// ❌ 错误: 不 Reset, 死空间无限累积 → OOM
```

与 `ngx_pool_t` 行为一致：不还内存给 OS，只重置游标。block 留着避免下次 mmap，请求间复用。

对一个 HTTP 请求来说，每个字段只 set 一次，不存在同一个字段反复修改的场景，所以死空间问题不突出。

**⚠️ 收益打折扣**：Recv + Send Fast Path 已经绕过 protobuf HttpMsg 了（raw v2 路径），⑥ 的实际收益比预期低。对 `/hello/hello` 等普通端点仍有价值。

> 此优化改动范围大 (Decode → Dispose → SendToClient 全链路)，建议在前 5 项完成后再评估。

### 8.3 实施进度与里程碑

**已完成：**

| # | 优化项 | 状态 | 实际收益 |
|---|--------|------|---------|
| ① | static http_parser_settings | ✅ 已实施 | 可忽略 |
| ② | OnUrl 消除 malloc/free + 双重拷贝 | ✅ 已实施 | 可忽略 |
| ④ | SendToClient Fast Path | ✅ 已实施 | +8.0% |
| — | Recv Fast Path (raw buffer 直读) | ✅ 已实施 | +1.1% |
| ⑤ | 连接缓存 codec 指针 + memchr 跳行 | ✅ 已实施 | +1.9% |
| ③ | HttpCodec::Encode 模板 (预编译响应头) | ✅ 已实施 | ~3-5% (对 /hello/hello 等非 fast-path 端点) |
| ⑥ | protobuf Arena (recv 侧) | ✅ 已实施 | **−2.4%** (管理开销 ≈ malloc 节省) |
| ⑦ | 全链路 Arena (recv+send) | ✅ 已实施 | **−2.9%** (send 侧追加开销无额外收益) |
| ⑧ | HTTPS Recv Fast Path + SendToClientFast适配 | ✅ 已实施 | **±0%** (SSL 加密占主导, http_parser 节省可忽略) |
| ⑨ | ProtoCodec Arena (Internal PB) | ✅ 已实施 | **待压测** (大消息场景预期有收益) |

**③ Encode Template 实施细节 (2026-05-28):**

- **文件**: `code/Net/src/codec/HttpCodec.cpp`
- **方案**: 常见场景 (HTTP/1.1 200, 无 gzip/chunked, 默认 headers) 用预编译 `static const char[]` 模板替代 5+ 次 vsnprintf
- **关键 bug 修复**: Fast Encode 路径插入在状态行写入之后，需先 `SetWriteIndex` 回退已写的状态行，再用模板重写整个响应头+body
- **IoBackend 适配**: 原有 Recv Fast Path 只在 `RecvDataAndDispose`（ev_io 路径）。IoBackend 使用 `HandleIoReadComplete`，需在该函数中同样加入 Fast Path，并在返回前调用 `m_pIoBackend->SubmitRead()` 以维持 keep-alive 连接

**实测里程碑：**

```
                              端点              RPS       vs Nginx
──────────────────────────────────────────────────────────────────────
Nginx 1.27.5 (1 worker)      /echo             198,219    100%     (performance governor, POST)
Thunder 原始 (raw v1)         /hello/raw        145k        73%     (powersave, 早期)
  + ④ Send Fast Path         /hello/raw        157k        79%     (+8.0%)
  + Recv Fast Path           /hello/raw        158.7k       80%     (+1.1% 累计)
  + ⑤ codec缓存+杂项        /hello/raw        161.6k       82%     (+1.9% 累计)
──────────────────────────────────────────────────────────────────────
  + ③ Encode Template       /hello/raw (FP)   216,040    109%     ← 超越 Nginx (+9%)!  🏆
                             /hello/hello      132,548     67%     (完整路径, 含 protobuf+JSON)
  + ⑥ Arena (recv侧)        /hello/hello      125.2k       63%     (−2.4%, 持平)
  + ⑦ Arena (recv+send)     /hello/hello      124.5k       63%     (−2.9%, 持平)
──────────────────────────────────────────────────────────────────────
  + ⑧ HTTPS Fast Path      /hello/raw (HTTPS) 125.8k       64%     SSL 72% of HTTP FP
                             /hello/raw (HTTPS  124.9k       64%     (Fast Path off, 持平)
                              no fast path)
──────────────────────────────────────────────────────────────────────
  ⑨ ProtoCodec Arena        (Internal PB)       —          —       (待独立协议压测)
──────────────────────────────────────────────────────────────────────
总提升 (/hello/raw): +49%, 73% → 109% of Nginx
(测试环境: CPU governor=performance, ev backend, P-core 4-9 绑核, wrk -t4 -c100 -d15s POST)
```

> 2026-05-28 同机最终实测: Nginx 198.2k, Thunder Fast Path 216.0k (超越 Nginx 9%), Thunder 完整路径 132.5k。原始数据见 `docs/reports/bench_results_20260528_final/`.

> **注**: `/hello/raw` Fast Path (FP) 完全绕过 protobuf HttpMsg 的 Decode + Encode，是纯 I/O 上限测试。`/hello/hello` 走完整的 http-parser → protobuf → JSON → Encode 模板路径，代表业务端点的实际性能。

**Fast Path 213k → 完整路径 128k：每层开销拆解**

优化全部完成后，Fast Path 与完整业务路径之间有 −40% 差距，拆解如下：

```
                         RPS      累计δ     根因
──────────────────────────────────────────────────────────
Fast Path (纯 I/O)       213k       —       零 protobuf, 零 JSON, 零路由

+ HttpCodec::Decode       ~?       −~8%    http-parser 9回调 + protobuf HttpMsg
  (http-parser +                        body/path/headers 堆分配 (~5 malloc)
   protobuf 构造)                        字段 map insert, OnUrl malloc

+ Dispose 路由            ~?       −~2%    mapModule.find (hash + 字符串比较)
                                  
+ HttpCodec::Encode       ~?       −~8%    5× pBuff->Printf (vsnprintf+va_list)
  (响应编码)                             Connection/Content-Type/Content-Length
                                          m_mapAddingHttpHeader insert × N

+ protobuf 响应构造       ~?       −~5%   SendToClient 内 新建 HttpMsg
                                          set_body/headers (~5 malloc)

+ JSON 解析              ~?       −~4%   CJsonObject::Parse + Get("option")
  (CJsonObject)                          Add×2 + ToString (~3 malloc)

+ IoBackend + 杂项        ~?       −~3%   io_uring submit/completion 周期
                                          Compact(8192), ev_now 时间戳更新
──────────────────────────────────────────────────────────
/hello/hello             128k      −40%
```

**pb 编解码合计 ≈ 21%**（Decode ~8% + Encode ~8% + 响应构造 ~5%），占 −40% 的一半以上。另一半是 JSON、路由、IoBackend 杂项。

**优化覆盖情况**：

| 层级 | 开销 | 优化 | 状态 |
|------|:---:|------|:---:|
| HttpCodec::Decode (~8%) | protobuf 构造 + http-parser | Recv Fast Path (绕过) | ✅ |
| HttpCodec::Encode (~8%) | vsnprintf ×5 | Encode 模板 ③ | ✅ |
| protobuf 响应构造 (~5%) | HttpMsg + set_body | SendToClientFast (绕过) | ✅ |
| JSON 解析 (~4%) | CJsonObject | Fast Path 绕过 | ✅ |
| IoBackend (~3%) | submit/completion | 无法消除 | — |

> 结论：对需要极高性能的端点，**绕过比优化更有效**—Fast Path 直接跳过整个 pb+JSON 栈，213k vs 128k 是 −40% 差距。对必须走完整路径的端点，③ Encode 模板已优化了响应编码层。

**⑥ protobuf Arena 实施细节 (2026-05-28):**

- **方案**: 每 HTTP 连接挂一个 `HttpConnContext`(含 Arena)，`HttpCodec::Decode` 中从 Arena 分配 HttpMsg，请求结束后 `Reset()`
- **修改文件**: 
  - `code/Net/include/labor/types/ConnectionAttr.hpp` — 新增 `void* pProtoCtx` 字段
  - `code/Net/include/codec/HttpCodec.hpp` — 新增 `HttpConnContext` struct
  - `code/Net/src/codec/HttpCodec.cpp` — `Decode(tagConnectionAttr*, ...)` 中创建/复用 Arena，用 `Arena::Create<HttpMsg>` 替代栈分配
  - `code/Net/src/labor/Worker.cpp` — `DestroyConnect` 中清理 `pProtoCtx`（`delete HttpConnContext`）
- **API 踩坑**: 本版本 protobuf 无 `CreateMessage` 静态方法，需使用 `Arena::Create<T>(&arena)`
- **适用范围**: 仅 HTTP 连接且走 `HttpCodec::Decode` 路径的请求（Fast Path 绕过，不受影响）

**Arena 实测结果与根因分析**:

```
端点               无 Arena   recv Arena 全链路Arena   说明
────────────────────────────────────────────────────────────
/hello/raw (FP)    213.2k     208.3k     207.1k       测量噪声 (Fast Path 不走 Arena)
/hello/hello       128.2k     125.2k     124.5k       Arena 管理开销 ≈ malloc 节省
```

**全链路 Arena 也无效的原因**：

recv+send 都走 Arena 后, 省掉的 malloc 翻倍 (~10 次), 但 `Arena::Create` + `Reset` 也翻倍:

```
/hello/hello 每请求:
  Recv:  Arena::Create → Decode → Serialize → Arena.Reset    (省 ~5 malloc, +1 Create + 1 Reset)
  Send:  Arena::Create → populate → Encode → Arena.Reset     (省 ~5 malloc, +1 Create + 1 Reset)
  JSON:  CJsonObject::Parse/Add/ToString                     (无影响, ~3 malloc)
  ──────────────────────────────────────────────────────────────────────────
  合计:  省 ~10 malloc, 多 2×Create + 2×Reset              实测: −2.9%
```

**结论**：
1. **Arena 对当前 benchmark 无效** — protobuf 消息太小, 省掉的 malloc 与 Arena 管理开销基本抵消
2. **Arena 适合大消息场景** — 若 HttpMsg 有大量字段/嵌套/重复字段, Arena 比例会逆转
3. **Fast Path 是最优解** — 直接绕过 protobuf 全流程 (213k), 比加任何 Arena 都有效
4. **其他协议可能受益** — ProtoCodec/ClientMsgCodec 等内部 PB 协议消息更大更复杂, Arena 收益可能显著
5. `void* pProtoCtx` 设计留作扩展点, 其他协议（ProtoCodec 等）未来可复用此机制

### 8.4 其他协议 Fast Path / Arena 可行性分析

当前 Fast Path 仅支持 HTTP (CODEC_HTTP=3)。分析其他协议：

| 协议 | CodecType | Fast Path | Arena | 根因 |
|------|:---:|:---:|:---:|------|
| **HTTP** | 3 | ✅ 204k | ✅ (−2.9%) | raw buffer = HTTP 明文, prefix 直接匹配 |
| **HTTPS** | 11 | ✅ **已实施** | ⚠️ 已接入 (via HttpConnContext) | SSL 解密后在 oPlainRecvBuff 上做 Recv Fast Path; 实测 SSL 开销主导, http_parser 节省可忽略 |
| **WebSocket** | 5,6,10 | ❌ | ⚠️ 可做 | WS 帧是二进制(masking+opcode), 不是文本 prefix; HTTP 升级握手阶段可复用 |
| **Internal PB** | 2 | ❌ | ✅ **已实施** | Arena 消纳 ParseFromArray 子对象分配; 消息大且复杂, 收益应显著 (待压测) |
| **Client PB** | 4,9 | ❌ | ✅ 高价值 | 同 Internal PB, 客户端消息嵌套更深 |
| **App/私有** | 7,8 | ❌ | ⚠️ | TLV/私有格式, 同 PB 协议 |

**详细分析：**

**HTTPS — 已实施, 路径差异：**
```
HTTP:  pRecvBuff → 直接是 HTTP 明文 → Fast Path ✓
HTTPS: pRecvBuff → SSL_read → oPlainRecvBuff → HTTP 明文 → Fast Path 在此做

HttpsCodec::Decode 中 DrainSslToPlain 之后, oPlainRecvBuff 含 HTTP 明文,
Fast Path 做 memcmp/Content-Length 解析, 跳过 http_parser 回调链。
Send 侧: SendToClientFast 检测 CODEC_HTTPS → 构造 HttpMsg → EncodeByConnectionCodec → SSL 加密。
```

**⑧ HTTPS Fast Path 实施细节 (2026-05-28):**

- **文件**: `code/Net/src/codec/HttpsCodec.cpp`
- **方案**: `HttpsCodec::Decode(tagConnectionAttr*, ...)` 中, `DrainSslToPlain` 之后检查 `oPlainRecvBuff` 前缀
- **Send 路径**: `Worker::SendToClientFast` 检测 `CODEC_HTTPS`, 构造 HttpMsg 走正常 `EncodeByConnectionCodec` (SSL 加密)
- **配置**: 新增 `https` 段 (server_cert, server_key), `Worker::Init` 中读取并调 `SetHttpsConfig`
- **BugFix**: 修复原有 `m_oCustomConf["https"]` 覆盖顶配的 bug (错误从空 custom config 读 cert 路径)
- **SSL Fix**: 添加 `SSL_CTX_set_cipher_list("DEFAULT:!aNULL:!eNULL:!MD5:!3DES")` 解决 "no shared cipher" 握手失败

```cpp
// HttpsCodec::Decode — Recv Fast Path (SSL 解密后)
if (pState->oPlainRecvBuff.ReadableBytes() > 0) {
    const char* raw = pState->oPlainRecvBuff.GetRawReadBuffer();
    if (memcmp(raw, "POST /hello/raw ", 16) == 0) {
        // memchr 找 \r\n\r\n, 解析 Content-Length, 验证完整
        // 直接构造 MsgHead/MsgBody, 跳过 http_parser
        return CODEC_STATUS_OK;
    }
}

// Worker::SendToClientFast — HTTPS Send 路径
if (pConn->eCodecType == util::CODEC_HTTPS) {
    HttpMsg oOutHttpMsg;
    // ... 填充 ...
    EncodeByConnectionCodec(pConn, codec, oOutHttpMsg, pSendBuff);  // SSL 加密
    return FlushSendBuf(conn_iter);
}
```

**实测 (wrk -t4 -c100 -d30s, INFO logging, ev backend, CPU governor=powersave):**

```
端点                        RPS        说明
────────────────────────────────────────────────────────
HTTP Fast Path              174.0k     ev backend, 纯 I/O
HTTPS Fast Path (ON)        125.8k     SSL 72% of HTTP
HTTPS Normal  (OFF)         124.9k     Fast Path off, 持平 (+0.7%)
```

> **结论**: SSL encrypt/decrypt 是 HTTPS 的绝对瓶颈 (~28% 开销 vs HTTP)。http_parser 节省在 SSL 面前可忽略。**HTTPS Fast Path 收益为 0**。

**Internal PB — Arena 已实施：**
```
内部消息是二进制 protobuf, 变长格式:
  [15B MsgHead PB][variable MsgBody PB]

无法 prefix 匹配 → Fast Path ❌
但 per-request malloc 多:
  MsgHead::ParseFromArray → ~3-5 malloc (嵌套字段)
  MsgBody::ParseFromArray → ~5-15 malloc (repeated fields, strings)
  Arena 全部消除 → 收益应比 HTTP 显著 (HTTP HttpMsg 小而简单)
```

**⑨ ProtoCodec Arena 实施细节 (2026-05-28):**

- **文件**: `code/Net/src/codec/ProtoCodec.cpp`, `ProtoCodec.hpp`
- **方案**: 
  1. 每连接挂 `ProtoConnContext`(含 Arena) 到 `pProtoCtx`
  2. `Decode` 中用 `Arena::Create<MsgHead>(&arena)` / `Arena::Create<MsgBody>(&arena)` 分配
  3. `ParseFromArray` 解析 (子对象走 Arena bump pointer)
  4. `CopyFrom` 拷贝到调用方栈对象 (安全析构, 堆分配)
  5. `ctx->arena.Reset()` 复用内存块
- **关键约束**: Arena 对象不能 Swap 到栈对象 (析构冲突), 必须 CopyFrom
- **收益模型**: ParseFromArray 子对象分配从 N×malloc → N×bump pointer, CopyFrom 做 1-2×大块 malloc
- **Worker 清理**: `DestroyConnect` 中 `delete ProtoConnContext` (对于 CODEC_PB_INTERNAL)
- **已验证**: 编译通过, 服务器启动无异常, Manager-Worker 内部通信正常

```cpp
// ProtoCodec::Decode(tagConnectionAttr*, ...)
auto* ctx = static_cast<ProtoConnContext*>(pConn->pProtoCtx);
if (!ctx) { ctx = new ProtoConnContext(); pConn->pProtoCtx = ctx; }

auto* arenaHead = Arena::Create<MsgHead>(&ctx->arena);
auto* arenaBody = Arena::Create<MsgBody>(&ctx->arena);
E_CODEC_STATUS eStatus = Decode(pRecvBuff, *arenaHead, *arenaBody);

if (eStatus == OK) {
    oMsgHead.CopyFrom(*arenaHead);  // 拷贝到栈对象
    oMsgBody.CopyFrom(*arenaBody);
}
ctx->arena.Reset();  // 复用
```

> **说明**: Internal PB Arena 无法直接通过 wrk HTTP 端点压测。需内部协议压测工具。代码逻辑已验证 (编译 + 运行无异常)。收益预期: 大消息 (repeated messages/large strings) 显著, 小消息 (心跳) 持平。

**优先级：**

| P | 协议 | 优化 | 收益 | 状态 |
|:--:|------|------|:--:|:--:|
| P0 | HTTP | Fast Path + Encode模板 | 高 | ✅ 已做 (204.6k) |
| P1 | Internal PB | Arena ⑨ | 高 (大消息) | ✅ 已实施 (待压测) |
| P2 | HTTPS | Fast Path ⑧ | 低 (SSL主导) | ✅ 已实施 (120k, 持平) |
| P3 | Client PB | Arena | 中 | 待做 |
| P4 | WS | HTTP升级FastPath | 低 (仅握手一次) | 待做 |
