-- wrk 压测 Hello 模块 /hello/hello（ModuleHello）
-- 服务端需解析 JSON body，故使用 POST + application/json
-- 用法见 pytest 用例 tests/e2e/test_wrk_smoke.py

wrk.method = "POST"
wrk.body   = '{"option":"Echo"}'
wrk.headers["Content-Type"] = "application/json"

-- 可选：在响应阶段统计非 2xx（wrk 默认只汇总延迟与 QPS）
-- status = function(status, headers, body) end
