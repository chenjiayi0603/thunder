-- wrk Lua: POST /hello/hello with 4KB JSON Echo payload (large packet)
-- Usage: wrk -t4 -c100 -d15s -s wrk_4k.lua http://127.0.0.1:27006/hello/hello
wrk.method = "POST"
wrk.body   = '{"option":"Echo","message":"' .. string.rep("x", 4096) .. '"}'
wrk.headers["Content-Type"] = "application/json"
