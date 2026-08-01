-- POST 64k binary body → fixed 24B response (fair comparison, no parsing)
wrk.method = "POST"
wrk.body = string.rep("X", 65536)
wrk.headers["Content-Type"] = "application/octet-stream"
