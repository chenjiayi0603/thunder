-- 异步链路: HEllo(Lua) → LOGIC → HEllo(Lua) → 客户端
-- 回调必须 return 应答字符串; C++ LogicStep::Callback 拿到返回值后用捕获的 m_shell
-- 发给客户端 (异步回调时 __current_shell 已清空, 不能用全局 SendToClientFast)。
function handle_request(msg)
    SendToLogic(msg:body(), function(resp)
        return '{"code":0,"msg":"ok","logic":' .. resp .. '}'
    end)
    return true
end
