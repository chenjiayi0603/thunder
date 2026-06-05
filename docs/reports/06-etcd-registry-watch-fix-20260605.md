# etcd 注册中心 / Watch / 健康检查 修复报告

- 日期：2026-06-05
- 分支：`fix/etcd-registry-watch-health`
- 关联 Issue：#19（注册键丢失）、#20（watch 重连风暴）、#21（healthcheck）
- 衍生：#22（真·长连接 watch follow-up）；④无租约孤儿键作为 #19 robustness 一并修复

---

## 一、问题与根因（带证据）

### ① 注册中心实际为空 —— 注册键随旧租约过期被删，节点永不重注册（#19，严重）

**现象**：etcd 全 keyspace `count=0`，但节点日志 `KeepAlive 成功 / node_id 247 / 心跳成功`，自我感觉良好；路由靠共享内存旧缓存续命，**新节点无法通过 etcd 发现现有节点**——注册中心需求未达到。

**根因**：`EtcdCenterConnector::DoRegister` 幂等分支只判断"注册键是否存在"，不判断它绑在哪个租约：

```
重启换新租约 → QueryRegistry 查到上一进程残留的注册键(绑【旧租约】)
            → 走"已注册(幂等)"直接 return，不重绑
旧租约过期 → etcd 删除注册键 → m_registered=true 永不重注册 → 注册表永久为空
```

证据：用 `LeaseGrant` 日志逐条还原（旧进程注册成功 → 新进程走幂等分支 → 旧租约过期键被删）；`/v3/lease/timetolive?keys=true` 返回无 keys 数组；`/v3/kv/range` 全表 count=0。

**附带根因**：`KeepAlive()` 只判断响应含 `result` 即算成功，但 etcd 对【已过期/不存在】的 lease 也回 HTTP 200、`TTL=0`，导致死租约被误判为续租成功，掩盖注册丢失。

### ② watch 每秒重连风暴（#20，中）

**现象**：Logic/Interface/HelloWs 各约 2.2 万行 `WARN Watch — 连接断开 code=0 msg=No error`，约 1 次/秒，日志刷屏（单 W0 100MB+ 频繁 rotate）。

**根因（两层）**：
1. 代码**完全没有处理** etcd 的取消响应 `{"canceled":true,"compact_revision":N}`（仅注释提到）。空 keyspace + 周期 compaction 后 `compact_revision == revision`，watch 起点 ≤ compact_revision 会被 etcd 立即取消。
2. 取消后原样 `sleep(1)` 重连同一坏起点 → 无限 1s 风暴；且每轮打 WARN → 日志刷屏。

**深层环境约束（关键发现）**：即便修正起点，**bundled libcurl 8.21-DEV 对 etcd HTTP gateway 的 chunked watch 流，收到首个 `created` 响应后即返回 `CURLE_OK`（实测 `perform` 耗时 0ms，不挂住）**；而 system curl 8.18 对同一请求可挂住数秒。已排除：请求内容（etcd 对 system curl 同请求挂住）、HTTP/2（强制 1.1 无效）、`Expect:100-continue`（关闭无效）。因此长连接 watch 在当前 bundled curl 下无法成立，退化为快照轮询。

### ③ etcd 容器 unhealthy（#21，假警报）

**现象**：`docker inspect` → `unhealthy`，`FailingStreak 8376`，报错 `exec "/bin/sh": no such file or directory`；但 etcd 服务本体正常。

**根因**：compose healthcheck `["CMD-SHELL","curl -sf .../health"]` 需要 `/bin/sh`，而 etcd v3.5.x 是 distroless 镜像，无 `/bin/sh` 也无 `curl` → healthcheck 永远失败。

### ④ 无租约时写出孤儿键（测试中暴露，robustness）

**现象**：在 etcd 与业务节点**同时重启**的竞态下，`Init` 的 `LeaseGrant` 失败 → `m_leaseId=0` → 注册流程仍写键，得到 `lease=0` 的孤儿键（永不过期）；跨重启累积占满槽位 → `所有槽位已满` → 节点改用新 slot（247→248）→ 路由错乱。`OnKeepAliveTimer` 在 `m_leaseId==0` 时直接 return，永不补领。

---

## 二、修复方案

### 代码改动

| 文件 | 改动 |
|------|------|
| `EtcdParse.hpp`（新增） | 纯解析/决策逻辑：`ParseRangeRevision` / `ParseRegistryKv` / `ParseWatchControl` / `DecideRegAction` / `GetGatewayInt64`（gateway int64 编码为 JSON 字符串，须按字符串取） |
| `EtcdCenterConnector.{hpp,cpp}` | ①`QueryRegistry` 带出 lease；`DoRegister` 用 `DecideRegAction`，残留旧租约时 `RebindRegistration` 重绑到当前租约；`SelfAuditRegistry` 周期自检+自愈；`KeepAlive` 校验 `TTL>0`。②watch 用 `ParseRangeRevision` 取起点、处理 `compact_revision` 取消、WARN 降级、退避；明确为 resync 轮询（间隔 `kWatchResyncIntervalSec=2s`）。④`DoRegister`/`OnKeepAliveTimer` 无 lease 时补领、否则不写键。监控指标 + 周期 `EtcdMetrics` 日志。 |
| `docker-compose.yml` | ③healthcheck 改 `["CMD","etcdctl","--endpoints=...","endpoint","health"]`（不经 shell） |
| `code/test/...` | 新增 `thunder_test_etcd_parse`（13 用例） |

### ② 的处置说明（重要，不夸大）

由于 bundled curl 无法挂住 chunked watch 流，**未实现真·事件推送长连接**。当前行为：每 2s 做一次全量快照 resync（路由表保持新鲜，等价于此前"靠 snapshot 维持"，但注册表已真实可用、且无日志刷屏）。`compact_revision` 处理、起点修正已就位，一旦将来换用可挂住的 HTTP 客户端（curl multi 流式 / 直连 gRPC）即可升级为真 watch。**建议另开 follow-up issue 跟踪真·长连接 watch。**

---

## 三、验证证据

### 单元测试（TDD，先红后绿）
- `thunder_test_etcd_parse`：**13/13 通过**（含真实抓取的空 range 响应取 revision、watch 取消解析 compact_revision、旧租约→Rebind 决策）。
- 全量 `ctest`：**301/301 通过**（0 失败；跳过项为 Docker/DB 环境门控）。
- Python 单元：**60/60 通过**。

### 端到端（真实 Docker 集群，干净 hermetic 起停）
- `./deploy.sh test e2e --skip-build`：**E2E 全部通过**。
- 手动冒烟（干净 bringup 后）：

| 项 | 结果 |
|----|------|
| ① 注册表 | `count=6`（3 registry + 3 slot），**全部绑非 0 租约，0 孤儿键** |
| ① 路由 GenKey | `{"code":0,...,"msg":"success"}`（Interface→Logic S2S 通） |
| ③ etcd 健康 | `healthy`，`FailingStreak=0` |
| ② watch | 新运行 `watchReconnect≈0.5/s`（2s resync），`连接异常(WARN)=0`，无日志刷屏；`EtcdMetrics` 周期输出 |
| ④ 孤儿键 | 干净起停后 `lease=0` 键数=0；节点 nodeId 稳定不漂移 |

### 监控（#19/#20 防复发）
- 周期 `EtcdMetrics` 日志：keepalive ok/fail、watchReconnect、watchCompactCancel、registryRebind、selfAuditFail、lease、nodeId。
- `SelfAuditRegistry`：每 ~30s 对账自身注册键存在性 + 租约归属，缺失/不符则 ERROR 告警并自动重绑（直接防 #19 复发）。

---

## 四、遗留与后续

1. **真·长连接 watch**（②深层约束）：bundled curl 不挂住 chunked 流，当前为 2s resync 轮询。建议 follow-up：换 curl multi 流式读 / 直连 gRPC。
2. etcd 与节点**同时重启**的竞态已被 ④ 的补领逻辑覆盖，但生产应保证 etcd 先于业务节点就绪（healthcheck 已修，可用作 depends_on 条件）。
