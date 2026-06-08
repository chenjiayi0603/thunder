# wrk test result

- mode: wrk
- target: `http://127.0.0.1:27006/hello/hello`
- args: `-t4 -c100 -d60s`
- requests_per_sec: 218569.03
- latency_avg: 543.07us
- transfer_per_sec: 27.10MB

## raw output

```text
Running 1m test @ http://127.0.0.1:27006/hello/hello
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   543.07us  598.37us  39.08ms   92.03%
    Req/Sec    54.95k    18.37k   78.57k    81.71%
  Latency Distribution
     50%  380.00us
     75%  413.00us
     90%    0.91ms
     99%    3.06ms
  13123481 requests in 1.00m, 1.59GB read
Requests/sec: 218569.03
Transfer/sec:     27.10MB

```
