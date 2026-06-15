-- SendToNodeType 三种模式示例：
--   1. Fire-and-forget: 不等待回调
--   2. 异步回调到 LOGIC: 等待 LOGIC 响应后回客户端
--   3. 异步回调 + targetId: 一致性哈希路由
--
-- 请求 body JSON:
--   {"mode":"fire_forget"}   → 仅广播, 立即返回
--   {"mode":"async"}         → 发 LOGIC, 等回包
--   {"mode":"async_target"}  → 发 LOGIC + targetId, 等回包

-- 简易 JSON 字段提取 (不依赖外部库, 仅提取顶层 string/number 字段)
local function json_extract_field(s, field)
    if not s then return nil end
    -- 匹配 "field": "value" 或 'field': 'value'
    local pattern = [=["([^"])%s*:\s*"([^"]+)"]=]
    local _, _, val = s:find('"' .. field .. '"%s*:%s*"([^"]+)"')
    if val then return val end
    -- 匹配 "field": number
    local num = s:match('"' .. field .. '"%s*:%s*([%d%.]+)')
    if num then return tonumber(num) end
    return nil
end

function handle_request(msg)
    local body_str = msg:body()
    local mode = json_extract_field(body_str, "mode") or ""
    local targetId = json_extract_field(body_str, "targetId") or ""

    if mode == "fire_forget" then
        -- Fire-and-forget: 广播到 LOGIC 所有节点, 不等待回调
        local ok = SendToNodeType("LOGIC", 10001, body_str)
        if ok then
            SendToClientFast('{"code":0,"msg":"fire_forget_ok"}')
        else
            SendToClientFast('{"code":1,"msg":"fire_forget_fail"}')
        end
        return true

    elseif mode == "async" then
        -- 异步回调: 发到 LOGIC (轮询选一个节点), 等待响应
        -- 注意: SendToNodeType 可能因 LOGIC 不可达而返回 false。
        -- 必须检查返回值，否则 handle_request return true 后框架以为
        -- 异步处理中但实际无 Step 注册 → 永远无超时 → 连接挂起。
        local ok = SendToNodeType("LOGIC", 10001, body_str, function(resp)
            return '{"code":0,"mode":"async","logic":' .. resp .. '}'
        end)
        if not ok then
            SendToClientFast('{"code":1,"msg":"async_send_failed"}')
        end
        return true

    elseif mode == "async_target" then
        -- 异步回调 + targetId: 一致性哈希路由
        local target = targetId ~= "" and targetId or "test_user"
        local ok = SendToNodeType("LOGIC", 10001, body_str, target, function(resp)
            return '{"code":0,"mode":"async_target","targetId":"' .. target .. '","logic":' .. resp .. '}'
        end)
        if not ok then
            SendToClientFast('{"code":1,"msg":"async_target_send_failed"}')
        end
        return true

    else
        -- 缺省: 同步返回当前模式列表 (纯查询)
        SendToClientFast('{"code":0,"msg":"node_type_demo","modes":["fire_forget","async","async_target"]}')
        return true
    end
end
