-- 异步链路: HEllo(Lua) → LOGIC → HEllo(Lua) → 客户端
-- 回调必须 return 应答字符串; C++ NodeTypeStep::Callback 拿到返回值后用捕获的 m_shell
-- 发给客户端 (异步回调时 __current_shell 已清空, 不能用全局 SendToClientFast)。
--
-- API:
--   SendToNodeType(nodeType, cmd, body, [targetId], [timeout], [callback])
--     nodeType  (string, 必填)  目标节点类型
--     cmd       (number, 必填)  命令字
--     body      (string, 必填)  消息体
--     targetId  (string, 可选)  目标标识（一致性哈希路由）
--     timeout   (number, 可选)  超时秒数（默认 0.5）
--     callback  (function, 可选) 异步回调; 缺省时 fire-and-forget
function handle_request(msg)
    -- 新 API: 通用跨节点类型发送
    SendToNodeType("LOGIC", 10001, msg:body(), function(resp)
        return '{"code":0,"msg":"ok","logic":' .. resp .. '}'
    end)

    -- 带 targetId 示例 (路由到指定用户所在节点):
    -- SendToNodeType("LOGIC", 10001, msg:body(), "user_123", function(resp)
    --     return '{"code":0,"msg":"ok","logic":' .. resp .. '}'
    -- end)

    -- fire-and-forget 示例 (不等待回调):
    -- SendToNodeType("NOTIFY", 20001, '{"event":"user_login"}')

    -- 向后兼容: 原有 SendToLogic 仍可用
    -- SendToLogic(msg:body(), function(resp)
    --     return '{"code":0,"logic":' .. resp .. '}'
    -- end)
    return true
end
