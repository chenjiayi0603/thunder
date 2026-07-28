# C++20 协程访问模式

> 源码: `code/Net/include/coro/`, `code/HelloHttp/src/ModuleHello/`

---

## 协程基础设施

### StepCo20 — 协程步骤基类

```
┌─────────────────────────────────────────────┐
│               StepCo20                      │
│  继承自 HttpStep（封装请求/响应上下文）       │
│                                              │
│  成员:                                       │
│    Context m_context                         │
│      └─ std::coroutine_handle<> handle       │
│      └─ LibevIo（IO 异步能力混入）           │
│                                              │
│  方法:                                       │
│    IoBoolAwaitable                           │
│      HttpGetAsync(url)                       │
│      HttpPostAsync(url, body)                │
│      SendToInternalByNodeTypeAsync(type,…)  │
│      SendToAsync(shell, HttpMsg)             │
└─────────────────────────────────────────────┘
```

### AsyncTask

```cpp
net::AsyncTask MyLogic(net::StepCo20& step) {
    bool ok = co_await step.HttpGetAsync("http://example.com/api");
    co_return;
}
```

---

## MySQL 协程访问

```cpp
net::AsyncTask HelloCoMysqlCo(net::StepCo20& step, util::tagDbConnInfo dbConn) {
    MySqlCoHelper db(step, dbConn);
    auto reply = co_await db.Query("SELECT id, name FROM users WHERE id=1");
    if (reply.ok && !reply.rows.empty()) {
        std::string name = reply.rows[0]["name"];
    }
}
```

```
co_await db.Query(sql)
  → 向 Worker 线程池提交 SQL → 协程挂起
  → 线程池执行 mysql_query()
  → PostToEventLoop() 回 Worker → 协程恢复 → 返回 MySqlReply
```

---

## Redis 协程访问

```cpp
net::AsyncTask HelloCoRedisCo(net::StepCo20& step, std::string host, int port) {
    RedisCoHelper r(step, host, port);
    co_await r.Set("key", "value", 3600);
    auto reply = co_await r.Get("key");
}
```

| 方法 | 等价命令 | 返回 |
|------|---------|------|
| `Get(key)` | `GET` | `RedisReply{ok, value}` |
| `Set(key,val,ttl)` | `SET EX` | `RedisReply{ok}` |
| `Del(key)` | `DEL` | `RedisReply{ok}` |
| `HGet(key,field)` | `HGET` | `RedisReply{ok, value}` |
| `HSet(key,field,val)` | `HSET` | `RedisReply{ok}` |

---

## 跨节点 RPC

```cpp
net::AsyncTask MyRpc(net::StepCo20& step) {
    MsgHead head; head.set_cmd(CMD_DO_SOMETHING); head.set_seq(step.GetSequence());
    MsgBody body; body.set_data(R"({"userId":42})");
    bool ok = co_await step.SendToInternalByNodeTypeAsync("LOGIC", head, body);
    if (ok) {
        const MsgBody& rspBody = step.GetLastRspMsgBody();
    }
}
```

`head.set_seq(step.GetSequence())` 必须——Worker 靠 seq 把响应路由回对应协程。

---

## 线程池卸载

```cpp
auto result = co_await net::MakePoolOffloadAwaiter(
    &step, std::move(imageData), outBuf,
    [](std::string&& data, std::shared_ptr<std::string>& out) -> bool {
        *out = expensiveTransform(data); return true;
    });
```

```
co_await MakePoolOffloadAwaiter(...)
  → packaged_task 提交到 std::threadpool → 协程挂起
  → 任务完成 → threadpool 调 PostToEventLoop()
  → 事件循环恢复协程 → 返回结果
```

---

## 协程 vs 线程池分工

| 维度 | 协程（事件循环） | 线程池 |
|------|:-------------:|:-----:|
| 并发模型 | 单线程协作式 | 多线程抢占式 |
| 上下文切换 | 0（函数调用级） | ~1-10μs |
| 内存开销 | ~几 KB/协程 | ~MB/线程 |
| 阻塞操作 | ❌ 卡死事件循环 | ✅ 设计目的 |
| 异步 IO | ✅ epoll/io_uring | ❌ |

```
99% 快速路径（协程）               1% 阻塞路径（线程池）
─────────────────────             ────────────────────
HTTP 解析、Protobuf 编解码        MySQL connect/query
Redis GET/SET                    文件 SHA256 / 大压缩
etcd 通信、RPC 转发              第三方阻塞 SDK
```
