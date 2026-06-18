# Plan: 去掉 Center，接入 etcd（单例 + 集群）

**关联评估**: `docs/architecture/evaluations/etcd_as_center_evaluation.md`
**复杂度**: MEDIUM-HIGH
**状态**: Phase 1-3 已实现，运行中发现若干 keepalive/注册 bug（#102-#105）已修复

---

## Summary

去掉自研 Center(Raft/注册/在线表/Admin),业务节点改接 etcd。通过现有 `CenterConnector` 抽象新增 `EtcdCenterConnector`,业务逻辑零改。`EtcdCenterConnector` 运行在 **Manager 进程**(Manager 负责连注册中心)。客户端**用 Thunder 原生 HTTP(框架已有 `HttpCodec`),不引 libcurl、不引 gRPC 运行时**:在 Manager 的 libev 事件循环上自管一条到 etcd 的连接,经 HTTP gateway 做长连接 watch + 短请求 KV。node_id 用槽位占位 + lease 续期。

---

## 决策(已定)

| 项 | 决定 |
|---|---|
| 客户端协议 | **用 Thunder 原生 HTTP,不用 libcurl、不引 gRPC**:<br>`EtcdCenterConnector`(运行在 **Manager**)在 Manager 的 libev 主循环上自管一条到 etcd 的 TCP 连接,收发用框架 `HttpCodec`(`Encode(HttpMsg,CBuffer)`/`Decode(CBuffer,HttpMsg)`,独立于 Worker)。<br>短请求(注册/txn/keepalive/range): 请求-响应<br>watch 长连接流式: 同一套 `HttpCodec` **增量解 chunked**,事件直接在主循环回调——**无独立线程、无 `ev_async` 跨线程投递**(去掉原 libcurl + curl 线程方案) |
| Admin 替代 | **Python/shell 脚本**查 etcd,替代 Center Admin 页 |
| node_id 策略 | **槽位占位 + 续期**:ip:port 已有 lease → KeepAlive 续期;不存在 → 抢槽位 txn |

---

## etcd 数据模型

```
/thunder/slot/{i}            # i=1..255, node_id 槽位, 绑 lease; 崩溃 lease 过期自动删
/thunder/registry/{ip:port}  # → {node_id, node_type, ...}, 在线表; 绑同一 lease
/thunder/config/{path}       # → 配置内容, mod_revision 即版本号
```

---

## Patterns to Mirror

| 类别 | 源 | 模式 |
|---|---|---|
| 客户端策略 | `code/Net/include/labor/CenterConnector.hpp` | 新增 `EtcdCenterConnector` 实现接口,仿 `TcpCenterConnector` |
| 事件回调 | `CenterEvent`(Registered/RouteUpdated/ConfigUpdated/ConnectionLost) | etcd 操作映射成这些事件 |
| 单线程模型 | Manager 的 libev 主循环 | 注册/watch/KeepAlive 全在 Manager 主循环上跑,**无独立线程、无跨线程投递**(替代原 curl 线程 + `PostToEventLoop`/ev_async 方案) |
| shm 下发 | version++ / 先 blob 后 len / 半包保护 | **完全不动**,只换上游来源 |
| 测试 | `tests/e2e`(pytest) | E2E 加 etcd 容器,单/集群两套 |

---

## Files to Change

| 文件 | 操作 | 原因 |
|---|---|---|
| `code/Net/include/labor/CenterConnector.hpp` | UPDATE | 增加 `etcd` 类型注释/工厂分支 |
| `code/Net/src/labor/EtcdCenterConnector.{hpp,cpp}` | CREATE | etcd 客户端实现(注册/watch/keepalive) |
| `deploy/docker-compose*.yml` | UPDATE | 加 etcd 服务(单节点开发 / 3 节点生产) |
| `deploy/Center/` | DELETE(Phase 6) | 下线自研 Center |
| `code/Center/` | DELETE(Phase 6) | 下线自研 Center 代码 |
| 各业务节点 `*Cmd.json` | UPDATE | 加 `etcd_endpoints` 配置,去掉 `centers` |

---

## Tasks

### Phase 0 — 骨架 + 数据模型(纯隔离,零行为变化)
- 新增 `EtcdCenterConnector` 空骨架(Init/Destroy/Name/ReportNodeStatus/IsConnected)
- `center_backend: tcp|etcd` 配置开关,工厂选实例
- 定义 etcd key schema(`/thunder/slot/*/registry/*/config/`)
- **验证**: 配 `tcp` 时 `ctest 全量 + E2E` 全绿

### Phase 1 — 注册 + node_id 分配
- `Init`:连 etcd endpoints(单/多),`LeaseGrant(TTL=10s)`
- `Register`:
  1. 幂等查 `registry/{ip:port}`
     - 存在(lease 活):直接 `KeepAlive` 续期,取回 node_id → `Registered` 事件
     - 不存在:抢槽位 txn(从 ip:port 哈希位置起扫,防惊群)→ 写 slot+registry → `Registered` 事件
  2. libev 定时器(Manager 主循环)每 ~3s `POST /v3/lease/keepalive`
- **长连接 watch**: `POST /v3/watch` 常驻连接,`HttpCodec` 增量解 chunked;短 KV 操作可复用同一 keep-alive 连接
- **验证**: 并发注册零碰撞;杀进程 → lease 过期 → 槽位释放可复用;重连续期不重新分配

### Phase 2 — 路由发现(watch → RouteUpdated → shm)
- watch `/thunder/registry/` 前缀长连接 → 收事件 → 按 `node_type` 过滤组装 `NodeNotice` → `RouteUpdated` → **现有 shm 写入链路不动**
- 记 `last_revision`,断线按 revision 重连补漏(不丢事件)
- watch 回调就在 Manager 主循环上,直接写 shm(无跨线程投递)
- **验证**: 节点 A 上线,节点 B watch 到,shm 更新;断网重连不丢事件

### Phase 3 — 配置(watch → ConfigUpdated → shm)
- watch `/thunder/config/` 前缀 → `ConfigUpdated` → 现有配置 shm 路径不动
- 配置写入改为 etcdctl / 薄 admin 写 etcd key(替代 `StepSetConfig`)
- **验证**: 写 etcd 配置 key → Worker shm 感知新版本

### Phase 4 — 部署替换(单例 + 集群)
- `docker-compose`:**单节点 etcd**(开发)+ **3 节点 etcd**(生产,替换 3 个 Center 容器)
- 业务节点配置 `etcd_endpoints:["127.0.0.1:2379"]` or 3 个 endpoint;连接失败自动轮换
- **验证**: E2E 单节点 etcd 全通;E2E 3 节点 etcd 全通;杀 1 个 etcd 节点集群仍可用

### Phase 5 — Admin 替代(Python/shell 脚本)
- 写几个脚本替代 Center Admin 页,放 `deploy/scripts/`:
  ```
  admin_nodes.py   # 查在线表: GET /v3/kv/range prefix=/thunder/registry/, 格式化输出
  admin_config.py  # 查/改配置: GET/PUT /v3/kv/range|put prefix=/thunder/config/
  admin_status.sh  # 集群健康: etcdctl endpoint health + endpoint status
  ```
- 调用方式: `python3 admin_nodes.py` 或 `./admin_status.sh`,传 etcd endpoint 参数
- **验证**: 能查集群在线表,能改配置,能看集群健康

### Phase 6 — 下线 Center + 全回归
- 删 `code/Center/`、`deploy/Center/`、`CmdRaft*`、自研 Raft 相关
- `center_backend` 默认 `etcd`,移除 `tcp` 分支(或保留兼容期)
- **验证**: 全功能回归(单元 + E2E 单/集群 + 冒烟 7 项);TSan(watch 回调跨线程)

---

## Validation

```bash
# 每阶段
./deploy.sh build && ./deploy.sh test unit --skip-build

# Phase 2/3/4/6
./deploy.sh test e2e --skip-build

# Phase 1 专项
# 并发注册零碰撞: 多节点同时注册, 验证 node_id 不重复
# 续期验证: 节点重连, 拿到原 node_id, lease 续上
# 崩溃回收: kill 节点, TTL 后槽位释放, 新节点可抢到同号

# Phase 4 专项
# 单节点 etcd: docker-compose up → E2E 全通
# 3 节点 etcd: docker-compose up → E2E 全通 → kill 1 节点 → 集群仍可用

# Phase 6
# 单线程模型: 注册/watch/keepalive 全在 Manager 主循环, 不再有 curl 线程跨线程写 shm
```

---

## 已知 Bug（生产运行发现，2026-06-16）

| # | 问题 | 严重 | 状态 |
|---|---|---|---|
| #103 | `AsyncTryClaimSlot` HTTP 错误被当 slot 已占，255 次遍历后误报"所有槽位已满" | 高 | ✅ 已修复 |
| #104 | `SelfAuditRegistry` 超时时以 nodeId=0 触发 rebind，写无效 `/thunder/slot/0` | 中 | ✅ 已修复 |
| #105 | 注册失败后 keepalive 恢复 → ConnectionRestored，但 nodeId 永远是 0 | 高 | ✅ 已修复 |
| #102 | `EtcdHttpConn` 单连接队列：selfAudit 超时触发 FailAll 把 keepalive 一并取消，keepalive 失败计数虚高 | 中 | 🟡 待根本修复（建议独立 keepalive 连接） |

---

## Risks

| 风险 | 可能性 | 缓解 |
|---|---|---|
| watch 长连接 + revision 续看要自己写 | 高 | 封装在 connector ~50 行;先单测断线续看 |
| KeepAlive 定时器(Manager 主循环),续租失败处理 | 中 | TTL=10s,每 3s 续;失败重试;超时 → ConnectionLost 事件 |
| 快速重连 vs lease 未过期的幂等边界 | 中 | registry 幂等逻辑 Phase 1 专项测 |
| Center 有隐藏职责未迁移 | 低 | Phase 6 前 grep 确认业务侧只依赖 `CenterConnector` |
| 3 节点 etcd 运维新增负担 | 低 | etcd 运维成熟,docker-compose 标准化 |
| watch 收敛延迟 vs 现状推送 | 中 | Phase 2 测收敛延迟,与现状对比 |
| EtcdHttpConn 单连接队列导致 keepalive 和 audit 请求互相影响 | 已确认 | #102 待修复：独立 keepalive 连接或优先队列 |

---

## Acceptance

- [ ] Phase 0: `tcp` 行为零变化,E2E 全绿
- [ ] Phase 1: 并发注册零碰撞;续期不重新分配;崩溃 TTL 后回收
- [ ] Phase 2: watch 路由 shm 更新;断线不丢事件
- [ ] Phase 3: etcd 配置写入 → shm 感知
- [ ] Phase 4: 单/3 节点 E2E 全通;3 节点容 1 故障
- [ ] Phase 5: 能查在线表/配置
- [ ] Phase 6: 全回归通过;Center 代码彻底下线;TSan 干净
