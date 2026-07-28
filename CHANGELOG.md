# Changelog

## v0.1.0 (2026-07-28)

### Added
- MQTT 3.1.1 Broker + K8s 部署 (#150, #165)
- etcd 混沌测试 K8s 版本 + Docker Compose 参数化 (#161)
- GitHub Actions CI 自动构建+测试 (#175)
- io_backend 配置修复: 尊重配置值, 不再强制 asio_uring (#164)
- ModuleLua ABI 版本校验修复
- Prometheus /metrics 端点方案设计 (#180)
- 架构文档重构 22→19 篇 (-73% 行) (#174)

### Fixed
- Interface→Logic GenKey 超时: CodecMqtt 未编入 libNet.so
- etcd lease TTL 文档修正 30s→60s
- MQTT deployment 配置覆盖 bug
