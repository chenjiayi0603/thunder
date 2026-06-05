# Thunder Issue List

> 最后更新: 2026-06-05
> 来源: 代码审查、测试发现、架构评估

---

## 🔴 严重 (已修复)

### #19 注册中心实际为空 — 注册键随旧租约过期被删
- **文件**: `code/Net/src/labor/EtcdCenterConnector.cpp`
- **状态**: ✅ 已修复 (dangling ref crash 也已修复: `a27c83d`)
- **根因**: `DoRegister` 幂等分支无视租约归属; `[&]` lambda 捕获导致 async 回调 crash
- **修复**: `DecideRegAction` + `RebindRegistration` + 按值捕获

### #20 watch 每秒重连风暴 — 未处理 compact_revision 取消
- **文件**: `code/Net/src/labor/EtcdCenterConnector.cpp`
- **状态**: ✅ 已修复
- **根因**: 未处理 etcd `{"canceled":true,"compact_revision":N}`; bundled libcurl 不挂住 chunked 流
- **修复**: `EtcdWatcher` 替代 curl 线程; `ParseWatchControl` 处理 compact_revision

### #21 etcd 容器健康检查永远失败
- **文件**: `docker/docker-compose.yml`
- **状态**: ✅ 已修复
- **根因**: etcd v3.5.x distroless 镜像无 `/bin/sh`/`curl`
- **修复**: 改用 `etcdctl endpoint health`

### #24 传输层 curl → Thunder 原生 HTTP(HttpCodec)
- **状态**: ✅ 已完成 (PR #25)
- **修复**: `EtcdHttpConn`(短请求) + `EtcdWatcher`(watch 流式)

---

## 🟡 中

### #4 ShmRingQueue Create/Destroy 尺寸参数重复硬编码
- **文件**: `code/test/labor/test_shm_queue.cpp`
- **状态**: ✅ 已修复 (`7bf56aa`)
- **根因**: `4096` 在测试中重复 10+ 次硬编码; Destroy 已从 ctrl 读尺寸但测试未覆盖非默认
- **修复**: 具名常量 + `CreateDestroyNonDefaultSize` 测试

### #25 EtcdHttpConn 排队请求静默丢弃
- **文件**: `code/Net/src/labor/EtcdHttpConn.cpp:58`
- **状态**: ✅ 已修复
- **问题**: `Reset()`/`Close()` 不清空 `m_queue`,连接关闭时排队请求的 callback 永不执行
- **场景**: Post() 入队后连接断开 → callback 静默丢失 → `Init` 的 `AsyncLeaseGrant` callback 丢失 → leaseId 永不设置
- **修复**: `Reset()` 中遍历 `m_queue`,对每个 Pending 调用 `cb(false, 0, "")`

### #26 EtcdWatcher chunked parser 无缓冲区上限
- **文件**: `code/Net/src/labor/EtcdWatcher.cpp:161`
- **状态**: ✅ 已修复
- **问题**: chunked 编码 hex 尺寸行 `m_chunkBuf` 无大小限制,非标准响应→无限堆积→OOM
- **场景**: 畸形 chunked 响应发极长 hex 尺寸行(无 `\r\n`) → `m_chunkBuf` 持续 append → 内存耗尽 crash
- **修复**: `m_chunkBuf.size() > 8192` 时 Reconnect

### #27 异步注册链异常时 m_regInProgress 永久为 true
- **文件**: `code/Net/src/labor/EtcdCenterConnector.cpp:428`
- **状态**: ✅ 已修复
- **问题**: `OnRegEnsureLease`/`OnRegQuery`/`OnRegScan` 回调中若 Json 解析抛异常跳过 `OnRegDone` → `m_regInProgress` 永不 reset → 后续 `DoRegister` 全部被跳过 → 节点永不注册
- **修复**: 所有 `OnReg*` 回调加 try-catch,finally 块中 reset `m_regInProgress` 并 emit 错误事件

### #28 EtcdWatcher base64 每次重连重复编码
- **文件**: `code/Net/src/labor/EtcdWatcher.cpp:82`
- **状态**: ✅ 已修复
- **问题**: watch 每次重连都重新 base64-encode `m_prefix`/`m_rangeEnd`(常量字符串),冗余 CPU
- **修复**: 缓存 base64 结果在成员变量中

### #29 OnRegQuery Fresh 路径 keepalive 失败不阻塞注册
- **文件**: `code/Net/src/labor/EtcdCenterConnector.cpp:456`
- **状态**: ✅ 已修复
- **问题**: Fresh 路径 `AsyncKeepAlive([...](bool ok) { (void)ok; ... OnRegDone(true); })` — keepalive 失败也被标记为注册成功。这是设计选择但值得注释说明
- **修复**: already intentional, just document

---

## ✅ 已修复

### #22 watch 退化为 2s 轮询 → 实现真·长连接 watch
- **状态**: 🟢 follow-up issue
- **原因**: bundled curl 8.21 不挂住 chunked 流,换用 thunder 原生 HTTP 后已解决

### k8s 部署适配
- **文件**: `docs/architecture/evaluations/thunder_on_k8s_evaluation.md`
- **状态**: 📋 评估完成,未实施
- **结论**: Interface 可放 k8s, Hello/Logic 应留裸机

---

## 🟡 后端日志异常 (2026-06-05 冒烟后发现)

### #30 Manager bind Address already in use — 重启竞态
- **文件**: `code/Net/src/labor/Manager.cpp:1199`
- **状态**: ✅ 已修复
- **问题**: Docker compose restart 时旧进程未完全释放端口 → `bind()` fail → exit
- **修复**: `setsockopt(SO_REUSEADDR)` 在 bind 前设置端口重用

### #31 unkonw cmd 7 from worker — WARN 级别过高
- **文件**: `code/Net/src/labor/Manager.cpp:2444`
- **状态**: ✅ 已修复
- **问题**: Manager 不识别 Worker 发来的 cmd 7, 每次打 WARN 刷日志
- **修复**: `LOG4_WARN` → `LOG4_TRACE` (未处理命令打 TRACE 即可)

### #32 node_id 分配日志不够显著
- **文件**: `code/Net/src/labor/EtcdCenterConnector.cpp:522`
- **状态**: ✅ 已修复
- **问题**: OnRegDone 只打 `注册成功 nodeId=247`, 缺少 node_type/ip:port/lease 上下文
- **修复**: `<<< node_id 分配完成: 247 (type=LOGIC addr=127.0.0.1:16068 lease=...) >>>`

### #33 ParseFromArray failed from fd 8 — Worker 管道数据错位
- **文件**: `code/Net/src/labor/Manager.cpp:2325`
- **状态**: ✅ 已修复
- **问题**: Worker 重启/初始化时管道二进制数据可能错位, MsgHead 解析失败 → 旧代码 DestroyConnect(摧毁 Manager-Worker 连接) → 需全进程重启才恢复
- **修复**: `LOG4_ERROR→WARN`; 不再 DestroyConnect, 改为 `SkipBytes(1)` 重试(MsgHead 失败) / `SkipBytes(header+body)` 跳过(MsgBody 失败)。逻辑:管道数据错位是瞬态的,跳字节后下次消息会对齐
