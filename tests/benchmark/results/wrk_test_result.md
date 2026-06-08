# wrk test result

- mode: wrk
- target: `http://127.0.0.1:27006/hello/hello`
- args: `-t4 -c100 -d60s`
- requests_per_sec: 265882.79
- latency_avg: 377.77us
- transfer_per_sec: 32.96MB

## raw output

```text
Running 1m test @ http://127.0.0.1:27006/hello/hello
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   377.77us  138.30us  16.57ms   95.84%
    Req/Sec    66.84k     5.68k   81.08k    79.54%
  Latency Distribution
     50%  369.00us
     75%  417.00us
     90%  442.00us
     99%    0.90ms
  15959894 requests in 1.00m, 1.93GB read
Requests/sec: 265882.79
Transfer/sec:     32.96MB

```
