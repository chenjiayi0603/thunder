# Thunder k8s 部署配置

## 文件

| 文件 | 内容 |
|------|------|
| `namespace.yaml` | thunder namespace |
| `etcd-local.yaml` | etcd Deployment (单节点,hostPath,测试用) |
| `etcd-statefulset.yaml` | etcd StatefulSet (3节点,PVC,生产用) |
| `logic-deployment.yaml` | Logic Deployment (1 replica) |
| `interface-deployment.yaml` | Interface Deployment + NodePort Service |
| `hello-deployment.yaml` | Hello Deployment + NodePort Service |
| `hello-ws-deployment.yaml` | HelloWS Deployment + NodePort Service |

## 部署

```bash
# 1. 创建 namespace
kubectl apply -f k8s/namespace.yaml

# 2. 部署基础设施 (etcd/redis/mysql)
kubectl apply -f k8s/etcd-local.yaml
kubectl apply -f k8s/redis.yaml
kubectl apply -f k8s/mysql.yaml

# 3. 等待基础设施就绪
kubectl wait --for=condition=ready pod -l app=thunder-etcd -n thunder --timeout=60s
kubectl wait --for=condition=ready pod -l app=thunder-redis -n thunder --timeout=60s
kubectl wait --for=condition=ready pod -l app=thunder-mysql -n thunder --timeout=120s

# 4. 部署业务服务 (Logic 先, 再 Interface/Hello/HelloWS)
kubectl apply -f k8s/logic-deployment.yaml
kubectl wait --for=condition=ready pod -l app=thunder-logic -n thunder --timeout=180s
kubectl apply -f k8s/interface-deployment.yaml
kubectl apply -f k8s/hello-deployment.yaml
kubectl apply -f k8s/hello-ws-deployment.yaml
kubectl wait --for=condition=ready pod --all -n thunder --timeout=300s
```

## 关键修复 (2026-06-07)

1. **镜像问题**: `thunder-node:latest` 无法导入 K3s containerd(需 sudo),改用 `ubuntu:26.04` + apt-get 安装依赖包
2. **YAML 修复**: interface-deployment.yaml 和 hello-ws-deployment.yaml 的 ports 字段损坏(多余行),已修复
3. **NodePort Service**: 为 Hello/HelloWS/Interface 添加 NodePort Service(30006/30010/30008)
4. **workingDir**: 添加 workingDir 到各 Deployment,确保 node.sh 正确调用
5. **k8s/conf 覆盖**: 启动时 cp k8s/conf/*.json 覆盖 deploy 配置,实现 k8s/docker-compose 配置独立
6. **Hello.json center**: 从旧格式改为 etcd connector,修复 Hello 节点无法注册的问题
7. **sed 正则**: inner_host/access_host 替换正则改为 `[^\"]*` 以支持空字符串

## 测试结果 (2026-06-06)

| 测试 | 结果 | 说明 |
|------|------|------|
| etcd | ✅ | healthy, 3 节点注册 |
| Hello Echo | ✅ | code:0 |
| HelloWS 握手 | ✅ | 101 Switching Protocols |
| Redis | ⚠️ | 需配置 k8s Service DNS |
| MySQL | ⚠️ | 需配置 k8s Service DNS |
| Interface | ⚠️ | 进程正常, Worker codec 初始化需排查 |

## 部署方式

```bash
kubectl apply -f k8s/namespace.yaml
kubectl apply -f k8s/etcd-local.yaml
kubectl apply -f k8s/redis.yaml
kubectl apply -f k8s/mysql.yaml
kubectl apply -f k8s/logic-deployment.yaml
kubectl apply -f k8s/interface-deployment.yaml
kubectl apply -f k8s/hello-deployment.yaml
kubectl apply -f k8s/hello-ws-deployment.yaml
```

## 冒烟测试结果 (2026-06-07 final)

| 测试 | 结果 | 备注 |
|------|------|------|
| etcd health | ✅ | healthy |
| etcd registry | ✅ | 4 nodes: Logic/Interface/Hello/HelloWS |
| Hello Echo | ✅ | code:0, msg:ok |
| Hello PoolCpu | ✅ | code:0, msg:ok |
| Hello HTTPS Echo | ✅ | code:0 (NodePort 30043) |
| Hello Redis | ✅ | set_ok:1, get_ok:1 (via k8s Service DNS) |
| Hello MySQL | ✅ | create/insert/select all ok (via k8s Service DNS) |
| HelloWS 握手 | ✅ | 101 Switching Protocols |
| Interface GenKey | ✅ | code:0, token+key returned |
| Interface via NodePort | ✅ | code:0 |
| Hello via NodePort | ✅ | code:0 |
| HelloWS via NodePort | ✅ | 101 |

**全部冒烟测试通过 (12/12)!** 🎉
验证: `./tests/test_smoke.sh --k8s`

## k8s 关键配置

inner_host 占位 + POD_IP 替换:
```bash
# 源配置: inner_host: "0.0.0.0"
# 启动脚本: sed -i "s/0.0.0.0/$POD_IP/" /thunder/deploy/*/conf/*.json
```

## k8s vs docker-compose 差异

| 项 | docker-compose | k8s |
|----|---------------|-----|
| 网络模式 | `network_mode: host` | Pod 独立 IP (CNI/flannel) |
| 目标地址 | `127.0.0.1:27006` | Pod IP (`10.42.0.143`) 或 NodePort (`192.168.3.61:30006`) |
| 集群检查 | `ss -tln \| grep ':27006'` | `kubectl get pods -n thunder` |
| etcd 地址 | `127.0.0.1:2379` | etcd Pod IP 或 Service DNS (`thunder-etcd.thunder:2379`) |
| Redis 地址 | `127.0.0.1:6379` (默认) | `thunder-redis.thunder:6379` (需显式传参) |
| MySQL 地址 | `127.0.0.1:3306` (默认) | `thunder-mysql.thunder:3306` (需显式传参) |
| 端口暴露 | 直接监听宿主机端口 | NodePort Service (30006/30008/30010) |
| 镜像 | Docker 构建 → `docker run` | 需导入 K3s containerd 或从 registry 拉取 |
| 配置位置 | `deploy/*/conf/*.json` | `k8s/conf/*.json` → 启动时 cp 覆盖 |
| 冒烟测试 | `./tests/test_smoke.sh` | `./tests/test_smoke.sh --k8s` |
