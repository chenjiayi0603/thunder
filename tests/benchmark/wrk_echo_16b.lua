-- wrk: Echo 16B payload → Thunder returns ~102B JSON response
wrk.method = "POST"
wrk.body   = '{"option":"Echo","size":16}'
wrk.headers["Content-Type"] = "application/json"
