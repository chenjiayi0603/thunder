wrk.method = "POST"
wrk.body   = '{"option":"Echo","message":"' .. string.rep("x", 220) .. '"}'
wrk.headers["Content-Type"] = "application/json"
