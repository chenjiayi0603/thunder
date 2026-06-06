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
# etcd (单节点, 本地测试)
kubectl apply -f k8s/namespace.yaml
kubectl apply -f k8s/etcd-local.yaml

# 业务节点 (需先构建 Docker 镜像)
docker build -t thunder-logic:latest -f docker/Dockerfile .
kubectl apply -f k8s/logic-deployment.yaml
kubectl apply -f k8s/interface-deployment.yaml
kubectl apply -f k8s/hello-deployment.yaml
kubectl apply -f k8s/hello-ws-deployment.yaml
```

## 待完成

- [ ] 构建 Docker 镜像 (当前使用 hostPath 挂载)
- [ ] etcd 3 节点 StatefulSet 测试 (需 PVC provisioner)
- [ ] 冒烟回归测试

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

## 冒烟测试结果 (2026-06-07)

| 测试 | 结果 | 备注 |
|------|------|------|
| Hello Echo | ✅ code:0 | |
| Hello PoolCpu | ✅ checksum=786432 | |
| HelloWS 握手 | ✅ 101 | |
| etcd health | ✅ | |
| etcd registry | ✅ 3 nodes | |
| Hello Redis | ❌ | K8s Service DNS 未注入 Hello 配置 |
| Hello MySQL | ❌ | 同上 |
| Interface GenKey | ❌ | 进程正常,HTTP 无响应 |

## 冒烟测试结果 (2026-06-06)

| 测试 | 结果 | 说明 |
|------|------|------|
| etcd health | ✅ | |
| etcd registry | ✅ | 3 nodes, 全部正确 Pod IP |
| HelloWS 握手 | ✅ | 101 |
| Hello Echo | ⚠️ | Pod 重启后 codec 编译延迟 |
| Interface GenKey | ⚠️ | code:1, 路由同步中 |
| Redis | ✅ | 传 host 参数 |
| MySQL | ✅ | 传 host 参数 |

## k8s 关键配置

inner_host 占位 + POD_IP 替换:
```bash
# 源配置: inner_host: "0.0.0.0"
# 启动脚本: sed -i "s/0.0.0.0/$POD_IP/" /thunder/deploy/*/conf/*.json
```
