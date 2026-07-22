# Thunder k8s 部署配置

## 文件清单

| 文件 | 内容 | 类型 |
|------|------|------|
| `namespace.yaml` | thunder namespace | 基础设施 |
| `etcd-local.yaml` | etcd Deployment (单节点, hostPath, 测试用) | 基础设施 |
| `etcd-statefulset.yaml` | etcd StatefulSet (3节点, PVC, 生产用) | 基础设施 |
| `redis.yaml` | Redis Deployment + ClusterIP Service | 基础设施 |
| `mysql.yaml` | MariaDB Deployment + ClusterIP Service | 基础设施 |
| `logic-deployment.yaml` | Logic Deployment (1 replica) | 业务 |
| `interface-deployment.yaml` | Interface Deployment + NodePort Service | 业务 |
| `hello-deployment.yaml` | Hello Deployment + NodePort Service | 业务 |
| `hello-ws-deployment.yaml` | HelloWS Deployment + NodePort Service | 业务 |
| `hello-https-deployment.yaml` | HelloHTTPS Deployment + NodePort Service | 业务 |
| `minio.yaml` | MinIO 制品库 (PV+PVC+Secret+Deployment+Service, #159) | 基础设施 |
| `admin-web-deployment.yaml` | Admin-Web 管理后台 Deployment + NodePort Service | 业务 |
| `admin-web-pvc.yaml` | Admin-Web 持久化存储 PV+PVC | 基础设施 |
| `admin-web-rbac.yaml` | Admin-Web ServiceAccount + RBAC | 运维 |
| `logic-hpa.yaml` | Logic Horizontal Pod Autoscaler | 运维 |
| `conf/` | k8s 环境专属配置文件（与 `deploy/` 独立） | 配置 |
| `regression-test.sh` | K8s 回归测试统一入口 (`--quick`/`--full`/`--all`) | 测试 |

---

## 架构设计

### 拓扑结构

```
                          ┌──────────────────────────────────────────────┐
                          │                 k8s Cluster                  │
                          │                                              │
   外部客户端 ──────────►  │  NodePort :30006 ──► Hello Pod (27006)       │
                          │  NodePort :30008 ──► Interface Pod (27008)   │
                          │  NodePort :30010 ──► HelloWS Pod (27010)     │
                          │  NodePort :30043 ──► HelloHTTPS Pod (27443)  │
                          │                                              │
                          │  ┌──────────────── 内部 S2S ──────────────┐ │
                          │  │  Interface ──► Logic (16068)            │ │
                          │  │  Hello ──► Logic (16068)               │ │
                          │  │  HelloWS ──► Logic (16068)             │ │
                          │  └────────────────────────────────────────┘ │
                          │                    │                         │
                          │                    ▼                         │
                          │           ┌──────────────┐                   │
                          │           │     etcd     │ 服务发现 & 路由     │
                          │           │  (2379)      │                   │
                          │           └──────┬───────┘                   │
                          │                  │                           │
                          │     ┌────────────┼────────────┐              │
                          │     ▼            ▼            ▼              │
                          │  ┌──────┐  ┌─────────┐  ┌─────────┐         │
                          │  │Redis │  │  MySQL  │  │  Logic  │         │
                          │  │:6379 │  │  :3306  │  │ (多副本) │         │
                          │  └──────┘  └─────────┘  └─────────┘         │
                          └──────────────────────────────────────────────┘
```

### 节点类型与端口规划

| 节点 | 类型 | access_host | access_port | inner_host | inner_port | 说明 |
|------|------|-------------|-------------|------------|------------|------|
| Hello | HELLO | `$POD_IP` | 27006 | `$POD_IP` | 27007 | HTTP 对外服务 (NodePort 30006) |
| HelloWS | HELLO | `$POD_IP` | 27010 | `$POD_IP` | 27011 | WebSocket 对外服务 (NodePort 30010) |
| HelloHTTPS | HELLO | `$POD_IP` | 27443 | `$POD_IP` | 27444 | HTTPS 对外服务 (NodePort 30043) |
| Interface | INTERFACE | `$POD_IP` | 27008 | `$POD_IP` | 27009 | API 网关 (NodePort 30008) |
| Logic | LOGIC | — | — | `$POD_IP` | 16068 | 纯后端，仅内网 S2S 通信 |

- **access_port**: 对外服务端口，Worker 进程绑定，暴露 NodePort
- **inner_port**: S2S 内部通信端口，Manager 进程绑定（Logic 只有一个 inner_port）

---

## 服务发现与路由

### etcd 注册流程

```
   Pod 启动
      │
      ▼
   node.sh → sed 替换 0.0.0.0 → $POD_IP
      │
      ▼
   Manager 进程启动
      │
      ▼
   EtcdCenterConnector::Init()
      │  连接 etcd (thunder-etcd.thunder:2379)
      │  申请 Lease (TTL=10s, 3s keepalive)
      ▼
   DoRegister()
      │  PUT /thunder/registry/{TYPE}/{POD_IP}:{inner_port}
      │  value: {node_id, node_type, node_ip, node_port, worker_num}
      ▼
   Watch 启动
      │  prefix watch /thunder/registry/
      │  全量 snapshot → 增量 watch
      ▼
   路由表更新 → 共享内存 → Worker 读取
```

### etcd Key 设计

```
/thunder/registry/
  ├── LOGIC/
  │     ├── 10.42.0.100:16068  →  {"node_id":1, "node_type":"LOGIC", ...}
  │     └── 10.42.0.101:16068  →  {"node_id":2, "node_type":"LOGIC", ...}
  ├── INTERFACE/
  │     └── 10.42.0.102:27009  →  {"node_id":3, "node_type":"INTERFACE", ...}
  └── HELLO/
        ├── 10.42.0.103:27007  →  {"node_id":4, "node_type":"HELLO", ...}
        └── 10.42.0.104:27011  →  {"node_id":5, "node_type":"HELLO", ...}
```

- Key 格式: `/thunder/registry/{TYPE}/{IP}:{PORT}`
- 类型前缀使路由按需下发成为可能（#38 upstream_types 过滤）

### etcd 地址发现 (业务节点如何知道 etcd 在哪)

业务节点通过配置文件中的 `center.etcd_endpoints` 获知 etcd 地址，不同环境使用不同来源：

```json
// 每个节点的 JSON 配置中
{
  "center": {
    "connector": "etcd",
    "etcd_endpoints": "http://thunder-etcd.thunder:2379"
  }
}
```

**k8s 环境 — 依赖 k8s Service DNS**：

```
  kubectl apply -f etcd-local.yaml
       │
       ▼
  k8s 创建 Service "thunder-etcd" (ClusterIP: 10.43.76.197)
       │
       ▼
  CoreDNS 自动注册:
    thunder-etcd.thunder.svc.cluster.local → 10.43.76.197
       │
       ▼
  业务 Pod DNS 解析:
    thunder-etcd.thunder → 10.43.76.197:2379 → etcd Pod
```

- 无需额外服务发现组件，k8s Service 创建后 DNS 自动生效
- 配置中写死域名 `thunder-etcd.thunder`，Pod 内 DNS 自动解析到 ClusterIP
- 格式: `<service-name>.<namespace>` (短域名, k8s 自动补全 `svc.cluster.local`)

**docker-compose 环境 — 直连宿主机端口**：

```
  network_mode: host
  etcd 监听 127.0.0.1:2379
  业务容器直接连 127.0.0.1:2379
```

| 环境 | etcd_endpoints 值 | 解析方式 |
|------|------------------|---------|
| k8s | `thunder-etcd.thunder:2379` | k8s CoreDNS → ClusterIP |
| docker-compose | `127.0.0.1:2379` | 本地回环 (host 网络) |
| 裸机多 etcd | `host1:2379,host2:2379` | DNS / hosts 文件 + 自动故障转移 |

### 跨节点 S2S 路由

```
  Interface Worker 收到 GenKey 请求
      │
      ▼
  StepCo20Func: co_await SendToInternalByNodeTypeAsync("LOGIC")
      │
      ▼
  NodesMgr::GetNodeByType("LOGIC")  ← 从共享内存路由表查找
      │
      ▼
  TCP 连接 → Logic Pod IP:16068
      │
      ▼
  Logic Worker 处理 → 返回 token+key
```

- Interface 配置 `upstream_types: ["LOGIC"]` 仅订阅 LOGIC 类型路由
- 路由表通过共享内存 (ShmRingQueue) 从 Manager 同步到 Worker

---

## 配置注入机制

### 设计原则

配置文件中的 IP 地址使用 `0.0.0.0` 占位符，运行时由启动脚本替换为实际 IP。

```
  源文件 (k8s/conf/*.json)           运行时 (/tmp/conf/*.json)
  ┌─────────────────────────┐       ┌──────────────────────────────┐
  │ "access_host": "0.0.0.0" │  sed  │ "access_host": "10.42.0.103" │
  │ "inner_host": "0.0.0.0"  │ ────► │ "inner_host": "10.42.0.103"  │
  │ "etcd_endpoints":        │       │ "etcd_endpoints":            │
  │   "http://thunder-etcd   │       │   "http://thunder-etcd       │
  │    .thunder:2379"        │       │    .thunder:2379"            │
  └─────────────────────────┘       └──────────────────────────────┘
```

### 启动脚本逻辑

```bash
# 1. 复制配置到临时目录（避免修改 hostPath 共享文件）
mkdir -p /tmp/conf
cp /thunder/k8s/conf/Hello.json /tmp/conf/Hello.json

# 2. 替换占位符为 Pod IP
sed -i "s|0.0.0.0|$POD_IP|g" /tmp/conf/Hello.json

# 3. 通过环境变量注入配置目录
export THUNDER_CONF_DIR=/tmp/conf
./node.sh start
```

### 为什么用 /tmp/conf 而不是直接修改 deploy/？

| 方式 | 问题 |
|------|------|
| 直接 sed deploy/ | hostPath 多 Pod 共享 → sed 互相覆盖，IP 错乱 |
| cp + sed /tmp/conf | 每个 Pod 独立副本，容器重启自动消失，安全隔离 ✅ |

---

## 网络设计

### 为什么 NodePort？

| Service 类型 | 适用场景 | Thunder 选择 |
|-------------|---------|-------------|
| ClusterIP | 集群内部通信 | etcd, Redis, MySQL, Logic |
| NodePort | 外部测试/调试 | Hello, Interface, HelloWS, HelloHTTPS |
| LoadBalancer | 生产对外 | 生产环境建议 |

NodePort 映射:
```
  宿主机 IP:30006  ──►  Hello Pod:27006
  宿主机 IP:30008  ──►  Interface Pod:27008
  宿主机 IP:30010  ──►  HelloWS Pod:27010
  宿主机 IP:30043  ──►  HelloHTTPS Pod:27443
```

### k8s vs docker-compose 网络对比

| 项 | docker-compose | k8s |
|----|---------------|-----|
| 网络模式 | `network_mode: host` | Pod 独立 IP (CNI/flannel) |
| 目标 IP | `127.0.0.1` / `192.168.x.x` | Pod IP (`10.42.0.x`) |
| 外部访问 | 直接 localhost:port | NodePort (`{nodeIP}:300xx`) |
| 服务发现 | 本地 etcd (`127.0.0.1:2379`) | k8s DNS (`thunder-etcd.thunder:2379`) |
| 配置注入 | `hostname -I` + sed → `/tmp/conf` | `$POD_IP` env + sed → `/tmp/conf` |
| 端口暴露 | 直接监听宿主机 | NodePort Service |
| 冒烟测试 | `pytest --mode=external` (127.0.0.1) | NodePort 或 port-forward |

---

## 管理控制台

| 服务 | 地址 | 用途 |
|------|------|------|
| **admin-web** | `http://192.168.3.61:30090` | 节点拓扑、插件管理、配置下发、Lua 脚本、灰度路由、etcd 浏览 |
| **MinIO Console** | `http://192.168.3.61:30091` | 制品库文件浏览（bucket: `artifacts`） |
| **etcd 浏览** | admin-web → `📂 etcd` tab | 按前缀浏览 etcd key/value |

### MinIO Console 登录

```
用户名: minioadmin
密码:   minioadmin
```

> 凭据定义在 `k8s/minio.yaml` 的 `minio-credentials` Secret 中。

### SO 制品下发（#159 Pull 模式）

```
admin-web PUT SO  ──→  MinIO bucket (thunder-minio.thunder:9000)
                ──→  etcd so_url + version bump

Manager Poll (5s) ──→  检测 etcd version 变化
                ──→  HTTP GET MinIO → 下载 /app/plugins/
                ──→  GracefulRestart Worker

Pod 重启        ──→  Manager 启动 → Poll 重拉 SO（不丢文件）
```

---

## SO 模块部署 (NFS 共享存储) — 已废弃

> ⚠️ 自 #159 起，SO 下发改用 Pull 模式（MinIO 存储 + Manager HTTP GET），
> NFS 共享存储方案已废弃。保留此节仅作历史参考。

所有 Pod 共用同一份 SO 文件，通过 NFS PersistentVolume 挂载。

**部署步骤**:

```bash
# 1. 先确认 NFS 服务器就绪, 修改 plugins-pv.yaml 中的 server IP
# 2. 创建 PV/PVC
kubectl apply -f k8s/plugins-pv.yaml

# 3. 在 deployment.yaml 的 containers 下加:
#    volumeMounts:
#    - name: plugins
#      mountPath: /data/thunder/plugins
#    volumes:
#    - name: plugins
#      persistentVolumeClaim:
#        claimName: thunder-plugins

# 4. 在 Pod 启动脚本中加 symlink:
#    ln -sf /data/thunder/plugins/HelloHttp deploy/HelloHttp/plugins
```

**文件**: `k8s/plugins-pv.yaml`

| 资源 | 说明 |
|------|------|
| PV | NFS ReadOnlyMany, 10Gi, server 改实际 IP |
| PVC | thunder namespace, 绑定 PV |

详见 `docs/architecture/08-so-module-hot-reload-via-etcd.md`

---

## 部署顺序

### 启动依赖链

```
  etcd ────► Logic ────► Interface
    │           │
    ├──► Redis ─┤
    │           │
    └──► MySQL ─┘
                  │
                  └──► Hello / HelloWS / HelloHTTPS
```

1. **基础设施**: namespace → etcd → redis → mysql
2. **核心服务**: Logic (最先，S2S 路由依赖)
3. **业务服务**: Interface → Hello / HelloWS / HelloHTTPS

Logic 必须先于 Interface 启动，否则 Interface 路由表为空，GenKey 失败。

### 冒烟测试

```bash
# k8s 环境
./tests/test_smoke.sh --k8s

# 或直接 pytest (需先 port-forward 或 NodePort 可达)
cd tests/e2e
python3 -m pytest . -m smoke -v --mode=external
```

---

## 已知限制

| 限制 | 影响 | 解决方案 |
|------|------|---------|
| etcd 单节点 (local) | 无高可用 | 生产用 `etcd-statefulset.yaml` (3副本) |
| NodePort 端口固定 | 多实例冲突 | 生产用 LoadBalancer / Ingress |
| hostPath 持久化配置 | 多 Pod 共享冲突 | 已解决：cp → /tmp/conf + THUNDER_CONF_DIR |
| certs 需预生成 | HTTPS 首次需手动 | 已内置 gen_self_signed_https_cert.sh |
| etcd 多端点仅取首 | 无故障转移 | #40 — 生产用 k8s Service LB

---

## Lua 热更新

### 方式一：K8s 直写 etcd（适合脚本/CI，跳过 admin-web）

```bash
ETCD_POD=$(kubectl get pods -n thunder -l app=thunder-etcd -o jsonpath='{.items[0].metadata.name}')

# 1. 写新 Lua 脚本到 etcd
kubectl exec -n thunder "$ETCD_POD" -- etcdctl \
  put /thunder/config/module/HELLO_HTTP \
  '{"module":[{"url_path":"/hello/lua_echo","so_path":"plugins/HelloHttp_ModuleLua.so","entrance_symbol":"create","load":true,"version":99,"script_content":"function handle_request(msg)\n  SendToClientFast('"'"'{\"code\":0,\"msg\":\"HOTRELOAD\"}'"'"')\n  return true\nend"}]}'

# 2. 验证
curl -s -X POST http://192.168.3.61:27006/hello/lua_echo -d 'test'
# → {"code":0,"msg":"HOTRELOAD"}
```

### 方式二：admin-web 页面

`http://192.168.3.61:30090` → `📜 Lua` tab：在线编辑 → 保存 → 自动热加载

## SO 热更新（#159 Pull 模式）

```bash
# 1. 上传 .so 到 admin-web 制品库（→ MinIO + 本地 PVC）
curl -X PUT --data-binary @ModuleHello.so \
  http://192.168.3.61:30090/api/plugins/HelloHttp/ModuleHello.so

# 2. 下发 → etcd 写 version + so_url → Manager 自动 Pull 下载
curl -X POST -H "Content-Type: application/json" \
  -d '{"filename":"ModuleHello.so"}' \
  http://192.168.3.61:30090/api/plugins/HelloHttp/deploy
```

**全链路**: admin-web PUT → MinIO bucket + etcd `so_url` → Manager Poll (5s) 检测 version 变化 → HTTP GET MinIO 下载到 `/app/plugins/` → GracefulRestart Worker。Push 模式 (exec+tar) 已废弃保留回滚。
