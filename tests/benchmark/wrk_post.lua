-- wrk Lua: POST /hello/hello with JSON Echo payload
-- Usage: wrk -s wrk_post.lua -t4 -c100 -d30s http://127.0.0.1:27006/hello/hello
wrk.method = "POST"
wrk.body   = '{"option":"Echo","data":"bench"}'
wrk.headers["Content-Type"] = "application/json"
wrk.headers["Connection"]   = "keep-alive"
