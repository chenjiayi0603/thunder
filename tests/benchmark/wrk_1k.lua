wrk.method = "POST"
wrk.body   = '{"option":"Echo","message":"' .. string.rep("x", 1000) .. '"}'
wrk.headers["Content-Type"] = "application/json"
