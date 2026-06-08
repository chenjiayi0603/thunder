# wrk test result

- mode: wrk
- target: `http://127.0.0.1:27006/hello/hello`
- args: `-t4 -c100 -d60s`
- requests_per_sec: 275922.30
- latency_avg: 360.72us
- transfer_per_sec: 34.21MB

## raw output

```text
Running 1m test @ http://127.0.0.1:27006/hello/hello
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   360.72us   78.63us  10.70ms   97.46%
    Req/Sec    69.35k     2.97k   80.34k    79.33%
  Latency Distribution
     50%  355.00us
     75%  381.00us
     90%  394.00us
     99%  505.00us
  16560009 requests in 1.00m, 2.00GB read
Requests/sec: 275922.30
Transfer/sec:     34.21MB

```
