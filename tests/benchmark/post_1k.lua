-- POST 1k binary body → fixed 24B response (fair comparison, no parsing)
wrk.method = "POST"
wrk.body = string.rep("X", 1024)
wrk.headers["Content-Type"] = "application/octet-stream"
