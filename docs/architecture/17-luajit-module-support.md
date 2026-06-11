# LuaJIT 模块支持 — 设计文档

> 日期: 2026-06-10 | 状态: 🟡 待实现

## 背景

当前 Thunder 模块为 C++ SO (dlopen)，修改需编译、NFS 分发、`GracefulRestartWorker`。
Lua 脚本可**不重启进程**热加载，且适合动态路由、请求改写、简单业务逻辑。

## 已有基础

| 组件 | 状态 | 说明 |
|------|:----:|------|
| libluajit | ✅ 已安装 | `libluajit-5.1-2`，OpenResty 带来 |
| LuaJIT C API | 成熟 | `luaL_newstate`, `lua_pcall`, `luaL_loadstring` |
| SO 模块接口 | ✅ 已有 | `class Module` 虚函数，`AnyMessage(HttpMsg)` |
| 热加载框架 | ✅ 已有 | `LoadSoAndGetModule`, etcd 版本管理 (#45) |

## 架构

```
Thunder Worker
  │
  ├── C++ Module (SO) — 高性能，计算密集
  │
  └── LuaJIT VM — 动态逻辑，热加载
        │
        ├── 全局状态 (lua_State*)
        ├── 注册函数 (SendToClientFast, HttpMsg 读写)
        ├── 脚本池 .lua (按 url_path 加载)
        └── 每请求 entry/exit
```

## ModuleLua 类

```cpp
class ModuleLua : public net::Module
{
public:
    ModuleLua(const std::string& strScriptPath);
    ~ModuleLua();

    virtual bool Init();
    virtual bool AnyMessage(const net::tagMsgShell&, const HttpMsg&) override;

private:
    lua_State* m_pLuaState = nullptr;
    std::string m_strScriptPath;
    int m_iFuncRef = LUA_NOREF;  // 缓存的处理函数引用
};
```

### Lua 端 API

```lua
-- 模块脚本入口 (AnyMessage 回调)
function handle_request(msg)
    -- msg.path, msg.body, msg.headers["key"]
    -- 支持:
    --   SendToClient(msg)    -- 返回 HTTP 响应
    --   SendToNext(module)   -- 转发到下一个模块
    --   Log(level, text)     -- 日志
    return {code = 0, msg = "ok"}
end
```

### 配置

```json
{
    "url_path": "/hello/lua_echo",
    "script_path": "scripts/echo.lua",
    "load": true,
    "version": 1
}
```


## 实测性能

同机同条件 (performance governor, P-core 绑核, wrk -t4 -c100 -d10s, POST echo JSON):

```
┌────────────────────────────────────┬──────────┬──────────────────┐
│              方案                  │   RPS    │   vs SO Fast Path│
├────────────────────────────────────┼──────────┼──────────────────┤
│ SO Fast Path (ModuleRaw)           │  234k 🏆 │        —         │
├────────────────────────────────────┼──────────┼──────────────────┤
│ ModuleLua (纯返回, 不读 msg)       │  203k    │       87%        │
├────────────────────────────────────┼──────────┼──────────────────┤
│ SO 完整路径 (ModuleHello)          │  194k    │       83%        │
├────────────────────────────────────┼──────────┼──────────────────┤
│ ModuleLua (读 msg:path + body)     │  179k    │       76%        │
├────────────────────────────────────┼──────────┼──────────────────┤
│ Nginx (return 200)                 │  127k    │       54%        │
├────────────────────────────────────┼──────────┼──────────────────┤
│ Envoy (direct_response)            │  239k    │      102%        │
├────────────────────────────────────┼──────────┼──────────────────┤
│ OpenResty LuaJIT (ngx.say)         │  131k    │       56%        │
└────────────────────────────────────┴──────────┴──────────────────┘
```

| 脚本写法 | RPS | 说明 |
|---------|:---:|------|
| `SendToClientFast(...)` | 203k | 纯返回，不读请求数据 |
| `msg:path()` + `msg:body()` + `SendToClientFast(...)` | 179k | 读取请求字段后再返回 |
| `msg:headers()["key"]` + `SendToClientFast(...)` | ~180k (估) | header 查找 |

差距原因：msg:path() 每次 Lua→C 跨边界 ~12k RPS 开销 (echo 极简路径下占比大，业务逻辑重时缩小)。

## LuaJIT vs SO vs WASM

| 维度 | SO 模块 | LuaJIT 脚本 | WASM 沙箱 |
|------|:-------:|:-----------:|:---------:|
| 性能 | 🏆 原生 | ✅ 接近 native (JIT) | ⚠️ ~30ns 调用 |
| 热更新 | 进程重启 | ✅ **重载文件** | ✅ **实例替换** |
| 开发效率 | 编译时间⏳ | ✅ **即时修改** | 需编译 wasm |
| 隔离 | ❌ 弱 | ✅ 沙箱 (限制 os/io) | ✅ 强沙箱 |
| 语言 | C/C++ | Lua | Rust/C++/Go |

## 实现步骤

| # | 内容 | 预估 |
|:-:|------|:----:|
| # | 内容 | 状态 |
|:-:|------|:----:|
| 1 | CMake 链接 luajit | ✅ 需主二进制预链接 + LD_LIBRARY_PATH |
| 2 | ModuleLua 类 + AnyMessage 回调 | ✅ 203k RPS (SO Fast Path 的 87%) |
| 3 | Lua binding: SendToClientFast/SendToNext/SendToConHash/SendToNodeType/SentTo | ✅ 已注册 |
| 4 | 配置 + 热加载 | ⚠️ 脚本路径需绝对路径 |
| 5 | 压测验证 | ✅ 最终 203k RPS | 链接 luajit (`find_package` / `pkg-config`) | 0.5h |
| 2 | 实现 `ModuleLua` 类 (Init/AnyMessage/析构) | 1天 |
| 3 | Lua binding: `SendToClientFast`, `HttpMsg` 读写 | 1天 |
| 4 | 配置 + 热加载支持 (重载 .lua 不重启) | 0.5天 |
| 5 | 单元测试 + echo 压测对比 | 0.5天 |



## 脚本分发与热加载

### 推荐方案 (etcd)

```
Admin / CI → PUT /thunder/config/scripts/echo.lua → etcd
                                                    ↓
                                   Worker Watch → 写入本地磁盘 → LuaJIT 重载
```

复用 #45 SO 模块管理的同一套 etcd 下发机制。脚本内容直接存 etcd value，Worker watch 到变更后写本地文件 + 通知 ModuleLua 重载。

### 其他方案对比

| 方案 | 版本管理 | 时效 | 复杂度 |
|------|:-------:|:----:|:------:|
| etcd (推荐) | ✅ 有 | 秒级 | 低 (现有机制) |
| NFS 共享 | ❌ 无 | 秒级 | 低 |
| GitOps | ✅ 有 | 分钟级 | 高 |
| Admin 直传 | ⚠️ 手动 | 秒级 | 低 |

### 为什么 Lua 走 etcd, SO 走 NFS

| | Lua 脚本 | SO 文件 |
|------|---------|---------|
| 典型大小 | 2-50 KB | 2-50 MB |
| etcd value 上限 (1.5MB) | ✅ 够用 | ❌ 超 1000 倍 |
| 变更频率 | 分钟级 (高频) | 天/周级 (低频) |
| etcd 存储方式 | **内容本体** (脚本直接存) | **元数据** (只存 path/version/url) |
| 分发方式 | etcd watch → 写磁盘 | NFS / Docker 镜像外部分发 |

etcd 管理 Lua 脚本不是因为它更"能"，而是 Lua 脚本**小 + 高频变更**的特点适合 etcd 直传。SO 文件大、变更少，走 NFS 更高效。
### 回滚

etcd 中保留历史版本 (#42)，Admin 页面一键回滚到上一版本。

## Lua 在业界的定位

| 公司 | 产品 | Lua 用途 | 特点 |
|------|------|---------|------|
| 携程 | OpenResty 网关 | 动态路由、限流、签名校验 | 只做判断和转发，无复杂 IO |
| 美团 | CAT 监控 | 数据采样、过滤、聚合 | 纯逻辑，几乎无 IO |
| 网易 | 游戏网关 | 协议路由、封禁检查 | 规则引擎类 |
| CloudFlare | OpenResty WAF | 防火墙规则、请求过滤 | 配置规则，非业务代码 |

**共同点**: Lua 全只做"判断和转发"，不做业务计算和 IO。路由、限流、鉴权、日志、header 改写 — 薄逻辑层，每请求几微秒。

### Thunder 的 Lua 定位

```
请求 → Lua (路由/鉴权/限流/改写) → 通过 → C++ SO 模块 (业务 + DB)
               ↓ 拒绝
            返回错误
```

Lua 做 **gatekeeper**，不做业务处理。业务逻辑和数据操作仍在 C++ SO 模块中。
## 注意事项

- **定位**: Lua 只写逻辑(路由/过滤/改写)，不碰 IO。DB 走 C++ StorageModule（co_await 异步）
- **为什么不支持 MySQL**: LuaJIT FFI 可绑 libmysqlclient，但 mysql_query() 是同步阻塞，单线程事件循环下会卡死所有请求。异步 Lua↔C++ 桥接复杂度高，不划算
- **安全**: 脚本中 os.execute / io.open 等危险函数需 remove
- **性能**: LuaJIT 对热点路径 JIT 编译，非热点走解释器，echo 场景实测 ≈ native
- **状态隔离**: 多 .lua 脚本共享一个 lua_State 还是独立？推荐共享 + 沙箱
- **调试**: `lua_Debug` + `luaL_traceback` 提供错误信息

## 参考

- OpenResty `content_by_lua` 实现 (ngx_http_lua_module)
- LuaJIT 官方文档: https://luajit.org/
- Thunder `Module` 接口: `code/HelloHttp/src/ModuleRaw/`
