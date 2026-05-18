# wrk test result

- mode: wrk
- target: `http://127.0.0.1:27006/hello/hello`
- args: `-t4 -c100 -d60s`
- requests_per_sec: 122594.76
- latency_avg: 749.92us
- transfer_per_sec: 15.20MB

## raw output

```text
Running 1m test @ http://127.0.0.1:27006/hello/hello
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   749.92us  115.90us   3.76ms   75.14%
    Req/Sec    30.81k     3.28k   42.96k    78.38%
  Latency Distribution
     50%  793.00us
     75%  835.00us
     90%    0.86ms
     99%    0.97ms
  7358133 requests in 1.00m, 0.89GB read
  Socket errors: connect 0, read 0, write 0, timeout 18
Requests/sec: 122594.76
Transfer/sec:     15.20MB

```
