# Thunder K8s 部署指南

> 版本: v0.9.7 | 最后更新: 2026-07-27

---

## 目录

- [1. 架构设计](#1-架构设计)
- [2. 部署流程图](#2-部署流程图)
- [3. etcd Key 设计](#3-etcd-key-设计)
- [4. 运维指令](#4-运维指令)
- [5. 文件清单](#5-文件清单)

---

## 1. 架构设计

### 1.1 拓扑结构

```
                         ┌──────────────────────────────────────────────────┐
                         │                  K8s Cluster (ns: thunder)        │
                         │                                                  │
  外部客户端 ──────────►  │  NodePort :30006 ──► HelloHttp Pod      :27006   │
                         │  NodePort :30008 ──► Interface Pod       :27008   │
                         │  NodePort :30010 ──► HelloWS Pod         :27010   │
                         │  NodePort :30043 ──► HelloHTTPS Pod      :27443   │
                         │  NodePort :30090 ──► Admin-Web Pod       :8080    │
                         │                                                  │
                         │  ┌──────────── 内部 S2S ──────────────┐           │
                         │  │  Interface ──► Logic (16068)        │           │
                         │  │  Hello    ──► Logic (16068)        │           │
                         │  │  HelloWS  ──► Logic (16068)        │           │
                         │  │  HelloHTTPS ─► Logic (16068)       │           │
                         │  └────────────────────────────────────┘           │
                         │                    │                              │
                         │        ┌───────────┼───────────┐                  │
                         │        ▼           ▼           ▼                  │
                         │  ┌──────────┐ ┌──────────┐ ┌──────────┐          │
                         │  │   etcd   │ │  Redis   │ │  MySQL   │          │
                         │  │  (2379)  │ │  (6379)  │ │  (3306)  │          │
                         │  └──────────┘ └──────────┘ └────┬─────┘          │
                         │                                │                 │
                         │                      ┌─────────▼──────────┐      │
                         │                      │  MinIO 制品库       │      │
                         │                      │  (9000/9001)       │      │
                         │                      └────────────────────┘      │
                         └──────────────────────────────────────────────────┘
```

### 1.2 节点类型与端口规划

| 节点 | 类型 | access_host | access_port | inner_host | inner_port | 说明 |
|------|------|-------------|-------------|------------|------------|------|
| HelloHttp | HELLO | `$POD_IP` | 27006 | `$POD_IP` | 27007 | HTTP 对外 (NodePort 30006) |
| HelloWS | HELLO | `$POD_IP` | 27010 | `$POD_IP` | 27011 | WebSocket 对外 (NodePort 30010) |
| HelloHTTPS | HELLO | `$POD_IP` | 27443 | `$POD_IP` | 27444 | HTTPS 对外 (NodePort 30043) |
| Interface | INTERFACE | `$POD_IP` | 27008 | `$POD_IP` | 27009 | API 网关 (NodePort 30008) |
| Logic | LOGIC | — | — | `$POD_IP` | 16068 | 纯后端 S2S (无对外端口) |

- **access_port**: Worker 绑定，对外服务
- **inner_port**: Manager 绑定，S2S 内部通信

### 1.3 Manager-Worker 进程模型

```
┌────────────── Pod ──────────────────────────────┐
│                                                  │
│  ┌──────────┐   ShmRingQueue   ┌──────────────┐ │
│  │  Manager  │ ◄──────────────► │   Worker     │ │
│  │          │                   │              │ │
│  │  · etcd  │                   │  · accept    │ │
│  │  · watch │                   │  · I/O       │ │
│  │  · 路由表│                   │  · 业务逻辑   │ │
│  │  · SO热更│                  │              │ │
│  │  · 注册  │                   │              │ │
│  │  inner   │                   │  access/     │ │
│  │  _port   │                   │  inner_port  │ │
│  └──────────┘                   └──────────────┘ │
│                                                  │
└──────────────────────────────────────────────────┘
```

Manager 负责控制面（etcd 注册/监听、路由表维护、SO/Lua 热更新），Worker 负责数据面（网络 I/O、业务处理），两者通过共享内存（ShmRingQueue）同步路由表。

---

## 2. 部署流程图

### 2.1 启动与注册

```
  kubectl apply -f k8s/
      │
      ▼
  Pod 启动 → entrypoint.sh:
    cp k8s/conf/{Type}.json → /tmp/conf/
    sed 's/0.0.0.0/'$POD_IP'/g' /tmp/conf/*.json
    export THUNDER_CONF_DIR=/tmp/conf
      │
      ▼
  ./node.sh → Hello 二进制启动
      │
      ▼
  Manager::Init()
      │
      ├─► ① LeaseGrant(TTL=30s) → m_leaseId
      │
      ├─► ② DoRegister()
      │     PUT /thunder/registry/{TYPE}/{IP}:{PORT}
      │     Value: {node_id, node_type, node_ip, node_port,
      │             worker_num, access_ip, access_port, resume}
      │
      ├─► ③ StartKeepAlive (每 10s leasekeepalive 续约)
      │
      ├─► ④ Watch /thunder/registry/ (prefix → 路由表)
      │
      ├─► ⑤ Watch /thunder/canary/   (prefix → 灰度权重)
      │
      └─► ⑥ Watch /thunder/config/  (prefix → SO/Lua 热更新)
      │
      ▼
  Manager ── ShmRingQueue(路由表) ──► Worker → accept() 开始服务
```

### 2.2 跨节点 S2S 路由

```
  Interface Worker 收到客户端请求
      │
      ▼
  SendToInternalByNodeTypeAsync("LOGIC")
      │
      ▼
  NodesMgr::GetNodesByType("LOGIC")   ← 共享内存路由表
      │
      ▼
  一致性哈希选取目标 Logic 节点
      │
      ▼
  TCP → Logic Pod IP:16068 (inner_port)
      │   Manager 接收 → 转发 Worker
      ▼
  Logic Worker 处理 → 返回响应
```

### 2.3 SO 热更新 (Pull 模式)

```
  ┌──────────┐     ① PUT .so        ┌──────────┐
  │ admin-web │ ───────────────────► │  MinIO   │
  │ CLI / CI  │     (制品存储)       │ (制品库)  │
  └────┬─────┘                      └──────────┘
       │  ② PUT /thunder/config/module/{TYPE}
       │     {so_url, size, md5, version++, load: true}
       ▼
  ┌──────────┐
  │   etcd   │ ──── Watch 事件 ────► 所有 Manager (3~5s 感知)
  └──────────┘
                                        │
                                        ▼
                                 ③ HTTP GET MinIO
                                   下载 .so → /app/plugins/
                                        │
                                        ▼
                                 ④ GracefulRestart Worker
                                    (Drain 旧 Worker → fork 新 Worker)
                                        │
                                        ▼
                                  Worker 加载新 .so → 热更新完成
```

### 2.4 Lua 热更新

```
  ┌──────────┐  ① PUT /thunder/config/module/{TYPE}
  │ admin-web │ ──── {script_content, load: true, version++}
  │ CLI / CI  │
  └──────────┘
       │
       ▼
  ┌──────────┐ ──── Watch 事件 ────► 所有 Manager
  └──────────┘
                                        │
                                        ▼
                                  解析 script_content
                                        │
                                        ▼
                                  ShmRingQueue → Worker
                                        │
                                        ▼
                                  Worker 热加载 Lua 脚本
                                  (无需重启进程)
```

### 2.5 etcd Watch 健康自愈

```
  Manager Watch Loop:
      │
      ▼
  Watch /thunder/registry/ (prefix)
      │
      ├─► 收到事件 → 更新路由表 → 重置健康计时器
      │
      └─► 45s 无事件?
              │
              ▼
           重建 Watcher: 全量 Get → snapshot → 增量 Watch
              │
              ▼
           路由表完整刷新
```

### 2.6 Lease KeepAlive 与故障检测

```
  时间线 ──────────────────────────────────────────►

  0s    LeaseGrant(TTL=30s) → m_leaseId
  3s    首次 leasekeepalive
  10s   leasekeepalive (每 10s 续约)
  ...

  若 Pod 宕机:
  最后一次 keepalive + 30s → Lease 过期
      → etcd 自动删除该节点所有 Key
      → 其他节点 Watch 到 DELETE
      → 路由表中移除该节点

  若 lease 意外丢失 (keepalive 失败):
    Manager 检测 → 自动 DoRegister() 重新注册
```

---

## 3. etcd Key 设计

### 3.1 Key 空间

```
/thunder/
├── registry/                    # 服务注册 (Lease 绑定)
│   ├── LOGIC/
│   │   ├── 10.42.0.10:16068    →  {node_id, node_type, node_ip, node_port, ...}
│   │   └── 10.42.0.11:16068
│   ├── INTERFACE/
│   │   └── 10.42.0.20:27009
│   └── HELLO/
│       ├── 10.42.0.30:27007
│       └── 10.42.0.31:27011
│
├── canary/                      # 灰度路由权重
│   └── {SERVICE}/weights        →  {v1: 80, v2: 20}
│
├── config/                      # 配置下发 (SO/Lua 热更新)
│   └── module/
│       ├── HELLO_HTTP           →  [{so_path, version, so_url, load, ...}]
│       ├── HELLO_HTTPS
│       ├── INTERFACE
│       └── LOGIC
│
└── slot/                        # 节点 ID 分配 (全局自增)
    ├── 1                        →  {node_type: "LOGIC", ...}
    └── 2                        →  {node_type: "INTERFACE", ...}
```

### 3.2 注册 Key: `/thunder/registry/{TYPE}/{IP}:{PORT}`

```json
{
  "node_id":     1,
  "node_type":   "LOGIC",
  "node_ip":     "10.42.0.10",
  "node_port":   16068,
  "worker_num":  4,
  "access_ip":   "",
  "access_port": 0,
  "resume":      true
}
```

| 字段 | 说明 |
|------|------|
| node_id | 全局唯一节点 ID (/thunder/slot/ 分配) |
| node_type | LOGIC / INTERFACE / HELLO |
| node_ip | Pod IP (inner_host) |
| node_port | inner_port, S2S 通信 |
| worker_num | Worker 进程数 |
| access_ip | access_host (对外节点) |
| access_port | access_port (对外节点) |
| resume | true=正常, false=Drain 中 |

- **生命周期**: 绑定 Lease (TTL=30s), Manager 每 10s keepalive
- **过期**: Pod 宕机 30s 后 Key 自动删除, Watch 通知其他节点剔除路由

### 3.3 配置 Key: `/thunder/config/module/{NODE_TYPE}`

```json
[
  {
    "so_path":         "plugins/HelloHttp_ModuleHello.so",
    "entrance_symbol": "create",
    "load":            true,
    "version":         42,
    "so_url":          "http://thunder-minio.thunder:9000/artifacts/HelloHttp/ModuleHello.so",
    "size":            204800,
    "md5":             "d41d8cd98f00b204e9800998ecf8427e",
    "script_content":  ""
  }
]
```

| 字段 | 说明 |
|------|------|
| so_path | 插件在 /app/plugins/ 下的路径 |
| entrance_symbol | SO 入口函数 (create) |
| load | true=加载, false=卸载 |
| version | 单调递增, Manager 对比此字段触发 ReloadSo |
| so_url | MinIO 下载地址 (SO 模块) |
| size | 文件大小 (字节) |
| md5 | 文件校验 |
| script_content | Lua 脚本内容 (Lua 模块, 内联) |

### 3.4 灰度 Key: `/thunder/canary/{SERVICE}/weights`

```json
{"v1": 80, "v2": 20}
```

Interface Manager Watch 此 Key, 变更后下发 Worker, 按权重分流到不同版本的 Logic 节点。

---

## 4. 运维指令

### 4.1 构建与测试

```bash
# === 构建 ===
./deploy.sh build                          # cmake configure + build

# === 测试 (从快到慢) ===
./deploy.sh test unit                      # C++ gtest + Python unit (~45s)
./deploy.sh test                           # unit + Docker E2E (~3min)
./deploy.sh test e2e                       # Docker 集成测试
./deploy.sh test smoke                     # 冒烟: 核心链路 + etcd
./deploy.sh test regression                # 全量回归 (提交前必跑)
./deploy.sh test k8s                       # K8s 全量回归
./deploy.sh test perf                      # wrk 压测

# === Docker Compose 本地 ===
./deploy.sh up / down / restart / status

# === K8s 回归 ===
bash k8s/regression-test.sh --quick        # 52项核心 (~30s)
bash k8s/regression-test.sh --full         # + admin API + canary (~5min)
bash k8s/regression-test.sh --all          # + 扩缩容/稳定性 (~10min)
```

### 4.2 K8s 部署

```bash
# === 部署 ===
kubectl apply -f k8s/namespace.yaml
kubectl apply -f k8s/etcd-statefulset.yaml
kubectl apply -f k8s/redis.yaml mysql.yaml minio.yaml
kubectl apply -f k8s/logic-deployment.yaml          # Logic 必须最先进
kubectl apply -f k8s/interface-deployment.yaml
kubectl apply -f k8s/hello-deployment.yaml
kubectl apply -f k8s/hello-https-deployment.yaml
kubectl apply -f k8s/hello-ws-deployment.yaml
kubectl apply -f k8s/admin-web-deployment.yaml admin-web-rbac.yaml
kubectl apply -f k8s/node-tuner-daemonset.yaml

# === 启停 / 扩缩 ===
kubectl scale deploy -n thunder thunder-logic --replicas=3
kubectl rollout restart deploy -n thunder thunder-logic
kubectl -n thunder get pods -o wide
kubectl -n thunder logs -f deploy/thunder-logic
```

### 4.3 SO 热更新

```bash
# 一键: CMake 编译 → 上传 MinIO → 下发所有 Pod
python3 tools/build_and_deploy.py \
  --target ModuleHelloHttp --type HelloHttp --admin http://192.168.3.61:30090

# 上传已有 .so 跳过构建
python3 tools/build_and_deploy.py \
  --so ./ModuleHello.so --type HelloHttp --admin http://192.168.3.61:30090

# 查看已部署
python3 tools/build_and_deploy.py --type HelloHttp --list --admin http://192.168.3.61:30090

# curl 手动
curl -X PUT --data-binary @ModuleHello.so \
  http://192.168.3.61:30090/api/plugins/HelloHttp/ModuleHello.so
curl -X POST -H "Content-Type: application/json" \
  -d '{"filename":"ModuleHello.so"}' \
  http://192.168.3.61:30090/api/plugins/HelloHttp/deploy
```

### 4.4 Lua 热更新

```bash
# CLI 一键
python3 tools/lua_deploy.py --file echo.lua --type HELLO_HTTP

# 查看
python3 tools/lua_deploy.py --type HELLO_HTTP --list

# K8s 直写 etcd
ETCD_POD=$(kubectl get pods -n thunder -l app=thunder-etcd -o jsonpath='{.items[0].metadata.name}')
kubectl exec -n thunder "$ETCD_POD" -- etcdctl \
  put /thunder/config/module/HELLO_HTTP \
  '{"module":[{"url_path":"/hello/lua_echo",
    "so_path":"plugins/HelloHttp_ModuleLua.so",
    "load":true,"version":99,
    "script_content":"function handle_request(msg)\n  ...\nend"}]}'
```

### 4.5 健康检查

```bash
# 服务连通
curl -s -X POST http://192.168.3.61:30006/hello/hello \
  -H "Content-Type: application/json" -d '{"option":"Echo","size":5}'
curl -s http://192.168.3.61:30008/Interface/gentoken

# etcd 注册表
ETCD_POD=$(kubectl get pods -n thunder -l app=thunder-etcd -o jsonpath='{.items[0].metadata.name}')
kubectl exec -n thunder "$ETCD_POD" -- etcdctl get /thunder/registry/ --prefix --keys-only

# Worker 日志
kubectl -n thunder exec deploy/thunder-logic -- tail -f /app/log/worker_0.log
```

### 4.6 故障排查

```bash
# Pod 状态
kubectl -n thunder describe pod <pod-name>

# etcd 健康
kubectl -n thunder exec deploy/thunder-logic -- curl -s http://thunder-etcd.thunder:2379/health

# 路由表检查 (Logic 是否注册)
kubectl exec -n thunder "$ETCD_POD" -- etcdctl get /thunder/registry/ --prefix

# 强制重新注册
kubectl exec -n thunder "$ETCD_POD" -- etcdctl del /thunder/registry/ --prefix

# Worker 进程
kubectl -n thunder exec deploy/thunder-logic -- pgrep -a Hello
```

### 4.7 Node 性能初始化

```bash
# 新 Node 加入后运行一次
sudo bash k8s/init-k8s-node.sh               # 完整初始化
sudo bash k8s/init-k8s-node.sh --dry-run     # 仅检查

# DaemonSet 自动化
kubectl apply -f k8s/node-tuner-daemonset.yaml
```

初始化项: CPU Manager static / NUMA 绑核 / TCP buffer sysctl / CPU governor=performance / THP=madvise / KeepAlive 优化。

### 4.8 管理控制台

| 服务 | 地址 |
|------|------|
| admin-web | `http://192.168.3.61:30090` |
| MinIO Console | `http://192.168.3.61:30091` (minioadmin/minioadmin) |

---

## 5. 文件清单

### 基础设施

| 文件 | 说明 |
|------|------|
| `namespace.yaml` | thunder namespace |
| `etcd-statefulset.yaml` | etcd StatefulSet (3 节点, PVC) |
| `etcd-pv.yaml` | etcd PV |
| `redis.yaml` | Redis Deployment+Service |
| `mysql.yaml` | MariaDB Deployment+Service |
| `minio.yaml` | MinIO 制品库 (PV+PVC+Secret+Deployment+Service) |

### 业务服务

| 文件 | 说明 |
|------|------|
| `logic-deployment.yaml` | Logic (纯后端 S2S) |
| `logic-v2-deployment.yaml` | Logic v2 (灰度测试) |
| `interface-deployment.yaml` | Interface (API 网关, NodePort) |
| `hello-deployment.yaml` | HelloHttp (NodePort 30006) |
| `hello-https-deployment.yaml` | HelloHTTPS (NodePort 30043) |
| `hello-ws-deployment.yaml` | HelloWS (NodePort 30010) |
| `hello-wss-deployment.yaml` | HelloWSS (WebSocket Secure) |
| `mqtt-broker-deployment.yaml` | MQTT Broker |
| `admin-web-deployment.yaml` | Admin-Web |
| `admin-web-pvc.yaml` | Admin-Web 持久化存储 |
| `admin-web-rbac.yaml` | Admin-Web RBAC |

### 运维

| 文件 | 说明 |
|------|------|
| `logic-hpa.yaml` | Logic HPA |
| `node-tuner-daemonset.yaml` | Node 性能调优 DaemonSet |
| `init-k8s-node.sh` | 新 Node 初始化脚本 |
| `regression-test.sh` | K8s 回归测试入口 |
