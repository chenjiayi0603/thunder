# Thunder etcd 混沌测试报告

> 日期: 2026-06-05
> 脚本: `tests/chaos_etcd.sh`
> 结果: **13/13 全部通过**

---

## 场景与目标

| 场景 | 验证 | 目标 |
|------|------|------|
| etcd 停止→恢复 | 路由缓存续命 + 异步重注册 | 短期不可达不影响服务 |
| etcd 重启(保留数据) | 数据持久 + 快速恢复 | raft log 完整,节点正常续租 |
| etcd 数据清空(灾难) | 自动重建 lease+slot+registry | 最坏情况自愈 |

---

## 测试过程

### 0. 基线

```
✅ etcd health
✅ GenKey 正常     — Interface→Logic S2S 路由正常
✅ registry 存在   — 3 节点已注册
```

### 1. etcd 停止 → 恢复

1. `docker stop etcd` — 停止 etcd 容器
2. 验证 etcd 不可达, **但 GenKey 仍可路由** (shm 缓存续命)
3. `docker start etcd` — 启动 etcd
4. etcd 恢复后, keepalive 定时器补领 lease, 节点重新注册
5. registry 恢复, GenKey 恢复

```
✅ etcd 不可达
✅ GenKey 仍可路由(缓存)      ← 关键: 短期 etcd 离线不影响已有路由
✅ etcd 恢复
✅ registry 恢复
✅ GenKey 恢复
```

**结论**: 短期 etcd 中断(秒级)不影响业务。路由走 shm 缓存, etcd 恢复后自动重注册。

### 2. etcd 重启(保留数据)

1. `docker restart etcd` — 重启 etcd
2. 验证 raft log 完整, 已有 keys 不丢
3. 节点 lease 续租正常

```
✅ etcd 健康
✅ keys 仍在                  ← 关键: raft log + snapshot 数据持久
```

**结论**: etcd 重启后数据不丢失。raft WAL + snapshot 保证持久性。

### 3. etcd 数据清空(灾难恢复)

1. 停止 etcd, 删除 `/docker/data/etcd/member/` (模拟磁盘故障)
2. 重启 etcd(空库)
3. 节点检测到 lease 失效 → AsyncLeaseGrant 补领
4. 节点检测到 registry 为空 → DoRegister → Claim 路径抢占 slot
5. 30s 内完成全部重建

```
✅ etcd 启动(空)
✅ 节点重新注册               ← 关键: 从零重建 lease+slot+registry
✅ GenKey 恢复                ← 路由恢复
```

**结论**: 最坏情况(etcd 数据全丢)下, 节点能自动重建注册表。租约 TTL=10s + KeepAlive 3s 定时器保证 30s 内自愈。

---

## 依赖机制

| 机制 | 文件 | 行号 |
|------|------|------|
| 异步 Lease 补领 | [EtcdCenterConnector.cpp#L261](code/Net/src/labor/EtcdCenterConnector.cpp#L261) | AsyncLeaseGrant |
| 注册延续链 | [EtcdCenterConnector.cpp#L425](code/Net/src/labor/EtcdCenterConnector.cpp#L425) | DoRegister |
| 槽位抢占 | [EtcdCenterConnector.cpp#L405](code/Net/src/labor/EtcdCenterConnector.cpp#L405) | AsyncTryClaimSlot |
| KeepAlive 定时器 | [EtcdCenterConnector.cpp#L660](code/Net/src/labor/EtcdCenterConnector.cpp#L660) | OnKeepAliveTimer |
| m_regInProgress 超时 | [EtcdCenterConnector.cpp#L427](code/Net/src/labor/EtcdCenterConnector.cpp#L427) | 30s 防死锁 |
| 路由 shm 缓存 | `Manager.cpp:2690` | route mirror update |
