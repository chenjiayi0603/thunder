# wrk test result

- mode: wrk
- target: `http://192.168.3.61:30006/hello/hello`
- args: `-t4 -c100 -d60s`
- requests_per_sec: 50995.24
- latency_avg: 4.30ms
- transfer_per_sec: 6.32MB

## raw output

```text
Running 1m test @ http://192.168.3.61:30006/hello/hello
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     4.30ms    6.58ms 101.25ms   86.23%
    Req/Sec    12.82k     6.44k   25.47k    74.58%
  Latency Distribution
     50%    2.27ms
     75%    2.43ms
     90%   15.88ms
     99%   26.80ms
  3060677 requests in 1.00m, 379.46MB read
Requests/sec:  50995.24
Transfer/sec:      6.32MB

```
