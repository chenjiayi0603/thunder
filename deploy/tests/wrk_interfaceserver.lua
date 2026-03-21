-- wrk 压测 Interface 模块 /Interface/gentoken（ModuleInterface）
-- 服务端需解析 JSON body，故使用 POST + application/json
-- 用法: deploy/start_interfaceserver.sh（默认 -s wrk_interfaceserver.lua）

wrk.method = "POST"
wrk.body   = '{"option":"Echo"}'
wrk.headers["Content-Type"] = "application/json"
