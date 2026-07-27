# Lua 跨节点类型发送

> 源码: `code/HelloHttp/src/ModuleLua/` (lua_SendToNodeType)

---

## API

```lua
SendToNodeType(nodeType, cmd, body, [targetId], [timeout], [callback])
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|:----:|------|
| `nodeType` | string | ✓ | 目标节点类型，如 `"LOGIC"`、`"NOTIFY"` |
| `cmd` | number | ✓ | 命令字 |
| `body` | string | ✓ | 消息体 |
| `targetId` | string | | 目标标识，用于一致性哈希路由 |
| `timeout` | number | | 超时秒数（默认 0.5） |
| `callback` | function | | 异步回调；缺省时 fire-and-forget |

返回值: `bool` — 消息是否成功投递

---

## 三种调用模式

### 1. 异步回调

```lua
local ok = SendToNodeType("LOGIC", 10001, msg:body(), function(resp)
    return '{"code":0,"data":' .. resp .. '}'
end)
```

框架通过 `GetSequence()` 设置消息 seq，远端回包时 seq 匹配 → `Callback()` 触发 Lua 回调。回调保存在 Lua registry，回调结束后 `luaL_unref` 清理。

### 2. 带 targetId（一致性哈希路由）

```lua
SendToNodeType("LOGIC", 10001, body, "user_123", function(resp)
    return '{"code":0,"data":' .. resp .. '}'
end)
```

`targetId` 写入 `MsgBody.targetid`，`SendToSession` 根据 targetId 做一致性哈希/取模选出目标节点。适用于 Session 亲和。

### 3. Fire-and-forget（广播）

```lua
SendToNodeType("NOTIFY", 20001, '{"event":"user_login"}')
```

不等待响应，无回调。消息通过 `SendToSession` 广播到对应类型的所有节点。
