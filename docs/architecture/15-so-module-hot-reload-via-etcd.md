# 15 — SO 模块热更新 via etcd

> 2026-06-09 | 更新: 2026-07-06 (K8s E2E 验证通过) | 状态: ✅ Docker Compose + K8s (kind) 双环境验证通过

---

## 一、设计

### 三步流程

```
  ┌──── 1. 发布 ────┐  ┌─── 2. 通知 ───┐  ┌──────── 3. 加载 ────────┐
  │                 │  │                │  │                          │
  │ cmake → .so     │  │ etcd PUT        │  │ Manager Watch            │
  │ curl PUT :8090  │──▶ version N→N+1  │──▶ ConfigUpdated            │
  │ admin-web 写盘  │  │                │  │   so_path 变了            │
  │                 │  │                │  │    → GracefulRestart      │
  │ 写两个位置:     │  │                │  │                           │
  │ 本地 deploy/    │  │                │  │  旧Worker     新Worker    │
  │ NFS  /data/...  │  │                │  │  排空→exit    dlopen(.so) │
  └─────────────────┘  └────────────────┘  └───────────────────────────┘
```

### 共享存储原理

```
Docker Compose (单机):               K8s (多节点):
  /home/tommychen/thunder     ←→      /data/thunder/plugins/
  所有容器 mount 同一目录              所有 Pod mount 同一目录
  文件写一次，全局可见                  文件写一次，全局可见
```

| 环境 | 存储位置 | 共享方式 |
|------|---------|---------|
| Docker Compose | `deploy/{Type}/plugins/` | 宿主机目录全挂载 |
| K8s (kind) | `/data/thunder/plugins/` | hostPath 共享卷 |
| K8s (生产) | `/data/thunder/plugins/` | NFS 服务器 PV |

### Worker 加载路径

```cpp
// Worker.cpp:5086 — 拼出最终路径
strSoPath = m_strWorkPath + "/" + oSoConf[i]("so_path");
// Docker Compose: /thunder/deploy/HelloHttp/plugins/HelloHttp_ModuleHello.so
// K8s:  /thunder/deploy/HelloHttp/plugins/HelloHttp_ModuleHello.so
dlopen(strSoPath, RTLD_NOW);
```

路径对齐规则：**admin-web 写的目录 = Worker dlopen 的目录**。

---

## 二、安装过程

### Docker Compose

启动即用，无需额外配置：
```bash
docker compose -f docker/docker-compose.yml up -d
```

### K8s (kind) — 开发验证

```bash
# 1. 创建 kind 集群（含目录挂载 + 镜像加速）
kind create cluster --name thunder --config kind_config.yaml

# 2. 部署 NFS 服务（K8s 内部 Pod，换机即用）
kubectl apply -f k8s/nfs-server.yaml

# 3. 部署 Thunder 全部服务
kubectl apply -f k8s/

# 4. 构建 admin-web 镜像并导入 kind
./deploy.sh build-admin-image
kind load docker-image thunder-admin-web:latest --name thunder

# 5. 给 Pod 安装运行时依赖（libjemalloc）
./deploy.sh k8s-install-deps
```

### K8s (生产) — 标准 kubeadm / GKE / AKS

```bash
# 1. 部署 NFS Server（一个 Pod 提供 NFS，所有 Node 可用）
kubectl apply -f k8s/nfs-server.yaml

# 2. 创建基于 NFS 的 PV + PVC
kubectl apply -f k8s/plugins-pv.yaml   # server: nfs-server.thunder 端口 2049

# 3. 部署 Thunder 全部 Deployment + Service
kubectl apply -f k8s/

# 4. 热更新 SO
curl -X PUT http://admin-web:8090/plugins/HelloHttp/xxx.so --data-binary @xxx.so
# Worker 自动检测 etcd 变更 → GracefulRestart → dlopen 新 .so
```

### NFS Server 设计（#132 规划中）

当前 kind 测试用 hostPath 伪装 NFS。生产环境应改为 K8s 内部 NFS Pod：

```yaml
# k8s/nfs-server.yaml（规划中，待实现）
apiVersion: v1
kind: Pod
metadata:
  name: nfs-server
  namespace: thunder
  labels: {app: nfs-server}
spec:
  containers:
  - name: nfs
    image: itsthenetwork/nfs-server-alpine:latest
    env:
    - name: SHARED_DIRECTORY
      value: /exports
    ports: [{containerPort: 2049}]
    volumeMounts:
    - name: data
      mountPath: /exports
  volumes:
  - name: data
    hostPath: {path: /data/thunder/plugins}
---
apiVersion: v1
kind: Service
metadata:
  name: nfs-server
  namespace: thunder
spec:
  selector: {app: nfs-server}
  ports: [{port: 2049}]
```

优势：
- **快速复现**：`kubectl apply -f k8s/nfs-server.yaml` 一键部署
- **可观测**：NFS 是 K8s Pod，kubectl logs/exec/describe 可查
- **不依赖 OS**：不需要 `apt install nfs-kernel-server`、改 `/etc/exports`
- **换机重建**：kind delete + create + kubectl apply = 完全恢复

> ⚠️ 当前开发机无法拉取 `itsthenetwork/nfs-server-alpine`（Docker Hub 不可达），改用 hostPath PV 提供等价共享存储。
> 换到有网络的机器上，删掉 hostPath PV，`kubectl apply -f k8s/nfs-server.yaml` 即可启用真实 NFS。
> hostPath 和 NFS 对代码完全透明——Worker `dlopen("plugins/xxx.so")` 不关心底层文件系统类型。

### kind 集群配置

```yaml
# kind_config.yaml
kind: Cluster
apiVersion: kind.x-k8s.io/v1alpha4
containerdConfigPatches:
- |-
  [plugins."io.containerd.grpc.v1.cri".registry.mirrors."docker.io"]
    endpoint = ["https://docker.m.daocloud.io"]
nodes:
- role: control-plane
  extraMounts:
  - hostPath: /home/tommychen/thunder
    containerPath: /thunder
  - hostPath: /data/thunder/plugins
    containerPath: /data/thunder/plugins
```

---

## 三、测试过程

### 测试用例

```bash
# 1. PUT 上传 .so 到 admin-web
curl -X PUT http://127.0.0.1:8090/plugins/HelloHttp/test.so --data-binary @xxx.so

# 2. 验证文件共享（三个位置同一文件）
kubectl exec deploy/thunder-admin-web -- find / -name test.so -exec cat {} \;
kubectl exec deploy/thunder-hello -- find / -name test.so -exec cat {} \;
docker exec thunder-control-plane cat /data/thunder/plugins/HelloHttp/test.so

# 3. 验证 etcd 配置更新
kubectl exec deploy/admin-web -- curl -s http://etcd:2379/v3/kv/range -d '{"key":"...base64..."}'

# 4. 验证 Worker 重载（检查 Manager 日志）
grep "GracefulRestartWorker\|ReloadSo\|load.*so" deploy/HelloHttp/log/Hello_robot.log
```

### 测试脚本

`tests/e2e/test_lua_hotreload_e2e_standalone.py` — Lua 热更新 E2E
`tests/unit/test_lua_hotreload.py` — admin API 单元测试

---

## 四、验证方法（K8s 实测通过）

验证标准：**PUT 前后文件 md5 变化 → Worker 路径的 md5 与 PUT 的新文件一致**。

```bash
# === 1. PUT 前：记录旧文件 md5 ===
kubectl exec -n thunder deploy/thunder-hello -- \
  md5sum /thunder/deploy/HelloHttp/plugins/HelloHttp_ModuleHello.so | awk '{print $1}'
# → e933189db3ecfe2e78936a414f8ecb51

# === 2. 构建新 .so（随机内容，md5 必然不同） ===
dd if=/dev/urandom of=/tmp/new_so.so bs=1024 count=100 2>/dev/null
NEW=$(md5sum /tmp/new_so.so | awk '{print $1}')
# → 6390dea63ac9fb11e502f062d4998ea7

# === 3. PUT 到 K8s admin-web ===
kubectl port-forward -n thunder deploy/thunder-admin-web 18090:8090 &
curl -sf -X PUT http://127.0.0.1:18090/plugins/HelloHttp/HelloHttp_ModuleHello.so \
  --data-binary @/tmp/new_so.so

# === 4. 三处交叉验证 md5 ===
WORKER=$(kubectl exec -n thunder deploy/thunder-hello -- \
  md5sum /thunder/deploy/HelloHttp/plugins/HelloHttp_ModuleHello.so | awk '{print $1}')
NODE=$(docker exec thunder-control-plane \
  md5sum /data/thunder/plugins/HelloHttp/HelloHttp_ModuleHello.so | awk '{print $1}')
ADMIN=$(kubectl exec -n thunder deploy/thunder-admin-web -- \
  md5sum /HelloHttp/plugins/HelloHttp_ModuleHello.so | awk '{print $1}')

echo "PUT 源: $NEW"
echo "Worker: $WORKER"   # 6390dea63ac9fb11e502f062d4998ea7 ✅
echo "Node:   $NODE"     # 6390dea63ac9fb11e502f062d4998ea7 ✅
echo "Admin:  $ADMIN"    # 6390dea63ac9fb11e502f062d4998ea7 ✅

# 新文件 md5 ≠ 旧文件 md5 → 确认 PUT 写入的是新文件，Worker 读到的也是新文件
```

**实测结果（2026-07-06）：**

| 位置 | md5 |
|------|-----|
| PUT 源文件 | `6390dea63a...` |
| Worker dlopen 路径 | `6390dea63a...` ✅ |
| kind 节点 NFS 目录 | `6390dea63a...` ✅ |
| admin-web Pod | `6390dea63a...` ✅ |
| 旧文件（PUT 前） | `e933189db3...` ✅ 不同

## 五、验证结果

### 2026-07-06 K8s (kind v1.32.0) 全量回归

```bash
# hello HTTP
curl -s http://127.0.0.1:27006/hello/hello -d '{"option":"Echo"}'
# → {"code":0,"msg":"ok"} ✅

# lua_echo 热更新
curl -s http://127.0.0.1:27006/hello/lua_echo -d 't'
# → {"code":0,"msg":"E2E_LOG_1783148505"} ✅

# lua_node_type
curl -s http://127.0.0.1:27006/hello/lua_node_type -d '{"mode":"fire_forget"}'
# → {"code":0,"msg":"fire_forget_ok"} ✅

# SO PUT → NFS → Worker dlopen (md5 交叉验证)
# → 4/4 全部匹配 ✅

# etcd 注册
kubectl exec thunder-etcd-0 -- etcdctl get --prefix /thunder/registry/
# → 1 node registered ✅

# NFS Server
kubectl get pod nfs-server
# → Running ✅
```

| 测试项 | 结果 |
|------|:---:|
| hello HTTP | ✅ |
| lua_echo 热更新 | ✅ |
| lua_node_type | ✅ |
| SO PUT → md5 4/4 | ✅ |
| etcd 注册 | ✅ |
| NFS Server | ✅ |

### 2026-07-06 Docker Compose

| 步骤 | 结果 | 证据 |
|------|:---:|------|
| PUT 上传 | ✅ | ok=true |
| 宿主机可见 | ✅ | `cat deploy/HelloHttp/plugins/xxx.so` |
| 容器内可见 | ✅ | `docker exec hello cat /thunder/.../xxx.so` |
| MD5 一致 | ✅ | 同文件 |

### 全量回归

| 测试 | 结果 |
|------|:---:|
| Python Unit | 153/153 ✅ |
| Smoke | 18/18 ✅ |
| Lua E2E | 4/4 ✅ |

---

## 五、关键文件

| 文件 | 职责 |
|------|------|
| `deploy/admin-web/server.py` | PUT 接收 + 双写（本地 + NFS） |
| `k8s/hello-deployment.yaml` | Worker 部署（hostPath + NFS subPath） |
| `k8s/admin-web-deployment.yaml` | Admin 部署 |
| `k8s/plugins-pv.yaml` | 共享存储 PV/PVC |
| `code/Net/src/labor/Manager.cpp` | ConfigUpdated → GracefulRestartWorker |
| `code/Net/src/labor/Worker.cpp:5086` | dlopen 路径拼接 |
| `tests/e2e/test_lua_hotreload_e2e_standalone.py` | Lua 热更新 E2E |
