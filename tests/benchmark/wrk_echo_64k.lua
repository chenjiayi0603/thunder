-- wrk: Echo 64KB payload → Thunder returns ~64.1KB JSON response
wrk.method = "POST"
wrk.body   = '{"option":"Echo","size":65536}'
wrk.headers["Content-Type"] = "application/json"
