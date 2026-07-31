-- POST 16k binary body → fixed 24B response (fair comparison, no parsing)
wrk.method = "POST"
wrk.body = string.rep("X", 16384)
wrk.headers["Content-Type"] = "application/octet-stream"
