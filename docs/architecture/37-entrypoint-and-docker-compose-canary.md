# Entrypoint 机制 & Docker Compose Canary 灰度测试

> 最后更新：2026-07-12

---

## 一、`ENTRYPOINT ["./entrypoint.sh"]` 是什么

### Docker 容器启动流程

Dockerfile 中的 ENTRYPOINT 定义了容器的 PID 1 进程：

```dockerfile
FROM ubuntu:26.04
COPY deploy/HelloHttp/ /app/
WORKDIR /app
ENTRYPOINT ["./entrypoint.sh"]    # ← 容器启动时执行 /app/entrypoint.sh
```

启动容器等价于：

```bash
docker run ... thunder-hello-http:latest
# → PID 1 = /app/entrypoint.sh
```

K8s 里也是同一个机制：`kubectl apply -f hello-deployment.yaml` 后，kubelet 拉镜像、启动容器，执行的还是 `entrypoint.sh`。

### 为什么不直接用 K8s `command:` 字段

因为 `entrypoint.sh` 需要在**二进制启动前**做配置替换——把 `0.0.0.0` 换成真实 Pod IP。K8s 的 `command:` 只能执行一条命令，做不了这个。

**Docker Compose 不用 `ENTRYPOINT`**——compose 文件用 `command: node.sh start` 覆盖了镜像里的 ENTRYPOINT。

---

## 二、entrypoint.sh 做了什么（以 HelloHttp 为例）

```bash
#!/bin/bash
set -e

# ① 设置 inner_port（环境变量覆盖 conf 文件）
PORT="${INNER_PORT:-27007}"
[ -n "$INNER_PORT" ] && sed -i "s/\"inner_port\": [0-9]*/\"inner_port\": $INNER_PORT/" ./conf/Hello.json

# ② 替换 inner_host（hostNetwork Pod 注册 0.0.0.0 会导致其他节点连不上）
MY_IP="${POD_IP:-$(hostname -i 2>/dev/null || echo '0.0.0.0')}"
[ "$MY_IP" != "0.0.0.0" ] && sed -i "s/\"inner_host\": \"0.0.0.0\"/\"inner_host\": \"$MY_IP\"/" ./conf/Hello.json

# ③ 启动 Thunder 二进制（会 daemonize：父进程 fork 后退出）
echo "Starting HelloHttp on $MY_IP:$PORT..."
./bin/HelloHttp ./conf/Hello.json

# ④ 检查退出码
EXIT=$?
if [ $EXIT -ne 0 ]; then
    echo "FATAL: HelloHttp exited with code $EXIT"
    exit $EXIT
fi

# ⑤ tail -f 日志（让 kubectl logs 可用）
tail -f log/Hello_robot.log 2>/dev/null &

# ⑥ 保持容器存活（二进制 daemonize 后父进程已退出）
sleep infinity
```

### 为什么不能用 `exec`（#14 修复）

Thunder 二进制启动后会 **daemonize**（父进程 fork 子进程后退出）。如果 entrypoint 用了 `exec ./bin/HelloHttp`，父进程退出 = 容器退出 = K8s 判定 CrashLoopBackOff。去掉 `exec` 后，父进程（bash）继续执行 `sleep infinity` 保持容器存活。

---

## 三、Docker Compose Canary 灰度测试

### 3.1 前提

docker-compose.yml 已包含 `logic`（v1, port 16068）和 `logic-v2`（v2, port 16069）两个服务。

```yaml
# docker/docker-compose.yml
logic:
  environment:
    NODE_VERSION: v1      # ← 注册到 etcd 时带 node_version=v1
    INNER_PORT: "16068"

logic-v2:
  environment:
    NODE_VERSION: v2      # ← 注册到 etcd 时带 node_version=v2
    INNER_PORT: "16069"
  working_dir: /thunder/deploy/Logic_v2
```

### 3.2 启动全栈

```bash
cd docker/
rm -rf data/etcd1 data/etcd2 data/etcd3  # 清空旧 etcd
docker compose build
docker compose up -d
docker compose ps  # 确认全部 healthy
```

### 3.3 验证 v1/v2 都已注册

```bash
docker compose exec etcd1 etcdctl get --prefix /thunder/registry/LOGIC/

# 预期输出：
# /thunder/registry/LOGIC/127.0.0.1:16068
# {"node_type":"LOGIC","node_version":"v1",...}
# /thunder/registry/LOGIC/127.0.0.1:16069
# {"node_type":"LOGIC","node_version":"v2",...}
```

### 3.4 执行 Canary 灰度

```bash
# 查看当前权重（无灰度配置时走一致性哈希）
PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python python3 tools/canary.py LOGIC

# v2 灰度 10%
PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python python3 tools/canary.py LOGIC canary v2 10

# 逐步放量
PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python python3 tools/canary.py LOGIC canary v2 50
PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python python3 tools/canary.py LOGIC full v2

# 回滚
PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python python3 tools/canary.py LOGIC rollback

# 删除权重键，恢复一致性哈希
PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python python3 tools/canary.py LOGIC reset
```

### 3.5 验证权重已生效

```bash
# Hello Worker 日志中应看到 canary 权重展开
docker compose exec hello tail -50 log/Hello_robot.log | grep Canary
# 预期：
# CanaryParsed[LOGIC]: v1=90 v2=10
# CanaryExpand[LOGIC]: 127.0.0.1:16068=90 127.0.0.1:16069=10
```

---

## 四、entrypoint.sh 对照表

| 服务 | entrypoint | 默认端口 | conf 文件 | 启动命令 |
|------|-----------|:---:|------|------|
| HelloHttp | `deploy/HelloHttp/entrypoint.sh` | 27007 | `Hello.json` | `./bin/HelloHttp ./conf/Hello.json` |
| HelloHttps | `deploy/HelloHttps/entrypoint.sh` | 27444 | `HelloHttps.json` | `./bin/HelloHttps ./conf/HelloHttps.json` |
| HelloWs | `deploy/HelloWs/entrypoint.sh` | 27011 | `HelloWs.json` | `./bin/HelloWs ./conf/HelloWs.json` |
| HelloWss | `deploy/HelloWss/entrypoint.sh` | 27445 | `HelloWss.json` | `./bin/HelloWss ./conf/HelloWss.json` |
| Interface | `deploy/Interface/entrypoint.sh` | 27009 | `Interface.json` | `./bin/Interface ./conf/Interface.json` |
| Logic | `deploy/Logic/entrypoint.sh` | 16068 | `Logic.json` | `./bin/Logic ./conf/Logic.json` |
| Logic v2 | `deploy/Logic_v2/entrypoint.sh` | 16069 | `Logic.json` | `./bin/Logic ./conf/Logic.json` |

---

## 五、K8s vs Docker Compose 启动机制对比

| | K8s | Docker Compose |
|---|---|---|
| 入口 | `ENTRYPOINT ["./entrypoint.sh"]` | `command: node.sh start` |
| IP 来源 | `POD_IP` env（downward API） | `hostname -i`（host 网络） |
| 容器保活 | `sleep infinity` | `tail -f /dev/null & wait` |
| 日志 | `kubectl logs` | 宿主机挂载目录 |
| `NODE_VERSION` | Deployment YAML `env:` | compose `environment:` |
| v2 节点 | 单独 Deployment | compose 加一个 service |
| 扩缩容 | `kubectl scale --replicas=3` | `docker compose up --scale logic-v2=3` |
| 端口约束 | 调度到不同节点可共用 | host 网络下必须不同 |
