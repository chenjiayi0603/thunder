# wrk test result

- mode: wrk
- target: `http://127.0.0.1:27006/hello/hello`
- args: `-t4 -c100 -d60s`
- requests_per_sec: 121974.85
- latency_avg: 425.56us
- transfer_per_sec: 15.12MB

## raw output

```text
Running 1m test @ http://127.0.0.1:27006/hello/hello
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   425.56us   67.86us  11.07ms   98.29%
    Req/Sec    30.65k    11.30k   52.99k    50.25%
  Latency Distribution
     50%  424.00us
     75%  437.00us
     90%  448.00us
     99%  509.00us
  7319872 requests in 1.00m, 0.89GB read
Requests/sec: 121974.85
Transfer/sec:     15.12MB

```
