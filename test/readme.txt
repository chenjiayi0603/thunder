[chen@localhost test]$ ./siegeTest.sh testfile 200 200 
length:
0
file testfile 200 200
/usr/local/bin/siege -c 200 -r 200 http://192.168.1.106:17137/hello/hello POST {
    "hello": "you"
}
** SIEGE 4.0.3rc4
** Preparing 200 concurrent users for battle.
The server is now under siege...
Transactions:                  40000 hits
Availability:                 100.00 %
Elapsed time:                  22.73 secs
Data transferred:               0.80 MB
Response time:                  0.10 secs
Transaction rate:            1759.79 trans/sec
Throughput:                     0.04 MB/sec
Concurrency:                  174.30
Successful transactions:       40000
Failed transactions:               0
Longest transaction:            3.99
Shortest transaction:           0.00