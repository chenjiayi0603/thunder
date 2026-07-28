# LuaJIT 模块支持

> 源码: `code/HelloHttp/src/ModuleLua/ModuleLua.cpp`, `code/Net/src/labor/Worker.cpp` (LuaReloadScript)

---

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
class ModuleLua : public net::Module {
    virtual bool Init();
    virtual bool AnyMessage(const net::tagMsgShell&, const HttpMsg&) override;
private:
    lua_State* m_pLuaState = nullptr;
    std::string m_strScriptPath;
    int m_iFuncRef = LUA_NOREF;
};
```

### Lua API

```lua
function handle_request(msg)
    -- msg:path()          -- 请求路径
    -- msg:body()          -- 请求 body
    -- msg:method()        -- HTTP method
    -- msg:headers()["key"]-- 请求头

    -- 同步响应:
    SendToClientFast('{"code":0,"msg":"ok"}')

    -- 异步响应 (发到 LOGIC，等回包):
    SendToLogic(msg:body(), function(resp)
        return '{"code":0,"logic":' .. resp .. '}'
    end)
    return true
end
```

### SendToLogic 回调底层

```
Lua:  SendToLogic("body", function(resp) return "response" end)
                      │                          │
                      ▼                          ▼
C++:  lua_SendToLogic(L)
        ├─ luaL_ref(L, LUA_REGISTRYINDEX) → cbRef  // 存注册表
        ├─ 取 __current_shell → tagMsgShell shell   // 捕获进 LogicStep
        ├─ new LogicStep(self, shell, cbRef, body)
        │     ├─ RegisterCallback(step)
        │     └─ Emit → SendToSession("LOGIC")
        │            └─ h.set_seq(GetSequence())    // seq 匹配回包
        │
        └─ LOGIC 回包 → seq 匹配 → LogicStep::Callback
              ├─ lua_rawgeti(L, REGISTRY, m_iCbRef) // 取回调
              ├─ lua_pushlstring(L, oBody.body())    // LOGIC 回包
              ├─ lua_pcall(L, 1, 1, 0)               // callback(resp)
              ├─ SendToClientFast(m_shell, resp)      // 用捕获的 m_shell
              └─ luaL_unref(L, REGISTRY, m_iCbRef)   // 释放
```

| 机制 | 说明 |
|------|------|
| `m_iCbRef` | `luaL_ref` 存注册表，`lua_rawgeti` 取回 |
| `m_shell` 捕获 | LogicStep 构造时保存 tagMsgShell，不依赖全局 __current_shell |
| `seq` 匹配 | GetSequence() 写入 MsgHead.seq，mapCallbackStep.find(seq) 路由 |
| 超时 | step_timeout(5s) → 自动返回 `{"code":1,"msg":"logic timeout"}` |

---

## Lua 脚本热更新 vs SO 热更新

```
POST /api/lua-scripts
  ├─ 写 .lua 文件 → deploy/{TypeDir}/scripts/echo.lua
  └─ etcd PUT → /thunder/config/module/{NODE_TYPE} (version++)

Manager Watch → ConfigUpdated
  ├─ version 变了 + 有 script_path → 收集索引
  └─ SendToWorker(CMD_REQ_RELOAD_LUA, luaMods)

Worker CmdReloadLua → LuaReloadScript
  └─ pModule->ReloadScript()
       ├─ luaL_unref(旧 handle_request)
       ├─ luaL_loadbuffer(新 script_content)
       ├─ lua_pcall → 定义新 handle_request
       └─ luaL_ref(新 handle_request)
```

| 步骤 | SO 热更新 | Lua 热更新 |
|------|----------|----------|
| Worker 进程 | 优雅重启 (fork/drain/exit) | ✅ 不动 |
| .so 文件 | dlclose + dlopen | ✅ 不动 |
| Lua VM | lua_close + luaL_newstate | ✅ 不动 (unref + loadbuffer) |
| 恢复耗时 | ~10s | <1ms |
| 影响范围 | 所有模块 + 所有连接 | 仅目标 url_path |

---

## 性能实测

同机同条件 (performance governor, P-core 绑核, wrk -t4 -c100 -d10s):

```
┌────────────────────────────────────┬──────────┬──────────────────┐
│              方案                  │   RPS    │   vs SO Fast Path│
├────────────────────────────────────┼──────────┼──────────────────┤
│ SO Fast Path (ModuleRaw)           │  234k 🏆 │        —         │
│ ModuleLua (纯返回, 不读 msg)       │  203k    │       87%        │
│ SO 完整路径 (ModuleHello)          │  194k    │       83%        │
│ ModuleLua (读 msg:path + body)     │  179k    │       76%        │
│ Nginx (return 200)                 │  127k    │       54%        │
│ OpenResty LuaJIT (ngx.say)         │  131k    │       56%        │
└────────────────────────────────────┴──────────┴──────────────────┘
```

---

## Lua vs SO 分发策略

| | Lua 脚本 | SO 文件 |
|------|---------|---------|
| 典型大小 | 2-50 KB | 2-50 MB |
| etcd value 上限 (1.5MB) | ✅ 够用 | ❌ 超 1000 倍 |
| 变更频率 | 分钟级 | 天/周级 |
| etcd 方式 | 内容本体直接存 | 只存元数据 (path/version) |
| 分发方式 | etcd watch → 写磁盘 | NFS / Docker 镜像 |
