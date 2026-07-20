# Thunder K8s 运维手册

> 集群: kubeadm v1.32.13 | CNI: flannel v0.28.5 | Runtime: containerd | OS: Ubuntu 26.04
> 更新: 2026-07-14

---

## 一、快速操作（已验证）

### 1.1 一键发布

```bash
# 编译 → 镜像 → 导入 containerd → 部署 → 回归测试
./deploy.sh release k8s

# 或分步：
./deploy.sh build                          # cmake 编译
./deploy.sh image logic interface logic-v2 hello http https ws wss  # 构建镜像
./deploy.sh deploy                         # 导入 containerd + kubectl apply
bash k8s/regression-test.sh                # 回归测试
```

### 1.2 导入镜像到 containerd

```bash
# 需要 sudo 密码
docker save thunder-logic:latest | sudo ctr -n k8s.io image import -
```

### 1.3 灰度权重管理 (`canary.py`)

```bash
# 端口转发 etcd 到本地
kubectl port-forward -n thunder pod/thunder-etcd-0 12379:2379 &

# 查看当前权重
ETCD_ENDPOINT=127.0.0.1:12379 python3 tools/canary.py LOGIC
# →  LOGIC: 无灰度配置（使用默认一致性哈希路由）

# 灰度 v2 占 30%
ETCD_ENDPOINT=127.0.0.1:12379 python3 tools/canary.py LOGIC canary v2 30
# → ✅ LOGIC: {"v2": 30, "v1": 70}
#     v1:   70 ( 70.0%)  ██████████████░░░░░░
#     v2:   30 ( 30.0%)  ██████░░░░░░░░░░░░░░

# 全量切换 v2
ETCD_ENDPOINT=127.0.0.1:12379 python3 tools/canary.py LOGIC full v2
# → ✅ LOGIC 全量切换 → v2

# 回滚
ETCD_ENDPOINT=127.0.0.1:12379 python3 tools/canary.py LOGIC rollback
# → ✅ LOGIC 已回滚 → v1=100%

# 清除灰度
ETCD_ENDPOINT=127.0.0.1:12379 python3 tools/canary.py LOGIC reset
```

### 1.4 Lua 热更新（直接 etcd）

```bash
ETCD_POD=$(kubectl get pods -n thunder -l app=thunder-etcd -o jsonpath='{.items[0].metadata.name}')

# 写新 Lua 脚本（version 99，script_content 含新逻辑）
kubectl exec -n thunder "$ETCD_POD" -- etcdctl --endpoints=http://127.0.0.1:2379 \
  put /thunder/config/module/HELLO_HTTP \
  '{"module":[{"url_path":"/hello/lua_echo","so_path":"plugins/HelloHttp_ModuleLua.so","entrance_symbol":"create","load":true,"version":99,"script_content":"function handle_request(msg)\n  SendToClientFast('"'"'{\"code\":0,\"msg\":\"NEW_LOGIC\"}'"'"')\n  return true\nend"}]}'

# 验证：请求应返回新逻辑
curl -s -X POST http://192.168.3.61:27006/hello/lua_echo -d 'test'
# → {"code":0,"msg":"NEW_LOGIC"}

# 全链路日志确认：
# Manager: ConfigUpdated → CMD_REQ_RELOAD_LUA
# Worker:  UnloadSoAndDeleteModule → LoadSoAndGetModule → ModuleLua::Init
```

### 1.5 回归测试

```bash
bash k8s/regression-test.sh
# 通过: 19  失败: 0  跳过: 0  总计: 19

# 测试覆盖：
#  1. CoreDNS Running
#  2. 5 个网关 Pod Running
#  3. 5 个网关插件隔离 (/app/plugins/)
#  4. DNS 解析 (resolv.conf, 短名, FQDN)
#  5. 服务直连 (hello, https, ws, wss, interface)
```

### 1.6 快速验证

```bash
# 集群状态
kubectl get pods -n thunder

# 服务连通性
curl -s -X POST http://192.168.3.61:27006/hello/hello \
  -H "Content-Type: application/json" -d '{"option":"Echo","size":5}'
# → {"code":0,"msg":"ok","size":5,"data":"XXXXX"}

# etcd canary 权重
kubectl exec -n thunder thunder-etcd-0 -- \
  etcdctl --endpoints=http://127.0.0.1:2379 get /thunder/canary/ --prefix

# Worker 渲染追踪
kubectl logs -n thunder deploy/thunder-hello --tail=5
```

---

## 二、集群概览

### 拓扑

```
外部客户端
  │
  ├─► 192.168.3.61:27006 ──► hello Pod (hostNetwork, HTTP)
  ├─► 192.168.3.61:27443 ──► hello-https Pod (hostNetwork, HTTPS)
  ├─► 192.168.3.61:27010 ──► hello-ws Pod (hostNetwork, WS)
  ├─► 192.168.3.61:27012 ──► hello-wss Pod (hostNetwork, WSS)
  └─► 192.168.3.61:27008 ──► interface Pod (hostNetwork, API 网关)

内部:
  control-plane (192.168.3.61)
    ├─► kube-apiserver :6443
    ├─► flannel CNI (Pod CIDR: 10.244.0.0/16)
    ├─► CoreDNS (10.96.0.10) — ClusterFirstWithHostNet DNS
    └─► containerd
```

### 入口/出口

| 入口 | 地址 | 协议 | 用途 |
|------|------|------|------|
| hello (HTTP) | `192.168.3.61:27006` | HTTP | Echo / Lua 路由 |
| hello-https | `192.168.3.61:27443` | HTTPS | 加密 echo |
| hello-ws | `192.168.3.61:27010` | WebSocket | 长连接 |
| hello-wss | `192.168.3.61:27012` | WSS | 加密 WebSocket |
| interface | `192.168.3.61:27008` | HTTP | API 网关 (→ Logic) |
| etcd (内部) | `thunder-etcd.thunder:2379` | gRPC | 配置中心 + 服务发现 |

### 关键组件

| 组件 | 类型 | 用途 |
|------|------|------|
| flannel | CNI | Pod 网络 (10.244.0.0/16) |
| CoreDNS | DNS | 集群内服务发现 (10.96.0.10) |
| etcd (thunder) | StatefulSet×3 | 应用层配置中心、服务发现、canary 权重 |
| Redis | Deployment | 缓存 |
| MySQL | Deployment | 持久化 |

### 部署模型

**镜像流转：**

```
构建机                   镜像仓库                      客户 K8s 集群
docker build     push   harbor.customer.com/    imagePullPolicy:
thunder-hello ────────► thunder/hello-http ──── IfNotPresent
                        (私有 Harbor / 公开 Docker Hub)   kubectl apply -f k8s/
```

| 服务 | 网络 | 镜像 | 插件 |
|------|:--:|------|------|
| 网关 (hello/interface) | hostNetwork | Docker (`/app/`) | 烘焙 (`/app/plugins/`) |
| Logic | ClusterIP | Docker (`/app/`) | 烘焙 (`/app/plugins/`) |
| etcd/Redis/MySQL | ClusterIP | 官方镜像 | — |

> **已废弃**: NFS 共享存储、NodePort 暴露、PVC subPath 隔离。改为 Docker 镜像部署。

---

## 二、集群搭建

### 前置条件

- Ubuntu 26.04（或 24.04/22.04）
- containerd 已安装
- 禁用 swap：`swapoff -a`
- 代理：Clash Verge `127.0.0.1:7897`（下载镜像用）

#### 内核模块 + bridge sysctl（必须持久化，否则每次重启 flannel 挂）

```bash
# 加载 br_netfilter（flannel vxlan 依赖）
sudo modprobe br_netfilter

# 持久化：systemd 开机自动加载
cat <<'EOF' | sudo tee /etc/modules-load.d/br_netfilter.conf
br_netfilter
EOF

# 持久化 bridge-nf sysctl（K8s 网络依赖）
cat <<'EOF' | sudo tee /etc/sysctl.d/99-kubernetes-bridge.conf
net.bridge.bridge-nf-call-iptables=1
net.bridge.bridge-nf-call-ip6tables=1
EOF

# 立即生效
sudo sysctl --system

# 验证
lsmod | grep br_netfilter
sysctl net.bridge.bridge-nf-call-iptables   # 必须 = 1
```

> **为什么重启会掉？** `modprobe br_netfilter` 只在当前运行时加载，没有写入 `/etc/modules-load.d/` 就不会开机自动加载。flannel 启动时读 `/proc/sys/net/bridge/bridge-nf-call-iptables` 和 `/run/flannel/subnet.env`，模块未加载直接 Error → 所有 ClusterIP Pod `FailedCreatePodSandBox`。

### 1. 安装 kubeadm

```bash
# 配置代理（下载 K8s 仓库时需要）
export https_proxy=http://127.0.0.1:7897 http_proxy=http://127.0.0.1:7897

# 添加 K8s 仓库
sudo apt-get update
sudo apt-get install -y apt-transport-https ca-certificates curl gpg
curl -fsSL https://pkgs.k8s.io/core:/stable:/v1.32/deb/Release.key | \
  sudo gpg --dearmor -o /etc/apt/keyrings/kubernetes-apt-keyring.gpg
echo 'deb [signed-by=/etc/apt/keyrings/kubernetes-apt-keyring.gpg] https://pkgs.k8s.io/core:/stable:/v1.32/deb/ /' | \
  sudo tee /etc/apt/sources.list.d/kubernetes.list
sudo apt-get update
sudo apt-get install -y kubelet kubeadm kubectl
sudo apt-mark hold kubelet kubeadm kubectl
```

### 2. 初始化 control-plane

```bash
sudo kubeadm init \
  --pod-network-cidr=10.244.0.0/16 \
  --service-cidr=10.96.0.0/12 \
  --apiserver-advertise-address=192.168.3.61

# 配置 kubectl
mkdir -p $HOME/.kube
sudo cp -i /etc/kubernetes/admin.conf $HOME/.kube/config
sudo chown $(id -u):$(id -g) $HOME/.kube/config

# 允许 control-plane 调度 Pod（单节点必须）
kubectl taint nodes --all node-role.kubernetes.io/control-plane-
```

### 3. 安装 CNI (flannel)

```bash
# 下载（需要代理！端口 7897）
export https_proxy=http://127.0.0.1:7897
curl -sSL -o /tmp/kube-flannel.yml \
  https://raw.githubusercontent.com/flannel-io/flannel/v0.28.5/Documentation/kube-flannel.yml

# 确认 initContainer command 是 cp（不是 /opt/bin/install-conf）
grep -A5 'install-cni' /tmp/kube-flannel.yml

kubectl apply -f /tmp/kube-flannel.yml
```

### 4. 配置 NFS 共享存储

```bash
# 宿主机安装 NFS Server
sudo apt-get install -y nfs-kernel-server
sudo mkdir -p /data/thunder/plugins
echo '/data/thunder/plugins *(rw,sync,no_subtree_check,no_root_squash)' | sudo tee -a /etc/exports
sudo exportfs -ra

# 创建 NFS PV + PVC
kubectl apply -f - <<'EOF'
apiVersion: v1
kind: PersistentVolume
metadata:
  name: pv-thunder-plugins-nfs
spec:
  accessModes: [ReadWriteMany]
  capacity: {storage: 10Gi}
  nfs:
    server: 192.168.3.61
    path: /data/thunder/plugins
  persistentVolumeReclaimPolicy: Retain
---
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: thunder-plugins
  namespace: thunder
spec:
  accessModes: [ReadWriteMany]
  resources: {requests: {storage: 10Gi}}
  volumeName: pv-thunder-plugins-nfs
EOF
```

### 5. 部署 Thunder 服务

```bash
# 一键部署（编译 + 镜像 + 导入 + apply）
./deploy.sh release k8s

# 或手动分步：
kubectl apply -f k8s/namespace.yaml
kubectl apply -f k8s/etcd-pv.yaml
kubectl apply -f k8s/etcd-statefulset.yaml
kubectl apply -f k8s/redis.yaml
kubectl apply -f k8s/mysql.yaml
kubectl apply -f k8s/logic-deployment.yaml
kubectl apply -f k8s/logic-v2-deployment.yaml
kubectl apply -f k8s/interface-deployment.yaml
kubectl apply -f k8s/hello-deployment.yaml
kubectl apply -f k8s/hello-https-deployment.yaml
kubectl apply -f k8s/hello-ws-deployment.yaml
kubectl apply -f k8s/hello-wss-deployment.yaml
kubectl apply -f k8s/node-tuner-daemonset.yaml   # 节点性能优化 (#154)
```

### 6. 构建并导入 admin-web 镜像

```bash
docker build -t thunder-admin-web:latest -f deploy/admin-web/Dockerfile deploy/admin-web/
docker save thunder-admin-web:latest | sudo ctr -n k8s.io image import -
kubectl delete pod -n thunder -l app=thunder-admin-web
```

---

## 三、拓展节点

```bash
# Step 1: control-plane 上获取 join 命令
kubeadm token create --print-join-command

# Step 2: 新节点上执行输出的命令
kubeadm join 192.168.3.61:6443 --token xxx --discovery-token-ca-cert-hash sha256:xxx
```

| 命令 | 在哪执行 | 作用 |
|------|---------|------|
| `kubeadm token create --print-join-command` | control-plane | 生成一次性 token + 输出完整 join 命令 |
| `kubeadm join ... --token ... --hash ...` | 新 worker | 拿 token 报到 → 下载证书 → 加入集群 |

加节点后无需额外操作：
- NFS PV (`server: 192.168.3.61`) 对所有节点可达，kubelet 自动 mount
- 所有节点 Manager watch 同一 etcd，PUT 后 version 变化 → 同时 GracefulRestartWorker
- Pod 调度到新节点 → 自动挂载 NFS → Worker dlopen 同一份 .so

### 查看/删除节点

```bash
kubectl get nodes -o wide
kubectl drain <node> --ignore-daemonsets --delete-emptydir-data
kubectl delete node <node>
# 在节点上: sudo kubeadm reset
```

---

## 四、配置说明

### 关键文件

| 配置 | 路径 | 说明 |
|------|------|------|
| K8s 集群 | `/etc/kubernetes/` | kubeadm 配置 |
| kubeconfig | `~/.kube/config` | kubectl 认证 |
| containerd | `/etc/containerd/config.toml` | 容器运行时 |
| flannel | `kubectl get cm -n kube-flannel kube-flannel-cfg` | CNI (Pod CIDR) |
| NFS exports | `/etc/exports` | 共享目录 |
| admin-web | `deploy/admin-web/server.py` | SO/Lua 上传服务 |
| Hello 配置 | `deploy/HelloHttp/conf/Hello.json` | Worker 配置 |
| etcd 模块配置 | `etcd://thunder-etcd-0:2379` key `/thunder/config/module/{NODE_TYPE}` | 模块版本 |

### admin-web 环境变量

| 变量 | 值 | 用途 |
|------|-----|------|
| `ETCD_URL` | `http://thunder-etcd-0.thunder-etcd.thunder:2379` | etcd 地址 |

### 网络参数

| 参数 | 值 |
|------|-----|
| CNI | flannel v0.28.5 |
| Pod CIDR | `10.244.0.0/16` |
| Service CIDR | `10.96.0.0/12` |
| 代理 | Clash Verge `127.0.0.1:7897` |

### Service 端口

| Service | 类型 | 外部访问 | 容器端口 |
|---------|------|---------|---------|
| thunder-admin-web | NodePort | `:30090` | 8090 |
| thunder-hello | NodePort | `:30006` | 27006 |
| thunder-interface | NodePort | `:30008` | 27008 |
| thunder-etcd | ClusterIP | 内部 | 2379 |
| thunder-mysql | ClusterIP | 内部 | 3306 |
| thunder-redis | ClusterIP | 内部 | 6379 |

---

## 五、集群测试

```bash
# 快速健康检查
kubectl get nodes && kubectl get pods -A

# NFS 协议确认
kubectl exec -n thunder deploy/thunder-hello -- mount | grep plugins
# → type nfs4 ✅

# SO 热更新四端验证
TOKEN="TEST_$(date +%s)" && echo "$TOKEN" > /tmp/test.so
curl -s -X PUT "http://192.168.3.61:30090/plugins/HelloHttp/HelloHttp_ModuleHello.so" \
  --data-binary @/tmp/test.so
# → {"ok":true,...,"etcd_notify":true}
LOCAL=$(md5sum /tmp/test.so | awk '{print $1}')
ADMIN=$(kubectl exec -n thunder deploy/thunder-admin-web -- \
  md5sum /data/thunder/plugins/HelloHttp/HelloHttp_ModuleHello.so | awk '{print $1}')
HELLO=$(kubectl exec -n thunder deploy/thunder-hello -- \
  md5sum /thunder/deploy/HelloHttp/plugins/HelloHttp_ModuleHello.so | awk '{print $1}')
HOST=$(md5sum /data/thunder/plugins/HelloHttp/HelloHttp_ModuleHello.so | awk '{print $1}')
echo "local:$LOCAL admin:$ADMIN hello:$HELLO host:$HOST"

# Worker 优雅重启确认
kubectl exec -n thunder deploy/thunder-hello -- \
  grep "ConfigUpdated.*version changed\|GracefulRestart\|new worker.*ready" \
  /thunder/deploy/HelloHttp/log/Hello_robot.log | tail -5

# dlopen 加载确认
kubectl exec -n thunder deploy/thunder-hello -- \
  grep "FATAL.*cannot load" /thunder/deploy/HelloHttp/log/Hello_robot_W0.log | tail -3
# → 无新 FATAL = 成功

# 业务接口
HELLO_IP=$(kubectl get pod -n thunder -l app=thunder-hello -o jsonpath='{.items[0].status.podIP}')
curl -s -X POST "http://${HELLO_IP}:27006/hello/hello" \
  -H "Content-Type: application/json" -d '{"option":"Echo","size":10}'
```

---


## 4. 稳定性压测

```bash
bash tests/stability_test_k8s.sh --node-ip <节点IP> --duration 180
```

---

## 六、更新升级

### admin-web 代码

```bash
vim deploy/admin-web/server.py
docker build -t thunder-admin-web:latest -f deploy/admin-web/Dockerfile deploy/admin-web/
docker save thunder-admin-web:latest | sudo ctr -n k8s.io image import -
kubectl rollout restart deploy/thunder-admin-web -n thunder
```

### Worker SO（热更新，无需重启 Pod）

```bash
curl -s -X PUT "http://192.168.3.61:30090/plugins/HelloHttp/xxx.so" \
  --data-binary @build_tsan/lib/xxx.so
# → etcd version++ → Worker GracefulRestart → dlopen 新 .so
```

---

## 七、常见问题

| 问题 | 原因 | 解决 |
|------|------|------|
| flannel Error (br_netfilter) | `br_netfilter` 内核模块未加载或未持久化 | `sudo modprobe br_netfilter` + 写入 `/etc/modules-load.d/br_netfilter.conf`（见前置条件） |
| flannel CrashLoop | initContainer command 错误 | 用官方 YAML，command 为 `cp` |
| 所有 ClusterIP Pod `FailedCreatePodSandBox` | flannel 不可用 → `/run/flannel/subnet.env` 缺失 | 先修 flannel→kubelet 重建 sandbox |
| CoreDNS CrashLoop | CNI 未就绪 | flannel Running 后自动恢复 |
| Worker 未启动 | 缺 libjemalloc、libluajit | `apt install -y libjemalloc2 libluajit-5.1-2` |
| Worker 空载 CPU 500m | WorkStealingPool `yield()` 忙等 | 已修复：`yield()` → `cv.wait_for()`，`code/Util/src/thread/work_stealing_pool.h` |
| 外部下载失败 | 没走代理 | `export https_proxy=http://127.0.0.1:7897` |
| 镜像拉取失败 | 未导入 containerd | `docker save \| sudo ctr image import -` |
| SO dlopen 失败 | 不是真 .so | `file xxx.so` 确认是 ELF |
| NFS mount 失败 | 缺 nfs-common | `sudo apt install nfs-common -y` |
| 节点 DiskPressure | 磁盘 >85% | `sudo journalctl --vacuum-size=200M`；`docker system prune -af` |
| NFS mount 失败 | `sudo apt install nfs-common -y` |
| Worker 未启动 | `apt install -y libjemalloc2 libluajit-5.1-2` |

---

## 附录 A：CNI 选型参考

> 本章为参考，当前集群使用 flannel vxlan。

### flannel 内部模式对比

| | vxlan | host-gw |
|------|-------|---------|
| 原理 | UDP 隧道封装/解封装 | 直接加路由表 |
| 数据路径 | Pod → 封包 → 物理网卡 → 对端解包 → Pod | Pod → 路由 → 物理网卡 → 对端 → Pod |
| 延迟 | +0.1~0.2ms | +0.01ms |
| CPU | 有封包开销 | 几乎零 |
| 前提 | 节点间 IP 可达即可 | 所有节点在同一二层网络 |
| 跨子网 | ✅ | ❌ |

### CNI 方案对比

| | flannel | Calico | Cilium |
|------|---------|--------|--------|
| 数据面 | vxlan / host-gw | BGP / vxlan / IPIP | eBPF |
| 性能 | 中(vxlan) / 高(host-gw) | 高 | 最高 |
| 网络策略 | ❌ | ✅ | ✅ (L7) |
| 加密 | ❌ | ✅ WireGuard | ✅ WireGuard / IPsec |
| 可观测 | 无 | Flow Log | Hubble |
| 复杂度 | ⭐ | ⭐⭐⭐ | ⭐⭐⭐⭐⭐ |
| 资源占用 | ~50MB | ~200MB | ~500MB+ |
| 学习成本 | 10 分钟 | 1 天 | 1 周 |

### 节点规模适用

| 节点数 | CNI 建议 | 原因 |
|--------|---------|------|
| 1~3 | flannel host-gw | 极简，同二层网络最佳 |
| 3~10 | flannel host-gw | 够用，无换的理由 |
| 10~50 | flannel vxlan | 跨子网时用 vxlan；仍够用 |
| 50+ | Calico BGP | 路由收敛快，BGP 优化 |

### 换 CNI 的触发条件

- 需要 NetworkPolicy 限制 Pod 间访问 → Calico / Cilium
- 需要 WireGuard 加密东西向流量 → Calico / Cilium
- 需要 L7 可观测 (Hubble) → Cilium
- 节点数超过 50 需要 BGP 路由优化 → Calico

### Thunder 流量特点

Thunder 业务中 CNI 只承载 RPC 和 etcd 心跳，数据量最大的 SO 文件读取走 NFS（宿主机网络），不经过 CNI。因此 flannel 在绝大多数规模下都足够。

### flannel 切换 host-gw

```bash
# 同二层网络（节点在同一网段）推荐切 host-gw
kubectl edit cm kube-flannel-cfg -n kube-flannel
# 改: "Type": "vxlan" → "Type": "host-gw"
# 删 Pod 重建
kubectl delete pod -n kube-flannel -l app=flannel
```

---

## 附录 B：跨机房 / 混合云

### 网络层面

flannel vxlan 协议上支持跨机房（UDP 隧道，只要节点 IP 可达），但有代价：

| 问题 | 说明 |
|------|------|
| MTU | vxlan 头 +50 字节，跨机房链路默认 MTU 1500 → 可能分片 → 性能下降。解决：调低 Pod MTU 或路径 MTU 发现 |
| 延迟 | 北京-上海 30ms+，etcd gRPC 心跳和 RPC 调用延迟放大 |
| 安全 | 公网裸跑 vxlan 不安全，必须走 VPN/专线，或换 Calico WireGuard |

> **有专线就可以**：专线解决了安全（不走公网）和带宽（有 SLA），延迟也远低于公网。Thunder 只在 Worker 启动/重载时读一次 SO 文件，dlopen 之后全在内存，运行时不受跨机房延迟影响。专线场景下 flannel vxlan + NFS 直接可用，无需多机房方案。

### 存储层面（核心瓶颈）

```
PUT 上传 → admin-web (北京) → NFS (192.168.3.61, 北京)
                                 │
                            上海节点 → mount 北京 NFS？
                                 │
                            └── 公网跨机房读 NFS: 延迟高、带宽窄
                            └── 专线跨机房读 NFS: 延迟可控，可以接受
```

**公网跨机房**：NFS 设计用于局域网，不适合跨地域。**有专线**：NFS 跨机房可用，SO 只在 Worker 启动/热重载时读取一次（dlopen 后常驻内存），运行时不受影响。

### 多机房方案

| 方案 | 存储 | 适用 |
|------|------|------|
| A：每机房独立集群 | 每机房自己 NFS + etcd | 机房间无依赖，独立部署 |
| B：对象存储分发 | S3/MinIO 取代 NFS | 跨地域共享 SO 文件 |
| C：NFS 就近读取 | 每机房部署 NFS 缓存节点 | 减少跨地域读 |

```
方案 A（每机房独立）:
  北京: admin-web → NFS(北京) → hello(北京)
  上海: admin-web → NFS(上海) → hello(上海)
  SO 上传两遍，机房之间无依赖

方案 B（对象存储）:
  北京/上海: admin-web → PUT → S3/MinIO → hello 从 S3 拉 SO
  无需 NFS，但 Worker 需要改 SO 加载路径
```

### 判断方法：二层还是三层

```bash
# 两台机器上分别查
ip route get <对方IP>
# 含 "dev eth0 src ... uid" → 直连，二层（host-gw 可用）
# 含 "via 192.168.x.1 dev eth0" → 过网关，三层（必须 vxlan）
```

| 场景 | 二层/三层 | host-gw |
|------|----------|---------|
| 同一办公室/机架 | 二层 | ✅ |
| 同云 VPC 内多台 ECS | 二层（通常） | ✅ |
| 虚拟机桥接 | 二层 | ✅ |
| 跨机房/跨地域 | 三层 | ❌ 必须 vxlan |
| 不同云厂商/混合云 | 三层 | ❌ |
| 虚拟机 NAT | 三层 | ❌ |

---

## 附录 C：HPA 行为详解

### 判断对象

HPA 看的是**所有 Pod 的平均 CPU 利用率**，不是单个 Pod：

```
利用率 = 平均 CPU 使用量 / requests.cpu

3 个 Pod: 200m, 300m, 100m → 平均 200m → 200/500(requests) = 40%
只会看整个 Deployment 的 CPU，不会问"第 1 个 Pod CPU 高要不要扩？"
```

### 扩容逻辑

| 参数 | 值 | 含义 |
|------|-----|------|
| `averageUtilization` | 70% | 平均值 >70% 才考虑扩容 |
| `stabilizationWindowSeconds` | 60 | 取过去 60 秒内最高建议值，防抖动 |
| `policy: Pods 1 / 60s` | — | 每分钟最多扩 1 个 |

```
60 秒窗口内 HPA 建议: 2, 3, 2, 2, 3 → 取最大 3 → 扩
60 秒窗口内 HPA 建议: 2, 1, 1, 1, 1 → 取最大 2 → 扩
只剩 1 个 Pod: 90% > 70% → 扩到 2 ✅
已有 3 个 Pod: 90% > 70%, 建议 4 → 但 max=3, 不扩
```

### 缩容逻辑

| 参数 | 值 | 含义 |
|------|-----|------|
| `stabilizationWindowSeconds` | 300 | 取过去 **5 分钟**最低建议值，防频繁弹 |
| `policy: Pods 1 / 60s` | — | 每分钟最多缩 1 个 |

```
5 分钟窗口内建议: 1, 1, 1, 1, 1 → 稳定 5 分钟都是 1 → 缩
5 分钟窗口内建议: 1, 1, 2, 1, 1 → 有波动 (2) → 不缩，等稳定
```

### 扩容快、缩容慢的原因

- **扩容 1 分钟窗口**：流量突发快速响应
- **缩容 5 分钟窗口**：防止 CPU 短暂下降就缩、然后又扩（抖动）

### 配置文件中查看

`k8s/logic-hpa.yaml`，可直接 `kubectl apply -f` 部署。

---

## 附录 D：Thunder 网关 hostNetwork vs NodePort

### D.1 为什么网关要考虑 hostNetwork

Thunder 作为高性能网关（HTTP 64B 延迟 220μs），K8s 默认的 NodePort 在数据面多一跳 kube-proxy + CNI 转发，额外消耗几~几十 μs。对于 ms 级业务可忽略，但对 Thunder 的 μs 级优势有影响。

### D.2 数据路径对比

```
hostNetwork:
  网卡 → 内核协议栈 → Pod 进程 (直通, 零 K8s 组件)

NodePort:
  网卡 → 内核 iptables/IPVS → CNI veth pair → Pod 进程 (多两跳)
```

### D.3 推荐配置

| 服务类型 | 网络模式 | 原因 |
|---------|:------:|------|
| 网关 (hello/http/ws/https) | **hostNetwork** | 对外服务，延迟敏感，端口固定不冲突 |
| Logic | ClusterIP | 内部通信，可水平扩缩容 |
| etcd / MySQL / Redis | ClusterIP | 内部通信，有状态服务，固定端口 |

### D.4 如何开启

```yaml
spec:
  hostNetwork: true
  dnsPolicy: ClusterFirstWithHostNet  # hostNetwork 必须显式设 DNS 策略
```

### D.5 为什么内部服务不需要 hostNetwork

etcd/MySQL/Redis 一次请求自身百 μs~ms 级，ClusterIP 多几 μs 转发可忽略。它们更需要 K8s 的 Service 抽象（Pod 重启 IP 不变）和扩缩容支持。

### D.6 hostNetwork 下控制面开销

containerd、kubelet、API Server 只影响 Pod 生命周期，不影响已建立连接的数据包转发。hostNetwork 下数据面零 K8s 开销，全部来自内核协议栈。

> 详细分析见 `docs/reference/gateway-deployment.md` §4.3 和附录 A。