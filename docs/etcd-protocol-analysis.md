# etcd 注册协议分析

> 2026-07-28 | 分析结论：当前阶段不需要 SDK 模块化

## 协议现状

Thunder 节点注册到 etcd 使用两层键：

```
/thunder/slot/{1..255}        → "ip:port"                  (带 lease，CAS 抢占)
/thunder/registry/{node_type}/{ip:port}  → JSON:
  {"node_id":3,"node_type":"HELLO_HTTP","node_ip":"192.168.3.61","node_port":27006,
   "worker_num":1,"node_version":"v1","registered_at":1753700000}
```

## 兼容性评估

| 要求 | 状态 | 说明 |
|------|:--:|------|
| slot/registry 两层键格式 | ✅ | `02-etcd-designed.md` 已文档化 |
| JSON 字段名规范 | ✅ | 固定字段，无变更计划 |
| Lease TTL = 60s, KeepAlive = 10s | ✅ | 常量在 `EtcdGrpcConnector.cpp` |
| CAS 槽位抢占 (1~255) | ✅ | 标准 etcd txn，无外部依赖 |
| Watch 实时推送 | ✅ | gRPC server-side streaming |

## 结论

当前阶段纯 Thunder 集群（全 C++），无外部服务接入需求。协议已文档化，不需要 SDK 模块化。如未来有 Go/Python 服务接入需求，只需提供协议文档 + 参考实现即可，无需独立 SDK。

## 参考

- [02-etcd-designed.md](architecture/02-etcd-designed.md) — 完整 etcd 设计
- `code/Net/src/register/EtcdGrpcConnector.cpp` — 参考实现
