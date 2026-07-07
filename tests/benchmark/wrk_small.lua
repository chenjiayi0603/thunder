-- wrk Lua: POST /hello/hello with 37B JSON Echo payload (small packet)
-- Usage: wrk -t4 -c100 -d30s -s wrk_small.lua http://127.0.0.1:27006/hello/hello
wrk.method = "POST"
wrk.body   = '{"option":"Echo","message":"hello"}'
wrk.headers["Content-Type"] = "application/json"
