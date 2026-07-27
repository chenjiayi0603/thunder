# SO 模块热更新 via etcd

> 源码: `code/Net/src/labor/Manager.cpp` (ConfigUpdated→GracefulRestartWorker), `code/Net/src/labor/Worker.cpp:5086` (dlopen), `deploy/admin-web/server.py` (PUT + etcd notify)

---

## 设计：三步流程

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
| K8s (kubeadm) | `/data/thunder/plugins/` | NFS PV + PVC (RWX), subPath 挂载 |

### Worker 加载路径

```cpp
// Worker.cpp — 拼出最终路径
strSoPath = m_strWorkPath + "/" + oSoConf[i]("so_path");
dlopen(strSoPath, RTLD_NOW);
```

路径对齐规则：**admin-web 写的目录 = Worker dlopen 的目录**。

---

## 原地覆盖安全性

SO 热更新采用**同名文件原地覆盖**策略：新 .so 覆盖旧 .so，路径不变。

```
PUT /plugins/HelloHttp/ModuleHello.so  →  覆盖同一文件
                                            │
  老 Worker (drain 中)                      新 Worker (刚 fork)
  ┌─────────────────────┐                  ┌─────────────────────┐
  │ dlopen 时 mmap 的    │                  │ dlopen 新文件内容    │
  │ 旧 inode → 内存保持   │                  │ 获得新代码           │
  │ 不重新 dlopen         │                  │                     │
  │ 只排空已有请求 → 退出  │                  │ 接收新请求           │
  └─────────────────────┘                  └─────────────────────┘
```

| 机制 | 说明 |
|------|------|
| `RTLD_NODELETE` | dlopen 标志，库加载后不会被 dlclose 卸载 |
| Linux mmap 语义 | 内核持有旧 inode 引用，文件被覆盖后旧进程内存映射不变 |
| Drain 不重载 | 老 Worker 进入 drain 后只处理已有连接，不调用 dlopen |

### GracefulRestart 时序

```
Manager 检测 etcd version 变化
  │
  ├─► fork+exec 新 Worker
  │     └─► dlopen("plugins/xxx.so") → 加载新 .so
  │
  ├─► 老 Worker EnterDrainMode()
  │     └─► 继续服务已有请求 (DRAIN_GRACE_PERIOD)
  │     └─► 不重新 dlopen，不碰磁盘文件
  │
  └─► 老 Worker drain 完成 → exit(0)
        └─► 内核释放旧 inode 引用
```

---

## etcd 通知实现

```python
def _notify_etcd_so_update(self, type_dir, so_path):
    """PUT 写文件后调用：读 etcd 模块配置 → 匹配 so_path → version++ → 写回"""
    # 1. type_dir → node_type 反向映射 (HelloHttp → HELLO_HTTP)
    # 2. etcd GET /thunder/config/module/{node_type}
    # 3. 遍历 module[] 找 match so_path
    # 4. version += 1
    # 5. etcd PUT 写回 → Manager watch → ConfigUpdated → GracefulRestartWorker
```

---

## SO 热更新 vs Lua 热更新

| | SO 热更新 | Lua 热更新 |
|------|----------|----------|
| 上传接口 | `PUT /plugins/{TypeDir}/{filename}` | `POST /api/lua-scripts` |
| 存储 | NFS 文件覆盖 | NFS 写脚本 + etcd 写 script_content |
| etcd 通知 | `_notify_etcd_so_update` (version++) | `_lua_push` (version++) |
| Worker 响应 | GracefulRestartWorker (fork+exec) | 原地 Lua VM 重载 (无进程重启) |
| 重启方式 | 新旧 Worker 交替，drain 后退出 | 无需重启，直接更新 Lua 函数表 |
| 安全机制 | RTLD_NODELETE + mmap + drain | Lua sandbox + 原子替换 |

---

## 关键文件

| 文件 | 职责 |
|------|------|
| `deploy/admin-web/server.py` | PUT 接收 + 双写（本地 + NFS）+ etcd 通知 |
| `k8s/hello-deployment.yaml` | Worker 部署（hostPath + NFS subPath） |
| `k8s/plugins-pv.yaml` | 共享存储 PV/PVC |
| `code/Net/src/labor/Manager.cpp` | ConfigUpdated → GracefulRestartWorker |
| `code/Net/src/labor/Worker.cpp` | dlopen 路径拼接 |
