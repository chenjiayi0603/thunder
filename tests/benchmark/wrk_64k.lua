-- wrk Lua: POST /hello/hello with 64KB JSON Echo payload (super packet)
-- Usage: wrk -t4 -c100 -d15s -s wrk_64k.lua http://127.0.0.1:27006/hello/hello
wrk.method = "POST"
wrk.body   = '{"option":"Echo","message":"' .. string.rep("x", 65536) .. '"}'
wrk.headers["Content-Type"] = "application/json"
