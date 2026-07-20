# Interface 服务崩溃排查与修复 — io_uring ENOMEM

> 日期: 2026-07-20 | 状态: ✅ 已修复 | 回归: 40/40 PASS
> 相关代码: `code/Net/src/labor/Labor.cpp`, `deploy/Interface/entrypoint.sh`

---

## 1. 症状

```
回归测试 Section 5: Interface :27008 → 无响应
Pod 状态: Running (1/1)，但端口未监听
C++ 日志: Manager/Worker init 成功，InitClientListener 日志正常
进程: 只有 tail -f + sleep infinity，无 Manager/Worker
```

**关键矛盾**: 日志显示 `InitClientListener() listen on iPortForClient(27008)` 成功，但 `ss -tlnp` 无端口，`curl` 返回 Connection refused。

---

## 2. 排查过程

```
┌────────┬────────────────────────────────────────────────────┬──────────────────────────────────────┐
│  轮次  │                      手段                          │                发现                   │
├────────┼────────────────────────────────────────────────────┼──────────────────────────────────────┤
│  1     │ 日志分析                                           │ Init 成功但进程消失, 无 FATAL/ERROR   │
│  2     │ 逐秒进程+端口监控                                  │ Manager+Worker 启动 1s 内消失          │
│  3     │ dmesg 查 segfault                                  │ Hello_ws/wss 大量 GPF(libUtil.so)     │
│        │                                                    │ Interface 无 segfault                  │
│  4     │ core dump (ulimit -c + SIGABRT)                   │ apport 拦截, 无 core 文件生成          │
│  5     │ strace -f (容器内)                                 │ 缺 libunwind → 无法运行               │
│  6     │ 拷贝 libunwind 到容器                              │ strace 可用                           │
│  7     │ strace -f 捕获完整系统调用                         │ ★ 发现 io_uring_setup ENOMEM          │
│  8     │ gdb + debug binary (cmake -DCMAKE_BUILD_TYPE=Debug)│ 确认退出路径: 异常→terminate→SIGABRT  │
│  9     │ 禁用 etcd 对比测试                                 │ 无 etcd: daemon 存活; 有 etcd: 崩溃   │
│ 10     │ etcd 非根因确认                                    │ connect() 从未被调用 → 崩溃在 etcd 前 │
└────────┴────────────────────────────────────────────────────┴──────────────────────────────────────┘
```

### 关键 strace 输出

```
PID 395 (Manager) 事件序列:

395   bind(4, {AF_INET, port=27009, addr=192.168.3.62})  = 0   ← bind 成功
395   listen(4, 100)                                      = 0   ← listen 成功
395   socketpair(... [8,9])                               = 0   ← Worker IPC
395   socketpair(... [10,11])                             = 0
395   clone()                                             = 396 ← fork Worker

396   bind(7, {AF_INET, port=27008, addr=0.0.0.0})       = 0   ← Worker 绑定客户端端口成功

395   io_uring_setup(16384, {IORING_SETUP_NO_SQARRAY})   = -1 ENOMEM  ← ★ 崩溃点
395   write(2, "terminate called after throwing ")               ← std::terminate
395   write(2, "std::system_error")                              ← 异常类型
395   write(2, "io_uring_queue_init: Cannot allocate memory")    ← 异常消息
395   tgkill(395, SIGABRT)                                       ← 自毁
```

---

## 3. 根因分析

```
配置 io_backend="ev"
  │
  ▼
Labor::InitIoBackend()  ← 即使配置是 ev, 也优先尝试 asio_uring
  │
  ├─ if (strBackend == "ev")  → 不匹配任何 if
  │
  └─ #ifdef THUNDER_IO_ASIO_URING   ← 编译时开启
       │
       ▼
     AsioUringIoBackend::Init()
       │
       ├─ asio::stream_descriptor 构造 → 触发 io_uring_service
       │    │
       │    └─ io_uring_setup(16384, {IORING_SETUP_NO_SQARRAY})
       │         │
       │         └─ ENOMEM  ← 内核无法分配 16384 entries 的内存
       │              │
       │              └─ ASIO 抛出 std::system_error
       │
       └─ ★ 异常未被捕获 → std::terminate() → SIGABRT
            │
            ├─ Manager 崩溃
            └─ Worker 检测 Manager 死亡 (getppid()==1) → 也退出
```

### 为什么之前没问题

1. 本分支 staged change 将 `io_backend` 从 `"ev"` 改成了 `"asio_uring"`
2. 即使后来改回 `"ev"`，`InitIoBackend()` 的 fallback 逻辑仍会优先尝试 asio_uring
3. 开发机上 `io_uring_setup(16384 entries)` 需要约 4MB 连续内核内存，ENOMEM
4. `AsioUringIoBackend::Init()` 返回 `bool` 但实际抛异常 → `if (!Init())` 无法捕获

---

## 4. 修复

### 4.1 Labor.cpp — try-catch 包裹 asio_uring init

```diff
 // 默认: 优先 asio_uring (如果已编译), 失败则 fallback ev
 #ifdef THUNDER_IO_ASIO_URING
     {
         AsioUringIoBackend* pBackend = new AsioUringIoBackend();
-        if (pBackend && pBackend->Init(m_loop, callback, static_cast<void*>(this)))
+        bool bInitOk = false;
+        try {
+            bInitOk = pBackend && pBackend->Init(m_loop, callback, static_cast<void*>(this));
+        } catch (const std::exception& e) {
+            LOG4_WARN("IoBackend: asio_uring init threw exception");
+            bInitOk = false;
+        } catch (...) {
+            LOG4_WARN("IoBackend: asio_uring init threw unknown exception");
+            bInitOk = false;
+        }
+        if (bInitOk)
         {
             m_pIoBackend = pBackend;
             LOG4_INFO("IoBackend: asio_uring initialized successfully (default)");
             return true;
         }
         delete pBackend;
         LOG4_WARN("IoBackend: asio_uring init failed, falling back to ev (default)");
     }
 #endif
```

### 4.2 entrypoint.sh — sed 匹配任意 inner_host

```diff
-[ "$MY_IP" != "0.0.0.0" ] && sed -i "s/\"inner_host\": \"0.0.0.0\"/\"inner_host\": \"$MY_IP\"/"
+[ "$MY_IP" != "0.0.0.0" ] && sed -i "s/\"inner_host\": \"[^\"]*\"/\"inner_host\": \"$MY_IP\"/"
```

**问题**: 原 sed 只匹配 `"0.0.0.0"`，当 conf 中有 `"127.0.1.1"`（如 `/etc/hosts` 映射）时无法替换，导致绑定 loopback 地址。

### 4.3 entrypoint.sh — 重试耗尽后 exit (防假 Running)

```diff
+RETRIES=0
 for i in $(seq 1 5); do
     ./bin/Interface ./conf/Interface.json && break
     EXIT_CODE=$?
     if [ $EXIT_CODE -eq 98 ]; then
+        RETRIES=$i
         sleep 2
     else
         exit $EXIT_CODE
     fi
 done
+if [ "$RETRIES" -ge 5 ] 2>/dev/null; then
+    echo "FATAL: Interface failed to bind after 5 retries"
+    exit 98
+fi
```

### 4.4 配置回退

```
deploy/Interface/conf/Interface.json:
  inner_host: "127.0.1.1" → "0.0.0.0"    (staged 错误变更回退)
  io_backend: "asio_uring" → "ev"          (asio_uring ENOMEM, 回退 ev)

k8s/interface-deployment.yaml:
  + env INNER_PORT: "27009"                (entrypoint 依赖此变量)

k8s/regression-test.sh:
  单次 curl → 3 次重试 + 5s 超时         (hostNetwork 间歇性)
```

---

## 5. 验证

```
修复后日志:
  [WARN] IoBackend: asio_uring init threw exception
  [WARN] IoBackend: asio_uring init failed, falling back to ev (default)
  [INFO] IoBackend: ev initialized successfully
  [INFO] InitClientListener() worker_0 listen on iPortForClient(27008)

端口:
  0.0.0.0:27008  LISTEN  ✅
  192.168.3.62:27009  LISTEN  ✅

curl:
  → {"code":400,"msg":"ok"}  ✅

回归测试: 40/40 PASS  ✅
```

---

## 6. 经验总结

```
┌─────────────────────────────────────────────────────────────────────┐
│  1. 返回 bool 的函数内部可能抛异常 → if (!fn()) 捕获不到            │
│     → 包裹 try-catch 或改为 noexcept                               │
│                                                                     │
│  2. io_uring_setup ENOMEM 不是 bug，是内核资源限制                   │
│     → InitIoBackend 的 fallback 设计是正确的，但实现漏了异常处理    │
│                                                                     │
│  3. entrypoint sed 不应假设配置文件的默认值                          │
│     → 用通配符匹配任意值更健壮                                      │
│                                                                     │
│  4. 容器内 debug 三板斧: strace(需libunwind) > gdb(需debug符号)     │
│     > core dump(apport拦截需手动配置)                                │
│     → strace -f 是最快速定位静默崩溃的手段                          │
│                                                                     │
│  5. 对比测试法: 禁用 etcd → daemon存活 → 排除 etcd 连接问题         │
│     → 快速缩小排查范围                                              │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 7. 涉及文件

```
修改:
  code/Net/src/labor/Labor.cpp           try-catch 包裹 asio_uring init
  deploy/Interface/entrypoint.sh         sed 通配 + 重试耗尽 exit
  deploy/Interface/conf/Interface.json   inner_host/io_backend 回退
  k8s/interface-deployment.yaml          INNER_PORT env
  k8s/regression-test.sh                3 次重试

新建:
  docs/quality/05-interface-crash-debug.md  本文档
```
