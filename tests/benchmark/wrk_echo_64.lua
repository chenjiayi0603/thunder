-- wrk: Echo 64B payload → Thunder returns ~150B JSON response
wrk.method = "POST"
wrk.body   = '{"option":"Echo","size":64}'
wrk.headers["Content-Type"] = "application/json"
