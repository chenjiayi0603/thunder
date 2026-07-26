function handle_request(msg)
  local body = '{"genkey":"1","token":"canary_test_token","key":"canary_test_key","address":"192.168.3.61"}'
  local function on_response(resp_body)
    -- 直接返回 Logic 的响应给客户端（不通过 SendToClientFast，避免双重发送）
    return resp_body
  end
  local ok = SendToNodeType("LOGIC", 10001, body, 5, on_response)
  if not ok then
    return false -- 让框架返回 500
  end
  return true
end
