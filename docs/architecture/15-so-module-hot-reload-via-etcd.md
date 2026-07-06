# 15 — SO 模块热更新 via etcd

> 2026-06-09 | 更新: 2026-07-06 (K8s NFS mount 对齐) | 状态: ✅ Docker Compose 验证通过，K8s 待验证

## 1. 三步流程

```
  ┌──────── 1. 发布 ────────┐  ┌────── 2. 通知 ──────┐  ┌────────── 3. 加载 ──────────┐
  │                         │  │                     │  │                              │
  │ cmake → .so             │  │ etcd PUT            │  │ Manager Watch                │
  │   │                     │  │ /thunder/config/     │  │   │                          │
  │   ▼                     │  │   module/{TYPE}     │  │   ▼                          │
  │ curl PUT :8090/plugins/ │  │   {"module":[{       │  │ ConfigUpdated                │
  │   {Type}/{file}.so      │  │     "so_path":"...", │  │   ├── 版本比对               │
  │   │                     │  │     "version": N+1   │  │   └── so_path 变了           │
  │   ▼                     │  │   }]}               │  │       → GracefulRestart      │
  │ admin-web 写盘          │  │                     │  │                              │
  │                         │  │                     │  │   旧Worker   新Worker        │
  │ Docker Compose:         │  │                     │  │   drain      dlopen(.so)     │
  │   deploy/{Type}/        │  │                     │  │     ↓          ↓             │
  │     plugins/{file}.so   │  │                     │  │   exit(0)    接管            │
  │                         │  │                     │  │               ✅ 零中断       │
  │ K8s:        写入 NFS ───┼──┼── 所有 Pod 可见 ◄───┼──┘                              │
  │   /data/thunder/plugins/│  │                     │                                  │
  │     {Type}/{file}.so    │  │                     │                                  │
  └─────────────────────────┘  └─────────────────────┘                                  │
```

## 2. 发布：PUT 直接上传

```bash
curl -X PUT http://127.0.0.1:8090/plugins/HelloHttp/xxx.so --data-binary @xxx.so
```

admin-web 收到后写两个位置：

| 位置 | 路径 | 环境 |
|------|------|------|
| 本地 | `deploy/HelloHttp/plugins/xxx.so` | Docker Compose |
| NFS | `/data/thunder/plugins/HelloHttp/xxx.so` | K8s |

## 3. 通知：etcd 版本变更

PUT 成功后，更新 etcd key `/thunder/config/module/HELLO_HTTP`，写入模块配置（version 递增）：

```json
{
  "module": [{
    "url_path": "/hello/hello",
    "so_path": "plugins/HelloHttp_ModuleHello.so",
    "entrance_symbol": "create",
    "version": 2
  }]
}
```

Manager 通过 Watch 检测到 version 变化。

## 4. 加载：GracefulRestartWorker → dlopen

Worker 加载路径：

```cpp
// Worker.cpp:5086
strSoPath = m_strWorkPath + "/" + oSoConf[i]("so_path");
// → /thunder/deploy/HelloHttp/plugins/HelloHttp_ModuleHello.so
dlopen(strSoPath, RTLD_NOW);
```

### Docker Compose 路径

```
admin-web:  PUT → /home/tommychen/thunder/deploy/HelloHttp/plugins/xxx.so
Worker:     dlopen → /thunder/deploy/HelloHttp/plugins/xxx.so  ← 同一挂载，同路径 ✅
```

### K8s 路径（方案 A：NFS mount 对齐）

```
admin-web:  PUT → /data/thunder/plugins/HelloHttp/xxx.so  (NFS)
Worker:     dlopen → /thunder/deploy/HelloHttp/plugins/xxx.so

对齐方式：K8s Deployment 中 mount NFS 到 /thunder/deploy/HelloHttp/plugins/
```

```yaml
# hello-deployment.yaml
spec:
  containers:
  - name: hello
    volumeMounts:
    - name: nfs-plugins
      mountPath: /thunder/deploy/HelloHttp/plugins    # ← 跟 Worker m_strWorkPath 对齐
  volumes:
  - name: nfs-plugins
    nfs:
      server: 192.168.3.100
      path: /data/thunder/plugins
```

这样 Worker `dlopen("plugins/xxx.so")` 直接从 NFS 读取，不改代码。

## 5. 对比

| | Docker Compose | K8s |
|------|------|------|
| 存储 | 宿主机 ext4 `/home/tommychen/thunder` | NFS 服务器 `/data/thunder/plugins/` |
| Pod 如何读 | 同 mount，本地文件 | NFS mount 到 Worker 读路径 |
| 代码改动 | 0 | 0（只改 deployment YAML） |
| 验证状态 | ✅ PUT → dlopen 全链路通过 | ⚠️ 待验证 |

## 6. etcd Key 结构

```
/thunder/config/module/{TYPE}
  → {"module":[{so_path, version, url_path?, so_url?}]}
```

## 7. 关键文件

| 文件 | 职责 |
|------|------|
| `deploy/admin-web/server.py` | PUT 接收 + 双写（本地 + NFS） |
| `k8s/plugins-pv.yaml` | NFS PersistentVolume |
| `k8s/hello-deployment.yaml` | 需 mount NFS 到 Worker 读路径 |
| `code/Net/src/labor/Manager.cpp:2735-2866` | ConfigUpdated → GracefulRestartWorker |
| `code/Net/src/labor/Worker.cpp:5086` | dlopen 路径拼接 |
