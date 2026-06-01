# wrk test result

- mode: wrk
- target: `http://127.0.0.1:27006/hello/hello`
- args: `-t4 -c100 -d60s`
- requests_per_sec: 158309.21
- latency_avg: 629.62us
- transfer_per_sec: 19.63MB

## raw output

```text
Running 1m test @ http://127.0.0.1:27006/hello/hello
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   629.62us   69.45us  11.65ms   96.18%
    Req/Sec    39.79k     1.31k   44.08k    83.75%
  Latency Distribution
     50%  625.00us
     75%  641.00us
     90%  657.00us
     99%  745.00us
  9501002 requests in 1.00m, 1.15GB read
Requests/sec: 158309.21
Transfer/sec:     19.63MB

```
