#存储日志格式 
一个日志占两行，一行消息头一行消息体
日志文件名为时间戳（分钟）、节点标识符、工作者号表示，如下：
Log_201710160001_192.168.10.20:17009.0.dat 
#消息头 
消息头格式如下：
{"index":{"_index":"db_click_trace","_type":"tb_click_trace","_id":"1740062366260124632209022432"},"user":{"app_id":1,"user_id":"5e4ef965-d2e1-4cbb-b9ec-46d2ba62a4c9","device_id":"","session_id":"1508926674591"}}
# 消息体 
消息体格式如下：
{"time":"2017-10-25 18:18:01","page":"/#/investList","session_id":"1508926674591","device_id":"5e4ef965-d2e1-4cbb-b9ec-46d2ba62a4c9","channel":"H5","event_id":"HIL_BatchInvest"}


# 写性能 

##    每个消息同步写 ##
 TestWriteLogs() TestWriteLogs AppendLog uiCounter(10000) strLogInfo.size(693) sizeof(LogDataHeader):8 log_info:693
 CustomClock TestWriteLogs use time(2561.010010) ms
                 文件6.68mb
 qps:3904.7
                   吞吐量：2.6 Mb/s

##                 每个消息同步写 + 加合法性检查 ##
TestWriteLogs() TestWriteLogs AppendLog uiSyncLog(1) uiCounter(10000) strLogInfo.size(693) sizeof(LogDataHeader):8 log_info:693
CustomClock TestWriteLogs use time(2635.263916) ms
                     文件6.68mb
 qps:3794.6
                   吞吐量：2.53 Mb/s

##          每个消息异步写 ##
   TestWriteLogs() TestWriteLogs AppendLog uiCounter(10000) strLogInfo.size(693) sizeof(LogDataHeader):8 log_info:693
   CustomClock TestWriteLogs use time(84.639000) ms
                文件6.68mb
    qps:118147.4
                       吞吐量：78.9 Mb/s

##                 每个消息异步写 + 加合法性检查 ##
 TestWriteLogs() TestWriteLogs AppendLog uiCounter(10000) strLogInfo.size(693) sizeof(LogDataHeader):8 log_info:693
 CustomClock TestWriteLogs use time(184.009995) ms
               文件6.68mb
qps:54347.8
                   吞吐量：36.3 Mb/s



## 每个消息异步写 （新） ##
TestWriteLogs AppendLog uiCounter(10000) strLogInfo.size(673) sizeof(LogDataHeader):8 log_info:568 m_uiVerifyLog(0)
 RunClock use time(9.402000) ms
文件6.28mb
约
qps:1000000
吞吐量：628 Mb/s

## 每个消息异步写 + 加合法性检查（新） ##
TestWriteLogs() TestWriteLogs AppendLog uiCounter(10000) strLogInfo.size(801) sizeof(LogDataHeader):8 log_info:696 m_uiVerifyLog(1)
 RunClock use time(134.341003) ms
文件7.5mb
约
qps:74626
吞吐量：56 Mb/s