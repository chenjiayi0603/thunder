# wrk test result

- mode: wrk
- target: `http://127.0.0.1:27006/hello/hello`
- args: `-t4 -c100 -d60s`
- requests_per_sec: 79231.61
- latency_avg: 3.61ms
- transfer_per_sec: 9.82MB

## raw output

```text
Running 1m test @ http://127.0.0.1:27006/hello/hello
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     3.61ms    6.29ms  57.85ms   86.72%
    Req/Sec    19.91k     6.24k   27.63k    74.25%
  Latency Distribution
     50%  772.00us
     75%    2.22ms
     90%   14.09ms
     99%   26.02ms
  4755864 requests in 1.00m, 589.62MB read
Requests/sec:  79231.61
Transfer/sec:      9.82MB

```
