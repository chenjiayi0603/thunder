-- wrk: Echo 4KB payload → Thunder returns ~4.1KB JSON response
wrk.method = "POST"
wrk.body   = '{"option":"Echo","size":4096}'
wrk.headers["Content-Type"] = "application/json"
