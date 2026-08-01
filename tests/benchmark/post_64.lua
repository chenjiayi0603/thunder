-- POST 64 binary body → fixed 24B response (fair comparison, no parsing)
wrk.method = "POST"
wrk.body = string.rep("X", 64)
wrk.headers["Content-Type"] = "application/octet-stream"
