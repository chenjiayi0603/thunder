# SO 模块分发优化 — 设计文档

> 日期: 2026-06-15 | 状态: 📝 提案中
> 关联文档: `15-so-module-hot-reload-via-etcd.md`

---

## 1. 现状

当前 SO 分发链路：

```
编译机: cmake --build → xxx.so（500KB）
     → docker build（包成镜像，含 alpine rootfs，~4MB）
     → docker push → registry

admin: docker pull registry/so-xxx:v3
     → docker create
     → get_archive（Docker daemon 打包 tar 流）
     → 读流 + untar（解出 .so）
     → docker rm
     → 写 NFS → 通知 etcd → 生产节点 dlopen
```

## 2. 问题

| 问题 | 说明 |
|---|---|
| **docker.sock 特权** | admin 挂 `/var/run/docker.sock`，K8s 反模式，有 root 逃逸风险 |
| **存储浪费** | 每版本 ~4MB（alpine 占 3.5MB），实际 .so 只 500KB，浪费 87% |
| **步骤冗余** | admin 取 .so 要 6 步：pull → create → get_archive → 读流 → untar → rm |
| **版本管理弱** | Docker tag（v1/v2）可覆盖，无原生回滚 |
| **调试不便** | "当前运行哪个版本？" → 追查 tag 对应 digest，麻烦 |

## 3. 方案：MinIO 对象存储

### MinIO 是什么

MinIO 是一个开源对象存储，跟 AWS S3 接口完全兼容。简单说 = **自己服务器上跑一个 S3**。

```
特点:
- 单个二进制，docker run 就起来
- 上传下载走 HTTP（PUT/GET），不挂载任何东西
- S3 API 兼容，mc、boto3、requests 都能用
- 自带版本控制
```

### 替换关系

| 组件 | 现在用 | 改成 |
|---|---|---|
| **版本存储**（构建机 → admin） | Docker registry | **MinIO** |
| **运行时共享**（生产节点读 .so） | NFS | **不动** |

**MinIO 替换 Docker registry，NFS 保留不动。** 两者不互斥。

### MinIO vs NFS

两个角色的定位不同，不需要二选一：

| | MinIO | NFS |
|---|---|---|
| **角色** | 版本存储（构建机 → admin） | 运行时共享（生产节点读 .so） |
| **存什么** | 所有历史版本 .so | 当前要加载的 .so |
| **谁读写** | 构建机写，admin 读 | admin 写，生产节点读 |
| **版本管理** | 有（文件名时间戳） | 无 |
| **替换谁** | **Docker registry** | **不动，保持现状** |

### Docker registry vs MinIO

| 维度 | Docker registry（现状） | MinIO（优化后） |
|---|---|---|
| 提取 .so 步骤 | 6 步（pull/create/get_archive/读流/untar/remove） | **1 步（HTTP GET）** |
| K8s 兼容性 | 反模式（挂 docker.sock） | **标准 Service，走 ClusterIP** |
| 安全 | docker.sock = root 特权 | **HMAC 密钥，最小权限** |
| 存储效率 | ~4MB/镜像（87% 是 alpine rootfs） | **~500KB/文件（纯 .so）** |
| 版本管理 | Docker tag（可覆盖，无回滚） | **文件名时间戳（不会丢）** |
| 构建上传 | `docker build` + `docker push` | **`mc cp` 一句话** |
| 运维依赖 | Docker daemon 必须运行 | **纯 HTTP，无守护进程依赖** |
| 改动量 | — | ~30 行 yaml + ~20 行 py + ~10 行 sh |

**结论：选 MinIO。** 所有维度 MinIO 都优于 Docker registry，没有 trade-off。改动量不大（~60 行），收益明确：省步骤、省空间、去 docker.sock、版本不丢。

### MinIO vs AWS S3

两者接口一样，部署位置不同。今天用 MinIO，将来想迁到 AWS S3 只需改 URL 和认证方式。

| | MinIO | AWS S3 |
|---|---|---|
| 谁管服务器 | 你自己 | AWS |
| 放哪里 | 你的 K8s 集群 / 物理机 | 亚马逊机房 |
| 费用 | 免费（开源） | 按量付费 |
| 适用场景 | 自建集群，私有部署 | 已有 AWS 账号，规模大 |

### 架构对比

**现状：**

```
构建机 → docker build + push → Docker registry
                                 ↓
           admin（挂 docker.sock）→ docker create → get_archive → untar → .so
                                 ↓
                              NFS（PV/PVC ReadOnlyMany）
                                 ↓
                          生产节点 → dlopen
```

**优化后：**

```
构建机 → mc cp → MinIO（ClusterIP Service）
                    ↓
            ┌───────┴───────┐
            ↓                ↓
    admin（HTTP GET）    生产节点（可选 InitContainer curl）
            ↓                ↓
         NFS（不动）      emptyDir（共享给主容器）
            ↓
     生产节点 → dlopen（热更新流程不变）
```

## 4. 版本管理

方案：**路径后缀**（推荐，零额外依赖，不改 MinIO 配置）

```
MinIO bucket 结构（thunder/plugins/）:

HelloHttp_ModuleHello/
├── HelloHttp_ModuleHello.so.20260615143001    ← v1（保留历史）
├── HelloHttp_ModuleHello.so.20260615153002    ← v2
└── HelloHttp_ModuleHello.so.latest            ← 最新版（覆盖写入，前端默认选这个）

Logic_CmdGetToken/
├── Logic_CmdGetToken.so.20260615120000
└── Logic_CmdGetToken.so.latest
```

每次构建上传两个文件：带时间戳的 + `latest`。admin 列版本 = `ls` 目录按时间戳排序。回滚 = 选旧版本 URL 写 etcd。

## 5. 具体改动

### 5.1 Docker Compose（本地开发）

`docker/docker-compose.yml` 加 MinIO service：

```yaml
services:
  minio:
    image: minio/minio:latest
    command: server /data --console-address ":9001"
    network_mode: host
    volumes:
      - ./data/minio:/data
    environment:
      MINIO_ROOT_USER: thunder
      MINIO_ROOT_PASSWORD: thunder123
    healthcheck:
      test: ["CMD", "mc", "ready", "local"]
      interval: 5s
      timeout: 3s
      retries: 10
```

本地 admin 通过 `http://127.0.0.1:9000` 访问。

### 5.2 K8s 部署（生产）

**StatefulSet（k8s/minio-statefulset.yaml）：**

```yaml
apiVersion: apps/v1
kind: StatefulSet
metadata:
  name: thunder-minio
  namespace: thunder
spec:
  serviceName: thunder-minio
  replicas: 1
  selector:
    matchLabels:
      app: thunder-minio
  template:
    metadata:
      labels:
        app: thunder-minio
    spec:
      containers:
      - name: minio
        image: minio/minio:latest
        args: ["server", "/data", "--console-address", ":9001"]
        env:
        - name: MINIO_ROOT_USER
          value: "thunder"
        - name: MINIO_ROOT_PASSWORD
          value: "thunder123"
        ports:
        - containerPort: 9000   # S3 API
        - containerPort: 9001   # Console UI
        volumeMounts:
        - name: data
          mountPath: /data
  volumeClaimTemplates:
  - metadata:
      name: data
    spec:
      accessModes: ["ReadWriteOnce"]
      resources:
        requests:
          storage: 20Gi
```

**Service（k8s/minio-service.yaml）：**

```yaml
apiVersion: v1
kind: Service
metadata:
  name: thunder-minio
  namespace: thunder
spec:
  selector:
    app: thunder-minio
  ports:
  - name: s3
    port: 9000
    targetPort: 9000
  - name: console
    port: 9001
    targetPort: 9001
```

集群内通过 `http://thunder-minio:9000` 访问。

### 5.3 Admin 部署改造

`k8s/admin-web-deployment.yaml`：去掉 docker.sock 挂载，去掉 `pip install docker`，加 MinIO 环境变量。

```yaml
# 改动要点:
# 1. 删掉 volumeMounts.docker-sock 和 volumes.docker-sock
# 2. pip install docker → pip install requests
# 3. 加三个环境变量
env:
- name: MINIO_URL
  value: "http://thunder-minio:9000"
- name: MINIO_ACCESS_KEY
  value: "thunder"
- name: MINIO_SECRET_KEY
  value: "thunder123"
```

### 5.4 构建脚本改造

`deploy.sh build-so`：从 `docker build + push` 改成 `mc cp` 上传到 MinIO。

```bash
build_one_so_module() {
    local mod="$1"
    local dir="${SO_IMAGE_DIR}/${mod}"
    local version=$(date +%Y%m%d%H%M%S)

    MC_HOST_minio="http://${MINIO_ACCESS_KEY}:${MINIO_SECRET_KEY}@${MINIO_HOST}:${MINIO_PORT}"

    for sofile in "$dir"/*.so; do
        local name=$(basename "$sofile")
        mc cp "$sofile" "minio/thunder/plugins/${mod}/${name}.v${version}"
        mc cp "$sofile" "minio/thunder/plugins/${mod}/${name}.latest"
    done

    echo "$version" > "${dir}/.version"
    ok "${mod} → minio:9000/thunder/plugins/${mod}/（v${version}）"
}
```

构建机访问方式：

| 场景 | 访问方式 |
|---|---|
| CI 在集群内（同 Namespace） | `http://thunder-minio:9000` |
| CI 在集群外 | `kubectl port-forward svc/thunder-minio 9000:9000` |
| 开发机 | `kubectl port-forward ...` 或 NodePort/LB |

### 5.5 Admin API 改造

`deploy/admin-web/server.py`：`_handle_so_extract` 从 6 步 Docker 操作改成 1 步 HTTP GET。

```python
MINIO_URL = os.environ.get("MINIO_URL", "http://thunder-minio:9000")
MINIO_ACCESS_KEY = os.environ.get("MINIO_ACCESS_KEY", "thunder")
MINIO_SECRET_KEY = os.environ.get("MINIO_SECRET_KEY", "thunder123")

def _handle_so_extract(self):
    body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
    image = body["image"]       # e.g. "HelloHttp_ModuleHello:v20260615143001"
    so_file = body["file"]
    node_type = body["type"]

    version = image.split(":")[-1]
    mod = image.split(":")[0]

    # 直接 HTTP GET，不走 Docker
    url = f"{MINIO_URL}/thunder/plugins/{mod}/{so_file}.{version}"
    resp = requests.get(url, auth=(MINIO_ACCESS_KEY, MINIO_SECRET_KEY))
    data = resp.content

    self._save_so(data, f"plugins/{TYPE_DIR[node_type]}/{so_file}")
```

### 5.6 生产节点：InitContainer（可选，仅 K8s）

如果想去掉 NFS 依赖，生产节点启动时从 MinIO 直拉 .so：

```yaml
initContainers:
- name: download-plugin
  image: curlimages/curl:latest
  command:
  - sh
  - -c
  - |
    curl -o /plugins/HelloHttp/Hello.so \
      "http://thunder-minio:9000/thunder/plugins/HelloHttp_ModuleHello/HelloHttp_ModuleHello.so.latest"
  volumeMounts:
  - name: plugins
    mountPath: /plugins

containers:
- name: app
  volumeMounts:
  - name: plugins
    mountPath: /data/thunder/plugins
```

当前架构用 NFS，这个改动可选，不做强制。

## 6. 迁移步骤

```
Phase 1 - 部署 MinIO + 双写（Docker + MinIO 并存）
  ├── docker-compose.yml 加 MinIO service（本地）
  ├── kubectl apply -f k8s/minio-statefulset.yaml（生产）
  ├── kubectl apply -f k8s/minio-service.yaml（生产）
  ├── deploy.sh 加 upload-so 命令（上传到 MinIO，与 docker build 并存）
  └── admin 页面可切换数据源验证

Phase 2 - Admin 去掉 docker.sock
  ├── 更新 admin-web-deployment.yaml，去掉 docker.sock 挂载
  ├── admin server 去掉 import docker 相关代码
  ├── deploy.sh build-so 默认上传 MinIO
  └── 验证全链路无误

Phase 3 - 清理
  ├── deploy.sh build-so 去掉 docker build/push
  ├── 删除 so-images/*/Dockerfile
  └── 可选：加 InitContainer，去掉 NFS
```

## 7. 改动清单

| 文件 | 改什么 |
|---|---|
| `docker/docker-compose.yml` | + MinIO service（本地开发） |
| `k8s/minio-statefulset.yaml` | **新增**（生产部署） |
| `k8s/minio-service.yaml` | **新增**（生产 Service） |
| `k8s/admin-web-deployment.yaml` | 去掉 docker.sock，加 MinIO 环境变量 |
| `k8s/hello-deployment.yaml` 等 | 可选加 InitContainer |
| `k8s/plugins-pv.yaml` | 不动 |
| `deploy.sh` | `build-so` 从 docker 改成 `mc cp` |
| `deploy/admin-web/server.py` | 从 6 步 Docker 改成 `requests.get` |
| `so-images/*/Dockerfile` | 删除 |
| `docs/architecture/15-so-module-hot-reload-via-etcd.md` | 更新构建/分发章节 |

## 8. 不动的部分

- etcd 通知机制
- NFS 共享存储
- GracefulRestartWorker 热更新
- admin 前端页面操作逻辑

