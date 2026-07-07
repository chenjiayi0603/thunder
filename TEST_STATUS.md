# Thunder 测试状态快照

> 这个文件是**活文档**，每次测试后由 `tests/save_status.sh` 更新。
> 不保留历史，只保留**当前状态**。

---

## 最近一次更新

| 字段 | 值 |
|------|-----|
| 日期 | 2026-07-06 |
| 分支 | `feat/etcd-watch-registry` |
| 操作人 | Claude |
| 环境 | **kubeadm v1.32.13** (containerd, flannel CNI) |
| 阶段 | K8s 环境验证：CNI 修复 + NFS SO 共享 + etcd Lua 热更新 + Worker 启动修复 |

---

## 构建

| 项 | 状态 | 备注 |
|----|:---:|------|
| `./deploy.sh build` | ✅ 0 error | 分支 feat/etcd-watch-registry |
| protobuf 降级分支构建 | ✅ 0 error | protobuf 7.35 → 6.33.5，ProtoCodec 编解码验证通过 |

---

## C++ 单元测试（ctest）

| 项 | 状态 | 备注 |
|----|:---:|------|
| 上次运行 | 2026-06-21 |
| 结果 | ✅ **356/356**（100%） | 0 failed |
| 命令 | `ctest --test-dir build/code/test -j$(nproc)` | |

---

## Python 单元测试（pytest unit）

| 项 | 状态 | 备注 |
|----|:---:|------|
| 上次运行 | 2026-06-21 |
| 结果 | ✅ **141/141** | 0 skipped |
| 命令 | `python3 -m pytest tests/unit/ -q` | |

---

## E2E 集成测试（Docker）

| 项 | 状态 | 备注 |
|----|:---:|------|
| 上次运行 | 2026-06-21 | 分支 `feat/etcd-watch-registry` |
| 结果 | ✅ **38 passed, 2 skipped** | 和上次基准一致 |
| 命令 | `./deploy.sh test e2e` | |

---

## 分支专项状态

### `chore/protobuf-6.33-downgrade`（#107 A~E 全完成）

| 子项 | 状态 | 备注 |
|------|:---:|------|
| 构建 0 error | ✅ | EtcdGrpcConnector + Manager 工厂 |
| ctest 355/355 | ✅ | Phase E 后；旧 EtcdParse/EtcdHttpConn 21 tests 已移除 |
| Python unit | ✅ | |
| B1 注册+KeepAlive | ✅ | gRPC 专属线程，SlotTxn，ev_async 事件桥 |
| B2 路由发现 | ✅ | ls 初始快照 + Watch 流，NodeNotice 组装 |
| B3 配置下发 | ✅ | config Watch + PutConfig 命令队列 |
| Manager 工厂接入 | ✅ | `connector=etcd-grpc` → EtcdGrpcConnector |
| deploy config 切换 | ✅ | 全部节点配置已改为 `etcd-grpc`，3 端点 |
| Docker lib | ✅ | libetcd-cpp-api-core.so 在 code/3party/lib |
| Phase C：3 节点 Raft | ✅ | docker-compose 3 节点 + YAML anchor |
| Phase E：旧代码清理 | ✅ | EtcdCenterConnector/HttpConn/Watcher/Parse 全删 |
| E2E 全通过 | ✅ | 30 passed，2 skipped（单节点 etcd 查询限制） |

---

## 专项测试（2026-06-25）

| 专项 | 结果 | 耗时 | 备注 |
|------|:----:|------|------|
| etcd 稳定性 S1~S5（test_etcd_stability.py） | ✅ **4/4** | 3m14s | |
| etcd 稳定性 S4/S6 slow（test_etcd_stability.py） | ✅ **2/2** | 5m49s | S4 kill-9 TTL 兜底 + S6 长跑 5min keepalive 无丢失 |
| etcd Watch 专项（test_etcd_watch.py） | ✅ **3/3** | 1m30s | PUT/DELETE/断流重建 全通 |
| Lua 下发（test_lua_module.py） | ✅ **9/9** | 45s | 含 SendToNodeType 全模式 |

---

## K8s 环境测试（2026-07-06）

### 集群状态

| 项 | 状态 | 备注 |
|----|:---:|------|
| kubeadm v1.32.13 | ✅ | control-plane Ready, containerd://2.2.2 |
| flannel CNI | ✅ | v0.28.5, Pod CIDR 10.244.0.0/16 |
| CoreDNS | ✅ | 2/2 Running |
| thuner 业务 Pod | ✅ | **10/10 Running** (admin-web, etcd×3, hello, interface, logic, mysql, redis) |
| 系统 Pod | ✅ | **8/8 Running** |

### CNI 修复

| 问题 | 原因 | 修复 |
|------|------|------|
| flannel Init:CrashLoopBackOff | initContainer `install-cni` command 为 `/opt/bin/install-conf`（不存在） | 用正确代理 (`127.0.0.1:7897`) 下载官方 YAML，command 改为 `cp` |
| CoreDNS CrashLoopBackOff | CNI 未就绪，Pod 网络不通 | flannel 就绪后自动恢复 |

### etcd StatefulSet 修复

| 问题 | 修复 |
|------|------|
| etcd-1/etcd-2 Pending (PVC unbound) | 创建 `pv-thunder-etcd-1` + `pv-thunder-etcd-2` (hostPath 1Gi RWO) |

### Worker 空载 CPU 优化（2026-07-06）

| 问题 | 原因 | 修复 | 效果 |
|------|------|------|------|
| Worker 空载 500m CPU | `WorkStealingPool` 线程池空闲时 `std::this_thread::yield()` 忙等自旋 | `yield()` → `condition_variable::wait_for()` 阻塞等通知，`commit()` 时 `cv.notify_one()` 唤醒 | Logic: 500m→3m, Hello: 500m→5m |

**定位过程**：`perf record -p PID` 火焰图 → 99% CPU 在 `__sched_yield` → 追溯 `work_stealing_pool.h:294` → 线程池空闲 worker 循环调用 `yield()` 空转。

**设计不变**：work-stealing、四级队列（local → submit → steal → global_q）、`commit()` 三级分发逻辑完全不动。只是空闲路径从轮询改为事件通知。

### Hello Worker 启动修复

| 问题 | 原因 | 修复 |
|------|------|------|
| Worker 未启动 | CoreDNS 不通时 `apt-get install libjemalloc2 libluajit-5.1-2` 失败 | DNS 恢复后手动 apt install + 重启 |

### NFS SO 共享验证

#### 真 NFS 协议（2026-07-06）

| 步骤 | 结果 |
|------|------|
| NFS Server | `nfs-kernel-server` 宿主机，export `/data/thunder/plugins` |
| PV | `pv-thunder-plugins-nfs`, `nfs: {server:192.168.3.61, path:/data/thunder/plugins}` |
| mount 类型确认 | `nfs4` (TCP, vers=4.2, 非 hostPath) ✅ |
| PUT `/plugins/HelloHttp/nfs_real.so` → admin-web | ✅ `{"ok":true}` |
| **四端 md5 交叉验证** | |

| 位置 | md5 |
|------|-----|
| ① PUT 源（本地 `/tmp/nfs_real.so`） | `99f7a2a960252f2c183b9bd390c03431` |
| ② admin-web Pod（NFS 挂载） | `99f7a2a960252f2c183b9bd390c03431` ✅ |
| ③ hello Pod（NFS 挂载） | `99f7a2a960252f2c183b9bd390c03431` ✅ |
| ④ 宿主机直接读（NFS Server 本地） | `99f7a2a960252f2c183b9bd390c03431` ✅ |

**写入链路**：`curl PUT :30090` → kube-proxy → admin-web Pod → NFS mount → TCP/2049 → 宿主机 NFS Server → 磁盘

**多节点兼容**：NFS 是网络协议，hello Pod 调度到任意节点 mount 同一 `192.168.3.61:/data/thunder/plugins`，行为一致。

### Lua 脚本热更新验证（etcd）

| 步骤 | 结果 |
|------|------|
| 修改前 `/hello/lua_echo` 响应 | `{"msg":"E2E_LOG_1783148505"}` |
| 写 etcd `/thunder/config/module/HELLO_HTTP` (version 99) | ✅ revision 41 |
| Worker 自动重载 (CheckShareMem mirror v1→v2) | ✅ 无需重启 |
| 修改后 `/hello/lua_echo` 响应 | `{"msg":"RELOAD_ROUND2_TEST"}` |
| Worker 重启后状态保持 | ✅ 从 etcd 恢复最新配置 |

**链路**：`curl POST /api/lua-scripts` → admin-web → etcd v3 kv/put → Worker etcd-grpc watch → 共享内存 mirror → Lua 模块热更新

### 测试环境要点

| 项 | 说明 |
|----|------|
| 代理 | Clash Verge `127.0.0.1:7897`（非 7890）；kubectl/curl 需显式设 `https_proxy` |
| 构建 | 无需重建，使用宿主机 `build_tsan/` 已编译 SO |
| NFS Server | 宿主机 `127.0.0.1`，PV `pv-thunder-plugins` (10Gi RWX) |

---

## 待测 / 阻塞

| 项 | 阻塞原因 | 关联 |
|----|---------|------|
| Phase D：Admin 脚本更新 | ✅ 完成 | #107 Phase D |
| TSan 验证 | ✅ 完成 | 37 races 全在 gRPC/abseil 内部，Thunder 源码 0 race |
| #119 Step 4：删除 vendored 文件 | 🟡 待做 | ep_grpc+ep_etcd from-source ✅，待干净机器跑 thirdparty_deploy 后删 code/3party/include/etcd/ 和 lib/libetcd-cpp-api-core.so |
| EtcdGrpcConnector gRPC subchannel failover | 🟡 待做 | 当前只取 `etcd_endpoints` 第一个；gRPC channel 内置 subchannel 故障切换未验证 |
| Lua 热更新：script_content vs Manager sync 冲突 | ⚠️ 已知限制 | Manager 注册后 sync 覆盖 etcd 中的 script_content；文件路径方式正常工作（#125 路径对齐已修复） |
| SO 热更新：admin-web 部署路径 | ✅ 已修复 | #125：upload_base → deploy/，rel 路径对齐（2026-06-25） |
| SO 热更新：etcd 版本通知 | ✅ 已实现 | `_notify_etcd_so_update`: PUT 后自动 etcd version++ → Manager ConfigUpdated → GracefulRestartWorker (2026-07-06) |
| SO 热更新：原地覆盖安全性 | ✅ 已验证 | RTLD_NODELETE + mmap + drain: 新 .so 覆盖旧 .so 文件，老 Worker 不受影响 (2026-07-06) |

---

## 快捷命令

```bash
# 构建
./deploy.sh build

# 单元测试（快，约 10s）
cd build && ctest -j4 --output-on-failure

# Python 单元测试（极快，约 0.1s）
python3 -m pytest tests/unit/ -q

# E2E（慢，约 3-5min）
./deploy.sh test e2e

# 保存测试结果到本文件
tests/save_status.sh
```

### K8s 环境验证

```bash
# 集群状态
kubectl get nodes -o wide
kubectl get pods -A

# 确认 NFS 协议（非 hostPath）
kubectl exec -n thunder deploy/thunder-hello -- mount | grep plugins
# → 192.168.3.61:/data/thunder/plugins/HelloHttp ... type nfs4

# NFS 共享四端验证
TOKEN="NFS_$(date +%s)"
echo "$TOKEN" > /tmp/nfs_test.so
LOCAL=$(md5sum /tmp/nfs_test.so | awk '{print $1}')
curl -s -X PUT "http://$(hostname -I | awk '{print $1}'):30090/plugins/HelloHttp/nfs_test.so" \
  --data-binary @/tmp/nfs_test.so
ADMIN=$(kubectl exec -n thunder deploy/thunder-admin-web -- md5sum /data/thunder/plugins/HelloHttp/nfs_test.so | awk '{print $1}')
HELLO=$(kubectl exec -n thunder deploy/thunder-hello -- md5sum /thunder/deploy/HelloHttp/plugins/nfs_test.so | awk '{print $1}')
HOST=$(md5sum /data/thunder/plugins/HelloHttp/nfs_test.so | awk '{print $1}')
echo "local:$LOCAL admin:$ADMIN hello:$HELLO host:$HOST"
# → 四个 md5 一致 = NFS 共享正常

# 代理: Clash Verge 端口 7897
export https_proxy=http://127.0.0.1:7897 http_proxy=http://127.0.0.1:7897
```
