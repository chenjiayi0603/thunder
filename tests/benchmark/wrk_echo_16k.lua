-- wrk: Echo 16KB payload → Thunder returns ~16.1KB JSON response
wrk.method = "POST"
wrk.body   = '{"option":"Echo","size":16384}'
wrk.headers["Content-Type"] = "application/json"
