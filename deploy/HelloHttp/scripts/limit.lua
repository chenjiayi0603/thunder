function handle_request(msg)
    local body = msg:body()
    if #body > 100 then
        SendToClientFast('{"code":1,"msg":"body too long"}')
        return true
    end
    -- 短 body: 转发 LOGIC 同时返回
    SendToLogic(body, function(resp)
        -- LOGIC 响应回来时处理 (异步)
    end)
    SendToClientFast('{"code":0,"msg":"ok"}')
    return true
end
