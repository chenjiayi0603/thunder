-- POST 4k binary body → fixed 24B response (fair comparison, no parsing)
wrk.method = "POST"
wrk.body = string.rep("X", 4096)
wrk.headers["Content-Type"] = "application/octet-stream"
