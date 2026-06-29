# LuaJIT 模块支持 — 设计文档

> 日期: 2026-06-10 | 更新: 2026-06-29 (#129 VM-only 热重载) | 状态: ✅ 已实现

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
    -- msg:path()          -- 请求路径 (string)
    -- msg:body()          -- 请求 body (string)
    -- msg:method()        -- HTTP method (int)
    -- msg:headers()["key"]-- 请求头 (table, 只读)
    --
    -- 同步响应 (不涉及 LOGIC):
    SendToClientFast('{"code":0,"msg":"ok"}')
    --
    -- 异步响应 (发到 LOGIC, 等回包再回客户端):
    SendToLogic(msg:body(), function(resp)
        return '{"code":0,"msg":"ok","logic":' .. resp .. '}'
    end)
    return true
end
```

#### `SendToClientFast(jsonStr)` — 同步返回 HTTP 响应

- 参数: JSON 字符串
- **只能在 `handle_request` 同步路径调用** — 此时 `__current_shell` 有效
- 异步回调中调用会失败: `__current_shell` 已被清空

#### `SendToLogic(body, callback)` → `bool` — 异步请求 LOGIC 并等回包

- 参数:
  - `body`: 发给 LOGIC 的请求体 (string)
  - `callback`: `function(resp)` — LOGIC 回包时被调用, `resp` 是 LOGIC 响应体
- 回调**必须 `return`** 一个字符串作为客户端响应 — C++ `LogicStep::Callback` 用捕获的 `m_shell` 发送
- **不要在回调里调 `SendToClientFast`** — 此时 `__current_shell` 已 nil
- LOGIC 不可达时自动返回 `{"code":1,"msg":"logic timeout"}`

### `SendToLogic` 回调底层实现

```
Lua:  SendToLogic("body", function(resp) return "response" end)
                      │                          │
                      │ arg1                     │ arg2
                      ▼                          ▼
C++:  lua_SendToLogic(L)
        │
        ├─ lua_tolstring(L, 1) → body
        ├─ luaL_ref(L, LUA_REGISTRYINDEX) → cbRef   // 将 Lua 函数存入注册表
        ├─ 取 __module_instance → ModuleLua*
        ├─ 取 __current_shell  → tagMsgShell shell   // 此时有效, 捕获进 LogicStep
        │
        ├─ new LogicStep(self, shell, cbRef, body)
        │     │
        │     ├─ 构造: 保存 m_shell, m_iCbRef, m_body
        │     │
        │     ├─ RegisterCallback(step)  // Step 存入 mapCallbackStep (key=seq)
        │     └─ Emit(0)                 // 触发 Emit → SendToSession("LOGIC")
        │                                    │
        │                            h.set_cmd(10001)
        │                            h.set_seq(GetSequence())  // seq 用于回包匹配
        │                            SendToSession("LOGIC", h, b)
        │
        └─ return true  (Lua 侧收到 boolean)

                    ┌──── LOGIC 处理请求 ────┐
                    │  CmdGetToken::AnyMessage │
                    │  → SendToClient(stMsgShell, resp) │
                    └──────────┬─────────────┘
                               │ 回包 seq 匹配
                               ▼
C++:  LogicStep::Callback(stMsgShell, msgHead, msgBody)
        │
        ├─ lua_rawgeti(L, LUA_REGISTRYINDEX, m_iCbRef) // 取回调函数
        ├─ lua_pushlstring(L, oBody.body())              // 压入 LOGIC 回包
        ├─ lua_pcall(L, 1, 1, 0)                         // 调 callback(resp), 1入1出
        │     │
        │     └─ Lua: function(resp) return '{"code":0,...}'
        │                    │
        │                    └─ return 字符串 → 留在 Lua 栈顶
        │
        ├─ lua_isstring(L, -1)?  →  lua_tolstring → resp, len
        ├─ SendToClientFast(m_shell, resp, len)  // 用捕获的 m_shell, 非全局变量
        ├─ lua_pop(L, 1)
        └─ luaL_unref(L, LUA_REGISTRYINDEX, m_iCbRef)  // 释放回调引用
```

**关键设计点:**

| 机制 | 说明 |
|------|------|
| `m_iCbRef` | `luaL_ref` 将 Lua 函数存注册表, 返回整数索引, 异步回包时用 `lua_rawgeti` 取回 |
| `m_shell` 捕获 | `LogicStep` 构造时保存 `tagMsgShell` — 此时连接有效; 异步回调不再依赖全局 `__current_shell` |
| `seq` 匹配 | `Emit` 把 `GetSequence()` 写入 `MsgHead.seq`, LOGIC 回包原样带回, 框架 `mapCallbackStep.find(seq)` 路由到正确的 Step |
| 回调返回值 | `lua_pcall(L, 1, 1, 0)` — 1 个参数 (LOGIC body), 期望 1 个返回值 (响应字符串)。不 return 或 return nil → C++ 不发客户端 |
| 超时 | `step_timeout`(默认 5s) 后 `LogicStep::Timeout` 触发 → `SendToClientFast(m_shell, '{"code":1,"msg":"logic timeout"}')` |

### 示例脚本

**echo.lua** — 纯同步返回:
```lua
function handle_request(msg)
    SendToClientFast('{"code":0,"msg":"ok"}')
    return true
end
```

**route.lua** — 异步转发 LOGIC, 等回包:
```lua
function handle_request(msg)
    SendToLogic(msg:body(), function(resp)
        return '{"code":0,"msg":"ok","logic":' .. resp .. '}'
    end)
    return true
end
```

**limit.lua** — 限长 + 异步转发:
```lua
function handle_request(msg)
    if #msg:body() > 100 then
        SendToClientFast('{"code":1,"msg":"body too long"}')
        return true
    end
    SendToLogic(msg:body(), function(resp)
        return '{"code":0,"msg":"ok","logic":' .. resp .. '}'
    end)
    return true
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

**裸机对比 (performance, taskset 4-9, INFO, 1 Worker, keep-alive):**

| 端点 | RPS | vs ModuleRaw | 说明 |
|------|----:|:-----------:|------|
| ModuleRaw | 237k | — | picohttpparser + `SendToClientFast`, C++ 4 行 |
| lua_echo | 168k | 71% | 以上 + `lua_pcall`, Lua 3 行 |
| ModuleHello | 156k | 66% | 以上 + `CJsonObject::Parse/Get/Add/ToString` |

> lua_echo 比 ModuleHello 快 8%: `lua_pcall` (29%) < CJsonObject 堆分配 (34%)。

## LuaJIT vs SO vs WASM

| 维度 | SO 模块 | LuaJIT 脚本 | WASM 沙箱 |
|------|:-------:|:-----------:|:---------:|
| 性能 | 🏆 原生 | ✅ 接近 native (JIT) | ⚠️ ~30ns 调用 |
| 热更新 | 进程重启 | ✅ **重载文件** | ✅ **实例替换** |
| 开发效率 | 编译时间⏳ | ✅ **即时修改** | 需编译 wasm |
| 隔离 | ❌ 弱 | ✅ 沙箱 (限制 os/io) | ✅ 强沙箱 |
| 语言 | C/C++ | Lua | Rust/C++/Go |

## Lua 脚本热更新流程

> 设计原则：admin **不直接**发指令给 Worker。admin 只写 etcd，Manager Watch 到变更后自动驱动。
> Worker 收到命令后只重建 Lua VM 中的函数引用（`luaL_unref` + `luaL_loadbuffer`），**不动 .so、不动 VM、不动进程**。

```
POST /api/lua-scripts                         # admin-web
  │
  ├─ 写 .lua 文件 → deploy/{TypeDir}/scripts/echo.lua
  └─ etcd PUT → /thunder/config/module/{NODE_TYPE}
       version++ + script_content
  │
  ▼
Manager Watch 回调 → ConfigUpdated (#126)
  ├─ 比较 old vs new module
  ├─ version 变了 + 有 script_path → 只收集索引，不设 soOrModuleChanged
  └─ SendToWorker(CMD_REQ_RELOAD_LUA, luaMods)      #129
  │
  ▼
Worker CmdReloadLua → LuaReloadScript                #129
  ├─ SetModuleConf(oConf)          ← 同步新 conf 到 Module 实例
  └─ pModule->ReloadScript()
       ├─ luaL_unref(旧 handle_request)              ← 只清函数引用
       ├─ lua_pushnil + lua_setglobal("handle_request") ← 清全局
       ├─ luaL_loadbuffer(新 script_content)         ← 加载新脚本
       ├─ lua_pcall → 定义新 handle_request          ← 执行，VM 不重建
       └─ luaL_ref(新 handle_request)                ← 注册新函数
```

### 与 SO 热更新的区别

> SO 热更新走 `LoadModule(force=true)` → `dlclose` + `dlopen` + `create()` + `Init()`。
> Lua 热更新走 `ReloadScript()` → 只重建 VM 中的函数引用。
> 两者共享同一个 etcd Watch 检测路径，但经过 #126 分流。

| 步骤 | SO 热更新 (`LoadModule`) | Lua 热更新 (`ReloadScript`) |
|------|-------------------------|---------------------------|
| Worker 进程 | 优雅重启 (fork/drain/exit) | ✅ 不动 |
| .so 文件 | `dlclose` + `dlopen` | ✅ 不动 |
| Lua VM | `lua_close` + `luaL_newstate` | ✅ 不动 (`luaL_unref` + `loadbuffer`) |
| JIT trace | 全部清空 | ✅ 保留（trace 按函数缓存） |
| 同 .so 其他 URL | 被 `dlclose` 连带卸载 | ✅ 不受影响 |
| 影响范围 | 所有模块 + 所有连接 | 仅目标 `url_path` |
| 恢复耗时 | ~10s（排空 + fork + init） | <1ms（一个 pcall） |

### 代码位置

| 组件 | 文件 | 关键函数 |
|------|------|---------|
| admin 下发 | `deploy/admin-web/server.py` | `_lua_push()` |
| 变更分流 | `code/Net/src/labor/Manager.cpp` | `ConfigUpdated` 中 `luaChangedIdx` |
| 命令定义 | `code/Net/src/cmd/sys_cmd/CmdReloadLua.cpp` | `AnyMessage()` |
| 命令号 | `code/Net/include/cmd/CW.hpp` | `CMD_REQ_RELOAD_LUA = 41` |
| Worker 入口 | `code/Net/src/labor/Worker.cpp` | `LuaReloadScript()` |
| VM 重载 | `code/HelloHttp/src/ModuleLua/ModuleLua.cpp` | `ReloadScript()` |
| 基类接口 | `code/Net/include/cmd/Module.hpp` | `virtual bool ReloadScript()` |

### 相关 issue

- #125: admin-web 下发路径对齐（`deploy/` 而非 `deploy/admin-web/`）
- #126: Lua 版本变更不触发 Worker 优雅重启（`soOrModuleChanged` 分流）
- #127: 发送 `CMD_REQ_RELOAD_MODULE`（后升级为 #129）
- #129: `CMD_REQ_RELOAD_LUA` → `ReloadScript()`（VM-only，不动 SO）

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
- **lua_pcall vs lua_call vs lua_resume**: 当前两处 (`handle_request` L140, `SendToLogic` 回调 L59) 用 `lua_pcall`(有 errfunc 保护)。`lua_call` 无 errfunc 开销, 实测可 +5-10%, 但 Lua 错误会导致 worker 直接 abort。`lua_resume` 适合回调有耗时逻辑时防阻塞事件循环。可考虑脚本声明 `#resume: true` 按需切换。

## 扩展阅读

- [21-lua-send-to-node-type.md](21-lua-send-to-node-type.md) — Lua 通用跨节点类型发送（`SendToNodeType`），替代硬编码的 `SendToLogic`

## 参考

- OpenResty `content_by_lua` 实现 (ngx_http_lua_module)
- LuaJIT 官方文档: https://luajit.org/
- Thunder `Module` 接口: `code/HelloHttp/src/ModuleRaw/`
