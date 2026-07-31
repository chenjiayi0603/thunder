-- wrk: GET /hello/raw fast-path (no body, 24B response)
-- Usage: wrk -t4 -c100 -d10s -s wrk_raw.lua http://IP:PORT/hello/raw
wrk.method = "GET"
