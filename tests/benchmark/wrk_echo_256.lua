-- wrk: Echo 256B payload → Thunder returns ~350B JSON response
wrk.method = "POST"
wrk.body   = '{"option":"Echo","size":256}'
wrk.headers["Content-Type"] = "application/json"
