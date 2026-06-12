function handle_request(msg)
    local body = msg:body()
    if #body > 100 then
        SendToClientFast('{"code":1,"msg":"body too long"}')
        return true
    end
    -- 短 body: 转发 LOGIC, 等响应回来再回客户端
    SendToLogic(msg:body(), function(resp)
        return '{"code":0,"msg":"ok","logic":' .. resp .. '}'
    end)
    return true
end
