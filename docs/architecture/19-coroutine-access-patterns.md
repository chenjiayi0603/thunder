# Thunder 协程访问模式总结

> 涉及: `code/Net/include/coro/`, `code/HelloHttp/src/ModuleHello/`
> 版本: C++20 协程 (`co_await` / `co_return`)

---

## 1. 协程基础设施

### 1.1 StepCo20 — 协程步骤基类

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
│    LibevIo::IoBoolAwaitable                  │
│      HttpGetAsync(url)                       │
│      HttpPostAsync(url, body)                │
│      SendToInternalByNodeTypeAsync(type,…)  │
│      SendToAsync(shell, HttpMsg)             │
│                                              │
│  回调钩子:                                   │
│    Callback(MsgHead, MsgBody)   ← PB响应     │
│    Callback(HttpMsg)            ← HTTP响应   │
└─────────────────────────────────────────────┘
```

- 子类只需继承 `StepCo20`，把业务写成 `AsyncTask` 协程函数，传入 `*this` 引用。
- `IoBoolAwaitable` 是统一的"一次异步等待"句柄：发起 IO → 挂起 → 回调时恢复 → 返回 `bool`。

### 1.2 AsyncTask — 协程 Promise 类型

```cpp
net::AsyncTask MyLogic(net::StepCo20& step) {
    // co_await 任意 IoBoolAwaitable / PoolOffloadAwaiter / ...
    bool ok = co_await step.HttpGetAsync("http://example.com/api");
    // ...
    co_return;
}
```

`AsyncTask` 是 `StepCo20` 启动协程的返回类型，内部持有挂起/恢复逻辑，无需用户关心。

---

## 2. MySQL 协程访问

### 2.1 接口

```cpp
#include "dbi/Dbi.hpp"   // MySqlCoHelper / MySqlReply

net::AsyncTask HelloCoMysqlCo(net::StepCo20& step, util::tagDbConnInfo dbConn) {
    MySqlCoHelper db(step, dbConn);

    // 查询 → 返回 MySqlReply（含 rows vector）
    auto reply = co_await db.Query("SELECT id, name FROM users WHERE id=1");
    if (reply.ok && !reply.rows.empty()) {
        std::string name = reply.rows[0]["name"];
    }

    // 执行（INSERT / UPDATE / DELETE）
    auto exec = co_await db.Exec("UPDATE users SET active=1 WHERE id=1");
    if (exec.ok) {
        // exec.affectedRows
    }
}
```

### 2.2 内部流程

```
co_await db.Query(sql)
  │
  ├─ 向 Worker 线程池提交 SQL 请求
  ├─ 协程挂起（让出 event loop）
  ├─ 线程池执行 mysql_query()
  ├─ 结果通过 PostToEventLoop() 回 Worker
  └─ Worker 恢复协程 → co_await 返回 MySqlReply
```

### 2.3 MySqlReply 结构

| 字段 | 类型 | 说明 |
|------|------|------|
| `ok` | `bool` | 执行是否成功 |
| `rows` | `vector<map<string,string>>` | SELECT 结果行 |
| `affectedRows` | `uint64_t` | DML 影响行数 |
| `errMsg` | `string` | 错误信息 |

---

## 3. Redis 协程访问

### 3.1 接口

```cpp
#include "cache/RedisCoHelper.hpp"   // RedisCoHelper / RedisReply

net::AsyncTask HelloCoRedisCo(net::StepCo20& step,
                               std::string host, int port) {
    RedisCoHelper r(step, host, port);

    // String 操作
    co_await r.Set("key", "value", 3600 /*TTL秒*/);
    auto reply = co_await r.Get("key");
    if (reply.ok) { std::string val = reply.value; }

    // Hash 操作
    co_await r.HSet("user:1", "name", "alice");
    auto hreply = co_await r.HGet("user:1", "name");

    // 删除
    co_await r.Del("key");
}
```

### 3.2 常用方法

| 方法 | 等价命令 | 返回 |
|------|---------|------|
| `Get(key)` | `GET` | `RedisReply{ok, value}` |
| `Set(key,val,ttl)` | `SET EX` | `RedisReply{ok}` |
| `Del(key)` | `DEL` | `RedisReply{ok}` |
| `HGet(key,field)` | `HGET` | `RedisReply{ok, value}` |
| `HSet(key,field,val)` | `HSET` | `RedisReply{ok}` |
| `Expire(key,ttl)` | `EXPIRE` | `RedisReply{ok}` |

### 3.3 内部流程

```
co_await r.Get(key)
  │
  ├─ 向 Worker I/O 提交 Redis 请求（hiredis / 自有连接池）
  ├─ 协程挂起
  ├─ 异步收到 Redis 响应
  └─ Worker 恢复协程 → 返回 RedisReply
```

---

## 4. 跨节点 RPC（节点间二进制协议）

### 4.1 接口

```cpp
net::AsyncTask MyRpc(net::StepCo20& step) {
    MsgHead head;
    head.set_cmd(CMD_DO_SOMETHING);
    head.set_seq(step.GetSequence());   // Worker 按 seq 路由响应

    MsgBody body;
    body.set_data(R"({"userId":42})");

    // 按节点类型路由（Center 注册的服务名）
    bool ok = co_await step.SendToInternalByNodeTypeAsync("LOGIC", head, body);
    if (ok) {
        const MsgHead& rspHead = step.GetLastRspMsgHead();
        const MsgBody& rspBody = step.GetLastRspMsgBody();
        std::string json = rspBody.data();
        // ... 解析 json
    }

    // 也可以按 identify（IP:Port:WorkerIndex）精确路由
    // bool ok = co_await step.SendToInternalByIdentifyAsync("192.168.1.2:9000:1", head, body);
}
```

### 4.2 路由流程

```
co_await SendToInternalByNodeTypeAsync("LOGIC", head, body)
  │
  ├─ Center 查询 LOGIC 节点列表（已注册）
  ├─ 选择目标节点（负载均衡 / 轮询）
  ├─ 序列化 MsgHead+MsgBody → 内部二进制帧
  ├─ 通过内部 TCP 链路发出
  ├─ 协程挂起（等待响应）
  │
  ├─ LOGIC Worker 处理，发回 MsgHead+MsgBody（seq 一致）
  ├─ 本 Worker 按 seq 找回 StepCo20 实例
  └─ 恢复协程 → 返回 bool; 读 GetLastRspMsgHead/Body()
```

### 4.3 注意事项

- `head.set_seq(step.GetSequence())` 是**必须**的，Worker 靠 seq 把响应路由回对应协程
- 超时/失败时 `co_await` 返回 `false`（对端不可达 / 超时）
- `GetLastRspMsgHead()` / `GetLastRspMsgBody()` 只在紧跟 `co_await` 之后有效

---

## 5. HTTP 异步（对外 HTTP 调用）

```cpp
net::AsyncTask FetchExternalApi(net::StepCo20& step) {
    // GET
    bool ok = co_await step.HttpGetAsync("http://api.example.com/v1/data");
    if (ok) {
        const HttpMsg& rsp = step.GetLastHttpMsg();
        std::string body = rsp.body();
    }

    // POST（JSON 请求体）
    std::string reqBody = R"({"action":"ping"})";
    ok = co_await step.HttpPostAsync("http://api.example.com/v1/action", reqBody);
    if (ok) {
        const HttpMsg& rsp = step.GetLastHttpMsg();
        // ...
    }
}
```

内部使用框架内置 HTTP 客户端（非阻塞，event loop 驱动），无需 libcurl。

---

## 6. 线程池卸载（CPU 密集 / 阻塞操作）

### 6.1 场景

- 图片/视频处理
- 正则匹配大文本
- 旧有阻塞库调用（不支持 async）

### 6.2 接口

```cpp
#include "coro/ThreadPoolAwaitable.hpp"

net::AsyncTask ProcessImage(net::StepCo20& step) {
    std::string imageData = step.GetHttpMsg().body();
    auto outBuf = std::make_shared<std::string>();

    // MakePoolOffloadAwaiter：把 lambda 卸载到线程池，完成后回 event loop 恢复协程
    auto result = co_await net::MakePoolOffloadAwaiter(
        &step,
        std::move(imageData),
        outBuf,
        [](std::string&& data, std::shared_ptr<std::string>& out) -> bool {
            // 在线程池线程执行（可以阻塞）
            *out = expensiveTransform(data);
            return true;
        }
    );

    if (result) {
        // 回到 event loop 线程，读取 outBuf
        step.Response(200, *outBuf);
    }
}
```

### 6.3 内部机制

```
co_await MakePoolOffloadAwaiter(...)
  │
  ├─ packaged_task 提交到 std::threadpool
  ├─ 协程挂起（event loop 继续处理其他连接）
  ├─ 任务完成 → threadpool 调 PostToEventLoop()
  └─ Worker event loop 恢复协程 → co_await 返回 ResultT
```

**关键**: `std::future::get()` 在 `await_resume()` 内调用，此时 future 已就绪（任务已 `PostToEventLoop` 返回），不会阻塞 event loop。

---

## 7. ORM 层（高阶封装）

### 7.1 ThunderMysqlMapper

```cpp
// 适合在 event loop 外（业务线程）使用，返回 std::future
ThunderMysqlMapper mapper(dbConfig);

// 同步风格（在线程池或独立线程中调用）
auto rows = mapper.FindAll<User>("SELECT * FROM users WHERE active=1");
auto user = mapper.FindById<User>(42);
mapper.Insert(user);
mapper.Update(user);
mapper.Delete<User>(42);
```

### 7.2 ThunderRedisMapper

```cpp
ThunderRedisMapper cache(redisConfig);

// 序列化对象到 Redis Hash
cache.HSet("user:1", user);        // 自动字段映射
auto u = cache.HGetAll<User>("user:1");

// 有 TTL 的缓存
cache.SetWithTTL("session:abc", sessionObj, 1800);
```

### 7.3 选型建议

| 场景 | 推荐方式 |
|------|---------|
| HTTP 请求处理中访问 DB/Redis | `co_await MySqlCoHelper` / `RedisCoHelper` |
| 大量数据导入/批处理 | `ThunderMysqlMapper`（线程池） |
| 对象缓存层 | `ThunderRedisMapper` |
| CPU 密集型处理 | `MakePoolOffloadAwaiter` |
| 跨微服务调用 | `SendToInternalByNodeTypeAsync` |

---

## 8. 完整示例：复合协程（DB + Redis + RPC）

```cpp
net::AsyncTask HandleOrder(net::StepCo20& step) {
    std::string reqBody = step.GetHttpMsg().body();
    util::CJsonObject req(reqBody);
    int64 userId = 0;
    req.Get("userId", userId);

    // 1. 查 Redis 缓存
    RedisCoHelper cache(step, "127.0.0.1", 6379);
    auto cached = co_await cache.Get("order:user:" + std::to_string(userId));
    if (cached.ok) {
        step.Response(200, cached.value);
        co_return;
    }

    // 2. 缓存未命中，查 MySQL
    MySqlCoHelper db(step, dbConn);
    auto dbReply = co_await db.Query(
        "SELECT * FROM orders WHERE user_id=" + std::to_string(userId) + " LIMIT 10");
    if (!dbReply.ok) {
        step.Response(500, R"({"code":-1,"msg":"db error"})");
        co_return;
    }

    // 3. 通知 LOGIC 节点更新统计
    MsgHead head;
    head.set_cmd(CMD_UPDATE_STATS);
    head.set_seq(step.GetSequence());
    MsgBody body;
    body.set_data(R"({"userId":)" + std::to_string(userId) + "}");
    co_await step.SendToInternalByNodeTypeAsync("LOGIC", head, body);

    // 4. 写缓存
    std::string result = buildJson(dbReply.rows);
    co_await cache.Set("order:user:" + std::to_string(userId), result, 300);

    step.Response(200, result);
}
```

**所有异步等待均为 `co_await` 一行，写法与同步代码一致，event loop 在等待期间处理其他请求，零阻塞。**
