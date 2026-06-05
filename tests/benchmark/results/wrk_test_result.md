# wrk test result

- mode: wrk
- target: `http://127.0.0.1:27006/hello/hello`
- args: `-t4 -c100 -d60s`
- requests_per_sec: 125541.29
- latency_avg: 811.39us
- transfer_per_sec: 15.56MB

## raw output

```text
Running 1m test @ http://127.0.0.1:27006/hello/hello
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   811.39us  606.68us  75.87ms   96.67%
    Req/Sec    31.55k     3.94k   36.46k    88.25%
  Latency Distribution
     50%  753.00us
     75%  775.00us
     90%  825.00us
     99%    2.21ms
  7534000 requests in 1.00m, 0.91GB read
Requests/sec: 125541.29
Transfer/sec:     15.56MB

```
