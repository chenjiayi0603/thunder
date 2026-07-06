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

### K8s (kind)

```bash
# 1. 创建 kind 集群（含目录挂载 + 镜像加速）
kind create cluster --name thunder --config kind_config.yaml

# 2. 创建共享存储 PV + PVC
kubectl apply -f k8s/plugins-pv.yaml

# 3. 构建 admin-web 镜像（含 requests 依赖）
docker build -t thunder-admin-web:latest -f- . << 'EOF'
FROM python:3.12-alpine
RUN pip install --no-cache-dir requests
WORKDIR /app
CMD ["python3", "server.py", "--port", "8090"]
EOF
kind load docker-image thunder-admin-web:latest --name thunder

# 4. 部署所有服务
kubectl apply -f k8s/

# 5. 给 hello Pod 安装运行时依赖（libjemalloc）
kubectl set image deploy/thunder-hello hello=ubuntu:26.04
kubectl patch deploy/thunder-hello --type json -p '[{"op":"replace","path":"/spec/template/spec/containers/0/command","value":["bash","-c","apt-get update -qq && apt-get install -y -qq libjemalloc2 && cp /thunder/code/3party/lib/*.so* /usr/lib/x86_64-linux-gnu/ 2>/dev/null; ./node.sh start && tail -f /dev/null"]}]'
```

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

## 四、验证结果

### 2026-07-06 K8s (kind v1.32.0)

| 步骤 | 结果 | 证据 |
|------|:---:|------|
| PUT 上传 | ✅ ok=true, size=11 | admin-web 返回 200 |
| 文件共享 (admin-web Pod) | ✅ | `K8S-SHARED` |
| 文件共享 (hello Pod) | ✅ | `K8S-SHARED` |
| 文件共享 (kind 节点) | ✅ | `K8S-SHARED` |
| 三处 MD5 一致 | ✅ | 同一文件 |

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
