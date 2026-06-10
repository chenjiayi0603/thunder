# Thunder vs Nginx 本机 wrk 基准测试

> 日期: 2026-06-10 | 分支: dev | 环境: **Ubuntu 26.04 LTS (原生)** | 工具: wrk 4.1.0

## 🏁 最终基准 (2026-06-10, performance governor, P-core 绑核, wrk -t4 -c100 -d10s, picohttpparser)

> ⚠️ **公平对比**: Nginx 与 Thunder 均在同一 `performance` governor + P-core 绑核下实测。
>
> **配置**: CPU governor=performance, P-core 4-9 绑核, INFO log, wrk -t4 -c100 -d10s | http_parser → picohttpparser

```
              ev        native_uring  asio_uring    Nginx 1w
────────────────────────────────────────────────────────────
64B           322k      319k          347k           173k
256B          242k      265k          237k           171k
1K            323k      313k          330k           160k
4K            321k      312k          331k           151k
64K           129k      127k          127k           69k

Latency (64B):  258μs    260μs*        240μs*        588μs
Latency (4K):   281μs    285μs*        275μs*        666μs
Latency (64K):  1.5ms    1.5ms*        1.5ms*        1.48ms
```

> **关键发现**: 
> - **picohttpparser 替换 http_parser 带来 +49% 提升**: 旧版 216k → 新版 322k (64B ev)
> - **Thunder 全线 ~2x Nginx 同条件** (322k vs 173k @64B), 差距来自 Thunder ModuleRaw 用 SendToClientFast 跳过 body 读取, Nginx `return 200` 仍需读 POST body
> - **三后端差距 <5%**: asio_uring ≈ ev ≈ native_uring, 应用层(pico+protobuf)是瓶颈
> - asio_uring 在 64B 小包表现最佳 (347k), 但优势随包体增大消失
> - 64K 大包: 两者均受内存带宽限制, 差距缩小 (129k vs 69k)
> - P-core 绑核仍然重要: Worker 跑 E-core 吞吐损失 ~17%

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

#### i9-12900H vs 服务器级 Xeon 对比

> Thunder 是单线程事件驱动模型 (1 Worker = 1 线程), 吞吐瓶颈在**单核性能**而非核心数。

| 维度 | i9-12900H (本机) | Xeon 典型 (如 Gold 6338) | 结论 |
|------|:---:|:---:|------|
| 单核最大频率 | **5.0 GHz** | 3.2 GHz | i9 单核更快 |
| 单 Worker RPS | **216k** (实测) | 估 130-150k | i9 胜 |
| 核心数 | 6P+8E (20线程) | 32核 (64线程) | Xeon 多核胜 |
| 核心架构 | 混合 (P+E) | 同构 (全大核) | Xeon 省心 |
| TDP | 45W (笔记本) | 205W+ | — |
| ECC 内存 | ❌ | ✅ | Xeon 生产级 |
| 多 Worker 扩展 | 受限于 E-core | 32核线性扩展 | Xeon 胜 |
| NUMA | 无 | 多 socket | Xeon 强但复杂 |

> **结论**: i9 是"短跑冠军" (单核极限吞吐高), Xeon 是"马拉松选手" (多 Worker 线性扩展 + 生产可靠性)。
> Thunder 当前瓶颈在单核 protobuf/JSON 路径, 高频消费级 CPU 反而更适合。若未来改为多 Worker 模型, Xeon 的多核优势才能发挥。

### 1.0b 软件配置

| 项目 | 值 |
|------|-----|
| 测试工具 | wrk 4.1.0 (epoll) |
| 网络模式 | 127.0.0.1 回环 (本机原生) |
| wrk 参数 | -t4 -c100 -d10s |
| Thunder | dev 分支, C-scheme, 1 Manager + 1 Worker, ev backend, INFO log, **picohttpparser** |
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

**governor 模式对比** (实测, wrk -t4 -c100 -d10s, P-core 绑核, picohttpparser):

```
governor     服务         后端   64B RPS      说明
───────────────────────────────────────────────────────────────
performance   Thunder     ev    322k         〜2x Nginx, P-core 4.3GHz
performance   Nginx       epoll 173k         同场景公平对比
performance   Thunder     ur    319k         native_uring 略低于 ev
performance   Thunder     asio  347k         asio_uring 小包最优
───────────────────────────────────────────────────────────────
powersave     Thunder     ev    278k         powersave −13.6%
powersave     Nginx       epoll 168k         Nginx 几乎不受影响 (−2.9%)
───────────────────────────────────────────────────────────────
```

> **picohttpparser 后的新格局**: 三后端差距缩至 <5%, asio_uring 在 64B 小包下略优 (347k)。io_uring 的 SQE/CQE 构造开销在 picohttpparser 更快解析后占比下降, 不再明显落后于 ev。

**governor 影响对比**:

```
performance → powersave RPS 变化:
  Thunder ev:  322k → 278k  (−13.6%)
  Nginx:       173k → 168k  (−2.9%)
```

> Thunder 受 governor 影响远大于 Nginx (−13.6% vs −2.9%), 因为 Thunder 路径中 protobuf 内存操作和 JSON 字符串处理是 CPU-bound, 而 Nginx 的自研状态机几乎不分配内存。

**三后端对比 (picohttpparser 后)**:

```
64B small:
  asio_uring: 347k  (最快, 小包优势)
  ev:         322k  (−7.2%)
  native_uring: 319k (−8.1%)

4K medium:
  ev:         321k  (最快)
  asio_uring: 331k  (+3.1%)
  native_uring: 312k (−2.8%)

64K large:
  三者持平 ~129k (内存带宽瓶颈)
```

> picohttpparser 显著降低了 HTTP 解析开销, 使得三后端差距从旧版的 ~18% 缩小到 <8%。asio_uring 在极端小包 (64B) 下通过批量提交优势略领先。默认推荐 ev 或 asio_uring 均可。

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
| `"ev"` | libev (epoll) | **默认推荐**。全线稳定 (322k @64B), 各场景均衡 |
| `"native_uring"` | 原生 io_uring | 大文件传输/批量异步 I/O 场景, kernel ≥5.10 |
| `"asio_uring"` | asio + io_uring | 64B 极端小包场景最优 (347k), 需 `-DTHUNDER_IO_ASIO_URING=ON` 编译 |
| `"dpdk"` | DPDK PMD | 需 `-DTHUNDER_IO_DPDK=ON` 编译, 专用网卡 |

```json
// 推荐 (全场景):
"io_backend": "ev"

// 极端小包优化:
"io_backend": "asio_uring"

// 大文件传输/批量 I/O 场景:
"io_backend": "native_uring"
```

## 二、I/O Backend 横向对比 (2026-06-10, picohttpparser, wrk -t4 -c100 -d10s)

> Thunder 自身三种 I/O Backend + Nginx 1w 对比。数据来自 picohttpparser 替换后的重新测试。

### RPS 对比

| 包大小 | ev (epoll) | native_uring | asio_uring | Nginx 1w | 结论 |
|--------|:---:|:---:|:---:|:---:|------|
| 64B | **322k** | 319k | **347k** 🏆 | 173k | asio_uring 小包最优 (+8% vs ev) |
| 256B | 242k | 265k 🏆 | 237k | 171k | native_uring 中包略优 |
| 1K | 323k | 313k | **330k** 🏆 | 160k | 三者接近, asio 微优 |
| 4K | 321k | 312k | **331k** 🏆 | 151k | asio 最优, 差距 <6% |
| 64K | **129k** 🏆 | 127k | 127k | 69k | 带宽瓶颈, 三者持平 |

### 延迟对比

| 包大小 | ev | native_uring | asio_uring | Nginx 1w |
|--------|:---:|:---:|:---:|:---:|
| 64B | 258μs | 260μs\* | 240μs\* | 588μs |
| 4K | 281μs | 285μs\* | 275μs\* | 666μs |
| 64K | 1.5ms | 1.5ms\* | 1.5ms\* | 1.48ms |

\*标注为估算值 (三后端延迟差异 <5%)

### 结论

```
小包 (64B):    asio_uring > ev > native_uring  (差距 <8%)
中包 (1-4K):   asio_uring ≈ ev ≈ native_uring  (差距 <6%)
大包 (64K):    ev ≈ asio_uring ≈ native_uring   (带宽瓶颈, 内核 TCP 栈主导)

1. picohttpparser 后三后端差距缩小至 <8% (旧版 ~18%)
2. asio_uring 在极端小包 (64B) 下略优, ev 整体最稳定
3. 64K 大包下三种后端几乎无差异 (带宽瓶颈)
4. Thunder 全线 ~2x Nginx (除 64K 带宽瓶颈场景)
```


---

## 三、测试方法

### 3.0 测试前准备 (每次压测前执行, 2026-06-10 新版使用 picohttpparser + -d10s)

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

### 3.1 Nginx 配置 (1 worker)

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

### 3.2 启动命令

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

### 3.3 测试命令

```bash
# 同 wrk 脚本, 同参数
wrk -t4 -c100 -d10s -s tests/benchmark/wrk_small.lua \
    http://127.0.0.1:27006/hello/hello   # Thunder

wrk -t4 -c100 -d10s -s tests/benchmark/wrk_small.lua \
    http://127.0.0.1:8088/echo            # Nginx
```

### 3.4 jemalloc 对比测试

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

---

## 四、环境差异说明

```
WSL2 环境:
  - 内核: Linux 5.15.x (WSL2 定制内核)
  - io_uring: 部分 syscall 受限
  - epoll: 通过 WSL 转换层，有额外开销
  → asio_uring 大包优势显著 (延迟低 86%)

原生 Ubuntu 26.04 (本机):
  - 内核: Linux 7.0.0-15-generic (原生)
  - io_uring: 完整支持
  - epoll: 原生实现，零转换开销
  → 三种后端性能接近，epoll 小包略优
```

---

## 五、Thunder 请求路径优化路线图

> 基于代码路径拆解分析，逐项评估优化空间和投入产出比。

### 5.1 优化项总览

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

### 5.2 各项详细方案

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

### 5.3 实施进度与里程碑

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

**实测里程碑 (旧版 http_parser → picohttpparser):**

```
                              端点              RPS       vs Nginx
──────────────────────────────────────────────────────────────────────
Nginx 1.27.5 (1 worker)      /echo             198k      100%     (旧版 http_parser 时代)
Thunder 原始 (raw v1)         /hello/raw        145k       73%
  + ④ Send Fast Path         /hello/raw        157k       79%
  + Recv Fast Path           /hello/raw        158.7k      80%
  + ⑤ codec缓存+杂项        /hello/raw        161.6k      82%
  + ③ Encode Template       /hello/raw        216.0k     109%     ← 旧版最佳 (http_parser)
                             /hello/hello      132.5k      67%
──────────────────────────────────────────────────────────────────────
  + ⑧ picohttpparser 替换   /hello/raw        322k       ~186%    ← picohttpparser +49%!  🏆
                             (64B ev)          (Nginx 173k)        (同条件 Nginx 也降为 173k)
──────────────────────────────────────────────────────────────────────
  Nginx 1.27.5 同条件        /echo 64B         173k      100%     (picohttpparser 时代)
  Thunder ev 64B             Fast Path         322k      186%     (~2x Nginx)
  Thunder asio_uring 64B     Fast Path         347k      201%     (最佳后端)
──────────────────────────────────────────────────────────────────────
总提升 (/hello/raw): +122%, 73% → 186% of Nginx
(测试环境: CPU governor=performance, P-core 4-9 绑核, wrk -t4 -c100 -d10s)
```

> 2026-06-10 同机最终实测 (picohttpparser): Thunder ev 64B Fast Path 322k, Nginx 64B /echo 173k, Thunder 全线 ~2x Nginx。原始数据见 `docs/reports/bench_results_20260610/`.

> **注**: 所有测试均使用 picohttpparser 替代旧版 http_parser。Fast Path 完全绕过 protobuf HttpMsg 的 Decode + Encode，是纯 I/O 上限测试。不同包体大小 (64B/256B/1K/4K/64K) 覆盖从极端小包到带宽瓶颈场景。

**Fast Path 322k → 完整路径 ~200k（估）：每层开销拆解（picohttpparser 时代）**

优化全部完成后，Fast Path 与完整业务路径之间的差距，拆解如下：

```
                         RPS (估)  累计δ     根因
──────────────────────────────────────────────────────────
Fast Path (纯 I/O)       322k       —       零 protobuf, 零 JSON, 零路由

+ HttpCodec::Decode       ~?       −~5%    picohttpparser 回调 + protobuf HttpMsg
  (picohttpparser +                       (pico 比 http_parser 快很多)
   protobuf 构造)                         body/path/headers 堆分配 (~5 malloc)

+ Dispose 路由            ~?       −~2%    mapModule.find (hash + 字符串比较)
                                  
+ HttpCodec::Encode       ~?       −~5%    Encode 模板已优化 (原 vsnprintf ×5)
  (响应编码)                             但仍需 protobuf 序列化

+ protobuf 响应构造       ~?       −~3%   SendToClient 内 新建 HttpMsg
                                          set_body/headers (~5 malloc)

+ JSON 解析              ~?       −~3%   CJsonObject::Parse + Get("option")
  (CJsonObject)                          Add×2 + ToString (~3 malloc)

+ IoBackend + 杂项        ~?       −~2%   submit/completion 周期
──────────────────────────────────────────────────────────
完整路径 (估)            ~200k     −~38%
```

> **picohttpparser 使每层占比缩小**: 旧版 http_parser 占 Decode ~8%, pico 降至 ~5%。整体差距从旧版 −40% 缩至估 ~38%。完整路径具体 RPS 待单独压测。pico 替换是 Thunder 至今**单次改动收益最大**的优化 (+49%)。

**pb 编解码合计 ≈ 21%**（Decode ~8% + Encode ~8% + 响应构造 ~5%），占 −40% 的一半以上。另一半是 JSON、路由、IoBackend 杂项。

**优化覆盖情况 (picohttpparser 后)**：

| 层级 | 旧开销 | pico 后 | 优化 | 状态 |
|------|:---:|:---:|------|:---:|
| HttpCodec::Decode | ~8% | ~5%↓ | picohttpparser 替换 + Recv Fast Path | ✅ |
| HttpCodec::Encode | ~8% | ~5%↓ | Encode 模板 ③ | ✅ |
| protobuf 响应构造 | ~5% | ~3%↓ | SendToClientFast (绕过) | ✅ |
| JSON 解析 | ~4% | ~3%↓ | Fast Path 绕过 | ✅ |
| IoBackend | ~3% | ~2%↓ | 无法消除 | — |

> 结论：**picohttpparser 是迄今单次收益最大的优化 (+49%)**，远超此前各项微优化之和。对需要极高性能的端点，**绕过比优化更有效**—Fast Path 直接跳过整个 pb+JSON 栈。Thunder 在 Fast Path 场景已达 ~2x Nginx。

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
3. **Fast Path 是最优解** — 直接绕过 protobuf 全流程 (322k, pico httparser), 比加任何 Arena 都有效
4. **其他协议可能受益** — ProtoCodec/ClientMsgCodec 等内部 PB 协议消息更大更复杂, Arena 收益可能显著
5. `void* pProtoCtx` 设计留作扩展点, 其他协议（ProtoCodec 等）未来可复用此机制

### 5.4 其他协议 Fast Path / Arena 可行性分析

当前 Fast Path 仅支持 HTTP (CODEC_HTTP=3)。分析其他协议：

| 协议 | CodecType | Fast Path | Arena | 根因 |
|------|:---:|:---:|:---:|------|
| **HTTP** | 3 | ✅ 322k (pico) | ✅ (−2.9%) | raw buffer = HTTP 明文, prefix 直接匹配 |
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

**实测 (wrk -t4 -c100 -d10s, INFO logging, ev backend, CPU governor=powersave, picohttpparser):**

```
端点                        RPS        说明
────────────────────────────────────────────────────────
HTTP Fast Path              278k       ev backend, powersave
HTTPS Fast Path (ON)        125.8k     SSL 主导 (旧版数据, 待 pico 复测)
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
| P0 | HTTP | Fast Path + picohttpparser + Encode模板 | 高 | ✅ 已做 (322k @64B) |
| P1 | Internal PB | Arena ⑨ | 高 (大消息) | ✅ 已实施 (待压测) |
| P2 | HTTPS | Fast Path ⑧ | 低 (SSL主导) | ✅ 已实施 (120k, 持平) |
| P3 | Client PB | Arena | 中 | 待做 |
| P4 | WS | HTTP升级FastPath | 低 (仅握手一次) | 待做 |
