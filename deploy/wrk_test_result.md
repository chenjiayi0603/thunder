./test_helloserver_wrk.sh

output:

Running 10s test @ http://127.0.0.1:27006/hello/hello
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     2.09ms    1.31ms  78.94ms   98.86%
    Req/Sec    12.23k   587.74    16.13k    91.07%
  Latency Distribution
     50%    2.04ms
     75%    2.11ms
     90%    2.20ms
     99%    3.84ms
  490347 requests in 10.10s, 53.78MB read
  Non-2xx or 3xx responses: 490347
Requests/sec:  48551.18
Transfer/sec:      5.32MB
