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
