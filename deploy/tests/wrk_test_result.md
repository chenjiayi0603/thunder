# wrk test result

- mode: wrk
- target: `http://127.0.0.1:27006/hello/hello`
- args: `-t4 -c100 -d60s`
- requests_per_sec: 78566.80
- latency_avg: 1.27ms
- transfer_per_sec: 9.74MB

## raw output

```text
Running 1m test @ http://127.0.0.1:27006/hello/hello
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.27ms  263.79us  39.26ms   91.18%
    Req/Sec    19.75k     1.35k   23.84k    68.62%
  Latency Distribution
     50%    1.24ms
     75%    1.34ms
     90%    1.47ms
     99%    2.29ms
  4716695 requests in 1.00m, 584.76MB read
Requests/sec:  78566.80
Transfer/sec:      9.74MB

```
