-- wrk: Echo 1KB payload → Thunder returns ~1.1KB JSON response
wrk.method = "POST"
wrk.body   = '{"option":"Echo","size":1024}'
wrk.headers["Content-Type"] = "application/json"
