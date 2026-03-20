./test_helloserver_wrk.sh

output:

Running 10s test @ http://127.0.0.1:27006/hello/hello
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     2.07ms    1.22ms  73.57ms   98.95%
    Req/Sec    12.24k     1.32k   15.25k    98.27%
  Latency Distribution
     50%    2.01ms
     75%    2.08ms
     90%    2.17ms
     99%    3.40ms
  496249 requests in 9.16s, 54.42MB read
  Socket errors: connect 0, read 0, write 0, timeout 100
  Non-2xx or 3xx responses: 496249
Requests/sec:  54192.73
Transfer/sec:      5.94MB
