# Thunder项目长期记忆

## 项目核心信息

### 项目概述
- **项目名称**: Thunder
- **类型**: C++20分布式异步集群服务框架
- **主要功能**: Center注册发现、Worker并发处理、HTTP与内部二进制协议接入、可插拔模块(.so)
- **当前架构**: 多进程Worker架构，单Manager管理多Worker

### 核心组件
1. **Center**: 集群管理中心，支持Raft选主
2. **Interface**: 接入层服务，处理HTTP请求
3. **Logic**: 业务逻辑服务
4. **Hello**: 示例服务
5. **Net**: 网络通信核心框架

### 当前痛点 (2026-03诊断)
- **扩展性**: Manager单点瓶颈，无弹性扩缩容
- **稳定性**: 缺少熔断、限流、降级机制
- **可观测性**: 仅有基础日志，无Metrics和Tracing
- **部署**: 无容器化支持，配置管理分散

## 架构演进方向

### Thunder-NG (Next Generation) 核心设计原则
1. **云原生化**: 容器化、服务网格、声明式部署
2. **弹性伸缩**: 自动扩缩容、多Manager集群
3. **观测驱动**: Metrics + Tracing + Logging三位一体
4. **容错设计**: 熔断、限流、降级、多活
5. **性能优先**: 零拷贝、内存池、连接池

### 关键技术选型
- **容器编排**: Kubernetes + Helm
- **服务网格**: Istio
- **监控**: Prometheus + Grafana
- **链路追踪**: Jaeger
- **配置中心**: etcd
- **日志**: ELK/EFK
- **发布**: Argo Rollouts (蓝绿/金丝雀)

### 性能基准
- **当前**: 54k QPS/Worker
- **目标**: 100k QPS/Worker
- **挑战**: 延迟P99 < 50ms，内存<400MB

## 实施规划

### 六阶段实施法
1. **基础架构升级** (1-2月): Docker + K8s + Istio
2. **可观测性建设** (1月): Prometheus + Jaeger + ELK
3. **弹性与高可用** (1-2月): HPA + 熔断限流降级
4. **配置与路由优化** (1月): etcd + 动态路由
5. **安全加固** (1月): TLS 1.3 + JWT + RBAC
6. **性能优化** (持续): 零拷贝 + 内存池

### 成功标准
- 可用性: 99.95%
- RTO: <30秒
- 扩容时间: <60秒
- 故障自愈: 自动检测与恢复

## 重要决策记录

### 2026-03-28 架构重构启动
- **决策**: 启动Thunder后端架构重构
- **原因**: 现有架构无法满足业务增长需求
- **方案**: 采用云原生架构全面重构
- **负责人**: 后端架构师团队
- **预期收益**: 扩展性提升3倍，运维成本降低50%

## 项目约定

### 开发规范
- C++20标准
- 异步非阻塞编程模型
- 基于Step的状态机设计
- 插件化开发(.so)

### 部署规范
- 容器化部署(Docker)
- Kubernetes编排
- GitOps发布流程
- 蓝绿/金丝雀发布

### 监控规范
- 所有服务暴露/metrics端点
- 关键路径添加Trace埋点
- 结构化日志输出(JSON)
- 统一日志格式和标准

## 联系方式与协作

- **项目仓库**: //wsl.localhost/Ubuntu-24.04/home/administrator/interview-quicker/thunder
- **部署目录**: //wsl.localhost/Ubuntu-24.04/home/administrator/interview-quicker/thunder/deploy
- **配置管理**: 迁移到etcd集中管理
- **文档中心**: 架构设计文档在7第三方库目录

## 参考资料

### 架构文档
- `7第三方库/Thunder架构重构方案.md` - 完整重构方案
- `README.md` - 项目概述
- `INSTALL.md` - 构建安装文档
- `deploy/deploy.md` - 部署说明

### 技术债务
- todo.md中的待办事项需要逐步清理
- Center路由同步机制待优化
- Worker配置热加载需要完善
- 插件热更新机制待实现
