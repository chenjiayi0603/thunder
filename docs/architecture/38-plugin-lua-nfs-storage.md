# 38 — 插件 / Lua / NFS 存储架构

## 名词（不需要 K8s 背景）

| 词 | 大白话 |
|----|--------|
| **etcd** | 一个分布式配置中心，存 key-value，Gateway 每 5 秒去读有没有新配置 |
| **Pod** | K8s 里最小的运行单位，你可以理解为一个"容器 + 它的网络/存储配置" |
| **Gateway** | Thunder 的服务进程（HelloHttp、Logic 等），负责处理网络请求 |
| **SO 插件** | `.so` 文件，C++ 编译出来的动态库，Gateway 启动时加载 |
| **Lua 脚本** | `.lua` 文本文件，在 LuaJIT 虚拟机里执行，可以不停机热更新 |
| **镜像** | Docker 镜像，打包了程序和所有依赖，Pod 用它来启动 |
| **kubelet** | 每台 K8s 节点上跑的代理程序，负责把 Pod 拉起来、挂载存储 |
| **NFS** | 网络文件系统，一台机器共享目录，其他机器通过网络挂载使用 |
| **PV / PVC** | K8s 的存储抽象。PV=实际存储空间，PVC=申请使用这块空间 |
| **hostPath** | 直接用宿主机上的目录，不经过网络 |
| **export** | NFS 术语：把本机某个目录"分享出去"，让别的机器能挂载 |
| **volume / mount** | K8s 术语：把一块存储（NFS/本地目录）挂到 Pod 容器里，进程就能读写 |
| **server** | yaml 配置里的一个字段，值就是 NFS 服务器的 IP 地址 |
| **容器** | 一个隔离的运行环境，里面的程序看到的是自己的文件系统 |

## 一张图

```
                         etcd
               /thunder/config/module/{TYPE}         ← Gateway 每 5s 读，版本变了就热重载
               ▲       ▲
          读   │  写   │
               │       │
      ┌────────┘       └──────────┐
      │                           │
      ▼                           │
┌──────────┐             ┌──────────────────────────────────────┐
│ Gateway  │             │            admin-web Pod              │
│ Pod      │             │                                      │
│          │             │  POST /api/lua/HELLO_HTTP            │
│ SO 插件:  │             │    ├─→ 写 etcd                       │
│ /app/    │             │    │     (Gateway 读到就热重载)        │
│ plugins/ │             │    │                                  │
│ *.so     │             │    └─→ 写 NFS 落盘 ╮                  │
│ (镜像    │             │                    │                  │
│  内置)   │             │  容器内 mount:      ▼                  │
└──────────┘             │  /data/thunder/plugins/              │
                         │      HELLO_HTTP/scripts/lua_echo.lua │
                         └──────────────┬───────────────────────┘
                                        │ NFS mount
                                        │ (kubelet 挂载)
                          ┌─────────────▼─────────────┐
                          │  宿主机 192.168.3.61       │
                          │  /data/thunder/plugins/   │
                          │    HELLO_HTTP/scripts/    │
                          │      lua_echo.lua         │
                          │                           │
                          │  nfs-kernel-server (apt)  │
                          │  export 上面这个目录        │
                          └───────────────────────────┘
```

## 一句话总结

| 东西 | 存在哪 | 谁用 | 怎么分发 |
|------|--------|------|---------|
| **SO 插件** `.so` | 镜像内 `/app/plugins/` | Gateway 进程 dlopen | 编译 → COPY 进镜像，不共享 |
| **Lua 脚本** `.lua` | ① etcd (热重载) ② 宿主机 NFS (备份) | Gateway LuaJIT VM | admin-web POST → 同时写两处 |

## SO 插件 — 镜像内置，各 Gateway 独立

编译 → `deploy/{Svc}/plugins/{Name}.so` → Dockerfile `COPY plugins/ /app/plugins/`

| Gateway | 自带的 .so |
|---------|-----------|
| HelloHttp | `ModuleHello.so` `ModuleLua.so` `ModuleRaw.so` |
| HelloHttps | `ModuleHello.so` `ModuleRaw.so` |
| HelloWs/Wss | `CmdHello.so` `ModuleShake.so` |
| Interface | `ModuleInterface.so` |
| Logic | `CmdGetToken.so` `ModuleLua.so` |

> 每个 Pod 只带自己的插件，不跨服务共享。K8s 回归已验证 5 个 Gateway 互不污染。

## 参考

| 项目 | 值 |
|------|-----|
| etcd key | `/thunder/config/module/HELLO_HTTP` |
| NFS 落盘路径 | `/data/thunder/plugins/HELLO_HTTP/scripts/lua_echo.lua` |
| etcd 不可用时 | admin-web 直接报错 |
| NFS 不可用时 | 热重载仍生效 (etcd 是主路径) |
| 单节点 | `k8s/admin-web-deployment.yaml` 第 31 行 `server: 127.0.0.1` 就是本机，本机装了 NFS，不用改 |
| 多节点 | 打开 `k8s/admin-web-deployment.yaml`，找到第 31 行 `server: 127.0.0.1`，把 `127.0.0.1` 换成装了 NFS 那台机器的 IP |

## 相关 K8s 资源

### admin-web 的 NFS 挂载 — `k8s/admin-web-deployment.yaml`

```yaml
volumes:
- name: nfs-plugins
  nfs:
    server: 127.0.0.1              # ← 单节点不改，多节点换成 NFS 机器 IP
    path: /data/thunder/plugins    # ← 远端的共享目录
containers:
- volumeMounts:
  - name: nfs-plugins
    mountPath: /data/thunder/plugins  # ← 挂到容器里这个路径
```

### 存储定义 — `k8s/plugins-pv.yaml`

```yaml
# PV — 实际存储空间
apiVersion: v1
kind: PersistentVolume
metadata:
  name: thunder-plugins
spec:
  hostPath:
    path: /data/thunder/plugins    # ← 直接用宿主机目录

# PVC — 申请使用
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: thunder-plugins
spec:
  accessModes:
    - ReadOnlyMany                 # ← 多 Pod 可同时读
```

### NFS 服务器（备用）— `k8s/nfs-server.yaml`

```yaml
# 当前环境用 apt 装的 nfs-kernel-server，这个 yaml 是备用的容器化方案
volumes:
- hostPath:
    path: /data/thunder/plugins    # ← 和上面 PV 同一个宿主机目录
```

## 容易搞混的目录

| 目录 | 在哪 | 是什么 |
|------|------|--------|
| `deploy/admin-web/plugins/` | 源码仓库本地 | SO 暂存区，不挂 NFS |
| `/data/thunder/plugins/` | 宿主机 192.168.3.61 | NFS 目录，只存 Lua 落盘脚本 |
| `deploy/{Svc}/plugins/` | 各 Gateway 镜像内 | 运行时 SO 插件路径 |
