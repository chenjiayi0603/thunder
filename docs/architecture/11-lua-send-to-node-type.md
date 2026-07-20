# Lua 跨节点类型发送 — 设计文档

> 日期: 2026-06-15 | 状态: ✅ 已实现

## 背景

`ModuleLua` 最初仅支持硬编码向 `"LOGIC"` 节点类型发送消息（`SendToLogic`），无法通用于其他节点类型。随着 Lua 模块在路由、通知、事件上报等场景的扩展，需要提供通用的跨节点类型发送能力，行为与 C++ 协程接口 `SendToInternalByNodeTypeAsync` 一致。

## API 设计

### Lua 全局函数

```lua
-- 通用跨节点类型发送
SendToNodeType(nodeType, cmd, body, [targetId], [timeout], [callback])
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|:----:|------|
| `nodeType` | string | ✓ | 目标节点类型，如 `"LOGIC"`、`"NOTIFY"` |
| `cmd` | number | ✓ | 命令字 |
| `body` | string | ✓ | 消息体 |
| `targetId` | string | | 目标标识，用于一致性哈希路由 |
| `timeout` | number | | 超时秒数（默认 0.5） |
| `callback` | function | | 异步回调函数；缺省时 fire-and-forget |

### 返回值

- `bool` — 消息是否成功投递

### 三种调用模式

**1. 异步回调（向一个节点发送并等待响应）**

```lua
local ok = SendToNodeType("LOGIC", 10001, msg:body(), function(resp)
    -- resp: 远端节点返回的响应体字符串
    -- 必须 return 一个字符串作为客户端 HTTP 响应
    return '{"code":0,"data":' .. resp .. '}'
end)
```

框架通过 `GetSequence()` 设置消息 seq，远端回包时 seq 匹配，触发 `Callback()` 调用 Lua 回调。`callback` 保存在 Lua registry 中，回调结束后自动 `luaL_unref` 清理。

**2. 带 targetId（一致性哈希路由）**

```lua
SendToNodeType("LOGIC", 10001, body, "user_123", function(resp)
    return '{"code":0,"data":' .. resp .. '}'
end)
```

`targetId` 写入 `MsgBody.targetid`，`SendToSession` 根据 `targetId` 做一致性哈希/取模选出目标节点。适用于 Session 亲和行为。

**3. Fire-and-forget（广播到某类型全部节点，不等待响应）**

```lua
SendToNodeType("NOTIFY", 20001, '{"event":"user_login"}')
```

无 callback 时走 `SendToNodeType` 广播到该类型所有节点（除自身），不注册回调。

## 实现架构

### 类图

```
┌─────────────────────────────────────────────────────────────┐
│                    ModuleLua                                 │
│  lua_State* m_pLua                                          │
│  ┌───────────────────────────────────────────────┐          │
│  │  Lua Registry                                  │          │
│  │  ├── __module_instance → ModuleLua*            │          │
│  │  ├── __current_shell  → tagMsgShell*           │          │
│  │  └── cbRef_N          → Lua callback function  │          │
│  └───────────────────────────────────────────────┘          │
└──────────────┬──────────────────────────────────────────────┘
               │
               │ 注册全局函数
               ├── SendToNodeType()
               ├── SendToLogic()     ← 兼容封装
               └── SendToClientFast()
               │
               ▼
┌─────────────────────────────────────────────────────────────┐
│  NodeTypeStep (继承 net::Step)                               │
│                                                             │
│  Emit():                                                     │
│    ┌─ 有 callback → SendToSession(nodeType, head, body)     │
│    │               (一致性哈希/轮询 → 选一个节点)             │
│    │                                                         │
│    └─ 无 callback → SendToNodeType(nodeType, head, body)    │
│                    (广播 → 全部节点)                          │
│                                                             │
│  Callback():                                                 │
│    ┌─ lua_rawgeti → 取 callback 函数                         │
│    ├─ lua_pushlstring → 推入响应体                           │
│    ├─ lua_pcall → 执行 Lua 回调                              │
│    └─ lua_tolstring → 取返回值, SendToClientFast 发回客户端  │
│                                                             │
│  Timeout():                                                  │
│    └─ SendToClientFast 发超时 JSON                           │
│       luaL_unref 清理回调                                    │
└─────────────────────────────────────────────────────────────┘
```

### 可选参数解析

Lua C 函数 `lua_SendToNodeType` 通过类型检查解析第 4~N 个可选参数：

```
Position 4..N:
  ├─ string   → targetId
  ├─ number   → timeout (秒)
  └─ function → callback (luaL_ref 存入 registry)
```

无函数重载的 Lua 中，这种模式兼容多种调用约定：

```lua
SendToNodeType("T", cmd, body)
SendToNodeType("T", cmd, body, cb)
SendToNodeType("T", cmd, body, targetId, cb)
SendToNodeType("T", cmd, body, targetId, timeout, cb)
```

### Step 生命周期

```
Lua: SendToNodeType(...)
  │
  ▼
C++: lua_SendToNodeType()
  │
  ├─ new NodeTypeStep(...)
  ├─ RegisterCallback(step)    ← 框架持有 unique_ptr
  ├─ step->Emit(0)             ← 发送消息
  │     │
  │     ├─ 有 callback → SendToSession() → 等待回调
  │     │                                  │
  │     │                          ┌───────┘
  │     │                          ▼
  │     │                    Callback() → lua_pcall(cb, resp)
  │     │                          │
  │     │                          └─ 返回值 → SendToClientFast()
  │     │
  │     └─ 无 callback → SendToNodeType() → 完成（不注册超时回调）
  │
  └─ 返回 bool 给 Lua
```

## 路由行为

| 模式 | 调用 | 路由方式 | 回调 | 超时 |
|------|------|----------|:----:|:----:|
| 异步回调 | `SendToNodeType("LOGIC", ...)` + callback | `SendToSession` → 按 targetId 一致性哈希/无 targetId 轮询 | ✓ | 默认 0.5s |
| Fire-and-forget | `SendToNodeType("NOTIFY", ...)` 无 callback | `SendToNodeType` → 广播到该类型全部节点 | ✗ | N/A |

## 兼容性

### `SendToLogic` 向后兼容

```lua
-- OLD: 硬编码 LogicStep
SendToLogic(body, callback)

-- NEW: 委托给 NodeTypeStep
SendToLogic(body, callback)
  → NodeTypeStep("LOGIC", GET_TOKEN_GEN(10001), body, "", 0.5, cbRef)
```

完全保持语义一致：`cmd=10001`、`SendToSession("LOGIC", ...)`、回调生命周期管理不变。

### 与 C++ 协程接口的对应关系

| C++ | Lua |
|-----|-----|
| `co_await SendToInternalByNodeTypeAsync("LOGIC", head, body)` | `SendToNodeType("LOGIC", cmd, body, callback)` |
| fire-and-forget（不等待响应） | `SendToNodeType("NOTIFY", cmd, body)` |
| `SendToSession(nodeType, head, body)` 设 targetId | `SendToNodeType("LOGIC", cmd, body, targetId, callback)` |
| 超时参数 | `SendToNodeType("LOGIC", cmd, body, targetId, timeout, callback)` |

## 涉及文件

| 文件 | 变更 |
|------|------|
| `code/HelloHttp/src/ModuleLua/ModuleLua.cpp` | 新增 `NodeTypeStep` 类、`lua_SendToNodeType` 函数；重构 `SendToLogic` 为委托调用 |
| `deploy/HelloHttp/scripts/route.lua` | 更新示例注释，展示新 API |

## 验收

- [x] Lua 脚本能 `SendToNodeType("LOGIC", ...)` 发送消息到指定节点类型
- [x] 支持异步等待返回（callback 参数）
- [x] 支持 fire-and-forget（无 callback 参数）
- [x] 兼容现有 `SendToLogic` 接口
- [x] 编译通过
