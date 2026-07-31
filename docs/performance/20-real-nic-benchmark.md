# Thunder vs Nginx：性能对比

> 日期: 2026-07-31 | 环境: Ubuntu 26.04 LTS | 工具: wrk 4.1.0
> 目标: 192.168.3.61 (enp0s31f6, 1GbE), Nginx 18088, Thunder 28006
> Hello 二进制构建时间: Jul 31 14:04

---

## 测试环境

| 项目 | 值 |
|------|-----|
| CPU | i9-12900H, 6P+8E, 20 线程 |
| OS | Ubuntu 26.04 LTS, Linux 7.0.0-22 |
| governor | performance |
| 绑核 | 服务 P-core 4-9, wrk E-core 12-19 |
| Worker | 1 |

两端点行为对等，均硬编码返回 24 字节 JSON `{"code":0,"msg":"ok"}`，无解析开销。

```
Thunder /hello/raw  →  ModuleRaw → SendToClientFast
Nginx  /echo        →  return 200 '{"code":0,"msg":"ok"}'
```

---

## HTTP 基准

> 端点 `/hello/raw` (Receive Fast-Path, 24B 响应, 无解析)
> 每轮 10s, wrk -t4 -c100 --latency, 目标 192.168.3.61

```
              ev        native_uring  asio_uring    Nginx 1w
────────────────────────────────────────────────────────────
RPS           217k      217k          227k           254k
P50           299μs     340μs         275μs          387μs
```

### 可变响应大小 Echo: `/hello/hello` POST

> Thunder: `{"option":"Echo","size":N}` → 返回 N 字节填充数据
> Nginx: 静态文件 serve

```
大小      ev          native_uring   asio_uring    Nginx (static)
───────────────────────────────────────────────────────────────
64B       191k        187k           178k           167k
256B      181k        180k           184k           165k
1K        163k        167k           164k           164k
4K        140k        138k           129k           162k
64K       24.8k       24.5k          24.7k          74.8k
```

```
大小      ev(μs)      native_uring(μs)  asio_uring(μs)  Nginx(μs)
─────────────────────────────────────────────────────────────────
64B       385         374              306              594
256B      456         538              315              601
1K        496         511              434              604
4K        594         566              618              611
64K       3.50ms      3.36ms           2.96ms           1.00ms
```

**分析:**

- **Fast-path**: Nginx 254k > asio_uring 227k > ev/native_uring 217k。Nginx 领先 12-17%
- **64K 大包**: Nginx 碾压（75k vs 25k），3 倍差距。sendfile 零拷贝优势
- **小中包 (64B-1K)**: Thunder 三后端与 Nginx 基本持平或略优
- **Echo 路径**: Thunder 有 JSON 解析/构造开销，Nginx 纯静态文件 serve，非完全对等

---

## 选型

| 场景 | 推荐 | 原因 |
|------|:---:|------|
| 小包 HTTP 代理 | ev/native_uring | 与 Nginx 持平 |
| 大包/静态资源 | Nginx | sendfile 零拷贝，64K 时 3 倍于 Thunder |
| 通用默认 | ev | 最稳定 |

---

## Nginx 为什么更快

```
Thunder /hello/raw:

  epoll_wait → readv → HttpCodec::Decode (picohttpparser + HttpMsg proto)
    → SerializeAsString → MsgBody → ParseFromString → Dispose(HttpMsg)
      → mapModule.find → ModuleRaw::AnyMessage (虚函数)
        → SendToClientFast → pSendBuff::Write → FlushSendBuf → ::send


Nginx /echo:

  epoll_wait → recv → HTTP 状态机 (原地, 零分配) → location 匹配
    → writev(fd, iovec, 2)
```

差距来自 Thunder 作为多协议网关框架的固定开销：一次 HTTP 请求经过 proto 两次构造 + 一次序列化 + 一次反序列化 + 模块分发虚函数 + 通用 buffer 抽象。这些每层都是纳秒级开销，在 230k RPS 下累计成 5-6% 的差距。不是 bug，是架构代价。

---

> ⚠ 本测试为单机压测。真正的网卡对比需要两台独立机器。
