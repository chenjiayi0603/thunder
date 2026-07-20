## 🚨 铁律：用户说 K8s = 标准 Kubernetes，不许私自替换

**用户说"K8s"，就是标准 Kubernetes（kubeadm/GKE/AKS/ACK）。禁止用以下方式替代：**

| 禁止 | 原因 |
|------|------|
| ❌ K3s 替代 K8s | 不同发行版，NFS/PVC 行为差异 |
| ❌ kind 替代 K8s | Docker-in-Docker，不能跑 NFS，是测试工具不是 K8s |
| ❌ 私自卸载集群 | 用户的 K3s 运行正常，未授权不得卸载 |
| ❌ 说"换机器就好" | 必须在这台机器上证明可行性 |
| ❌ 用 Docker Compose 结果替代 K8s | Docker Compose 和 K8s 是不同环境，Docker Compose 通过 ≠ K8s 通过 |
| ❌ 用"代码路径一样"当借口 | 运维能力和代码能力是两个维度，都要验证 |
| ❌ K8s 上出问题就退到 Docker Compose | **K8s 上出的问题必须在 K8s 上修**，不许换环境绕路 |

**如果当前环境没有标准 K8s：**
1. 明确告诉用户："当前没有标准 K8s 集群，需要安装 kubeadm 或接入云集群"
2. 询问用户是否安装
3. 得到确认后再操作
4. 绝不私自用 K3s/kind 顶替

---

## k8s regression — K8s 回归测试流程

### 🚨 铁律：回归测试发现的非本次更新引入的问题也要修复

**`./deploy.sh test k8s` 跑出的任何 FAIL，无论是否与本次改动相关，都必须修复。**

| 禁止 | 正确做法 |
|------|---------|
| ❌ "这是预存问题，不管" | 排查根因 → 修复 → 回归通过 |
| ❌ "只修 #154 范围的问题" | 全量 36/36 必须通过 |
| ❌ 手动 curl 测一下就说通过 | 必须 `regression-test.sh` 全绿 |
| ❌ 用旧 Pod 的旧镜像掩盖问题 | BUILD 阶段全新构建确保无灰生产 |
| ❌ 把失败归为"测试脚本 bug" | 修复测试脚本，不是忽略失败 |

**教训**：Interface Worker 崩溃是 7/18 `process_num` 类型不匹配 + Recreate 端口竞争导致——不是 #154 引入，但在回归中被发现，必须修。

## 🚨 铁律：回归测试必须用 `./deploy.sh test k8s`，严禁直接跑 `bash k8s/regression-test.sh`

| 禁止 | 原因 |
|------|------|
| ❌ `bash k8s/regression-test.sh` 单独跑 | 跳过 PRE-CHECK(残留没清)、BUILD(用旧镜像)、CLEAN(污染积累) |
| ❌ 改完 YAML 直接 `kubectl apply` 就当测试 | 没有重建镜像，测的是旧二进制 |
| ❌ `curl` 手动测一下就说通过 | 没有全量 36 项回归覆盖 |

**只用这个命令：**
```bash
./deploy.sh test k8s
```

### 标准化测试流程（#146）

```
┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐
│0.PRE-CHK │ → │ 1. BUILD │ → │ 2.DEPLOY │ → │ 3. TEST  │ → │ 4.CLEAN  │
│ 检查端口  │    │ C++/Go   │    │ 清 etcd  │    │regression│    │ 删残留   │
│ 清僵尸Pod │    │ 构建8镜像 │    │ 导入部署  │    │ -test.sh │    │ 删测试文件│
│ 清etcd残留│    └──────────┘    └──────────┘    └──────────┘    └──────────┘
└──────────┘
```

| 阶段 | 做什么 | 为什么必须 |
|------|--------|-----------|
| 0. PRE-CHK | 端口检查、僵尸 Pod/残留容器清理、资源余量、etcd 测试残留清理 | **进程卫生**：清理死进程释放端口/资源；只删已知测试脏 key，不删运营数据 |
| 1. BUILD | cmake C++ + Go admin-web + 8 个 Docker 镜像 | 确保测试的是最新代码 |
| 2. DEPLOY | 导入 containerd → 滚动更新 → 等 Ready | 所有 Pod 用新镜像 |
| 3. TEST | `bash k8s/regression-test.sh` 36 项 | 全量回归 |
| 4. CLEAN | 删 artifacts/NFS 测试文件/etcd `_regression_*` 条目/审计记录 | 不污染下次测试，但保留运营数据 |

### 验证标准

除 36 项全通过外，还需确认：
1. `docker image inspect` 的 Created 时间 = 刚构建的时间
2. Pod 内二进制 md5 ≠ 部署前
3. 清理后 etcd 不残留 `_regression_*` 条目 (registry/canary 等运营数据保留)
4. 清理后 NFS 不残留 `_regression_*` 文件

### 命令

```bash
# 唯一回归测试入口 — 5 阶段全流程 (#146)
./deploy.sh test k8s

# 日常部署 (不含测试)
./deploy.sh deploy && kubectl wait --for=condition=Ready pods --all -n thunder --timeout=120s
```

## 🚨 铁律：禁止重启绕过 Bug

**遇到任何服务异常，禁止用重启/重部署/缩容扩容作为修复手段。**

| 禁止 | 正确做法 |
|------|---------|
| ❌ `kubectl delete pod` 绕过问题 | 查日志 → 定位根因 → 修代码 |
| ❌ `kubectl scale 0→1` 绕过问题 | 同上 |
| ❌ `kubectl rollout restart` 绕过问题 | 同上 |
| ❌ "重启就好了" 作为结论 | 必须找到并修复根因 |

**CPU 高诊断流程**：
1. 火焰图：`perf record -F 99 -p <pid> -g -- sleep 30 && perf script | FlameGraph/stackcollapse-perf.pl | FlameGraph/flamegraph.pl > flame.svg`
2. strace：`strace -c -p <pid>` 看系统调用分布，定位忙等待
3. 不许用重启/缩容绕过 CPU 高的问题

### 根因修复（已固化，不需要每次测试前手动执行）

| 问题 | 永久修复 | 位置 |
|------|---------|------|
| br_netfilter 重启丢失 | `echo br_netfilter > /etc/modules-load.d/br_netfilter.conf` | `/etc/modules-load.d/` |
| DiskPressure taint | `evictionHard.nodefs.available: 5%` | `/var/lib/kubelet/config.yaml` |
| 镜像不在 containerd | `deploy.sh deploy` 自动 `docker save \| ctr import` | `deploy.sh` |
| worker 缺 lib | Dockerfile 预装 `libluajit-5.1-2`，去掉 TSan 编译 | `deploy/*/Dockerfile` |
| etcd endpoint 硬编码 | 配置改为 `thunder-etcd.thunder.svc.cluster.local:2379` | `deploy/*/conf/*.json` |

---

## 会话开头必读

每次新会话开始，**先跑环境预检**，确认服务在线再开始工作：

```bash
bash tests/check_env.sh
```

测试跑完后记录结果到本地：

```bash
tests/save_status.sh          # 跑完整测试 + 更新本地状态
tests/save_status.sh --quick  # 只跑构建+ctest+pytest（跳过 E2E）
```

> 测试结果保留在本地 `TEST_STATUS.md`（`.gitignore`，不提交）。

## 🚨 铁律：失败测试必须修到全绿

**任何测试（Smoke/E2E/Unit）如果有一项红色 ❌，必须停下来修好，不得带着失败往前进。**

原因：
- 红色项可能是本次改动引入的回归，不能留到后面
- 本次没碰的模块如果红了（如 HTTPS/WS 服务没起），也属于环境问题，修好再继续
- 只有全绿状态下跑新功能测试，结果才有意义

修复流程：
1. 定位失败原因（服务没起？配置错误？代码 bug？）
2. 修复根因，不是靠重启/重试蒙混
3. 重跑测试直到全绿
4. 再继续后续工作

**🚫 禁止的行为：**
- 测试红了 → 重跑一遍 → 绿了 → "修好了"（其实是 flaky，没找根因）
- 服务挂了 → 重启 → 不查为什么挂
- 把失败归咎于"暂时性问题"而不验证修复后的稳定性

---

## 测试术语约定

| 说法 | 对应脚本 | 说明 |
|------|---------|------|
| **冒烟测试** | `tests/test_smoke.sh` | 核心链路快速验证，需 Docker 集群在线 |
| **端到端测试** | `tests/e2e/`（`./deploy.sh test e2e`）| pytest 全链路，Docker Compose 自动起停 |
| **单元测试** | `tests/unit/`（ctest + pytest tests/unit/）| 零外部依赖 |
| **回归测试** | 三项全部：smoke + e2e + unit | 三项均通过才算回归通过 |

E2E 通过 ≠ 冒烟通过，覆盖范围不同，必须分开跑、分开确认。

### 测试环境唯一标准：Docker Compose

**E2E 测试固定使用 Docker Compose，禁止在测试代码里写死 k8s 地址（如 192.168.x.x:3xxxx）。**

理由：
- 端口统一来源于 `tests/ports.env`，Docker Compose 和测试代码都从这里读
- k8s NodePort 地址机器相关，写死后别的环境跑不起来

**🚨 环境生命周期铁律：**

| 操作 | 命令 | 说明 |
|------|------|------|
| 测试前 | `down` → `up -d` | 清残留（死 fd、残留 Step、Manager kill 记录），从零启动 |
| 测试后 | `down` | 不留残留给下次 |
| ❌ 禁用 | `restart` | 不清容器状态，残留堆积导致 CPU 高、no fd 洪水 |

原因：容器长生命周期会堆积残留状态——Step 注册不过期、死 fd 不释放、Manager 反复 kill 记录——导致每次测试都出现"no fd 8"日志洪水、CPU 虚假 100% 等问题。
- k8s 回归（`k8sregression`）是部署验证，不是日常功能测试，两者独立

测试入口：
```bash
./deploy.sh test unit   # 单元测试（C++ gtest + Python pytest unit/）
./deploy.sh test e2e    # E2E（Docker Compose 自动起停）
tests/test_smoke.sh     # 冒烟（需 Docker 集群已在线）
```

---

## deploytest — Thunder 本地部署测试

当用户说"deploytest"或"本地部署测试"时：

**唯一入口：`./deploy.sh`**

### 第一步：构建 + 单元测试
```bash
./deploy.sh test unit    # C++ gtest (~250 cases) + Python pytest (64 cases)，全部通过才继续
```

### 第二步：E2E 集成测试（Docker Compose）
```bash
./deploy.sh test e2e     # Docker compose up → 等待服务就绪 → pytest E2E → docker compose down
```
等价于手动流程：`./deploy.sh build` → `./deploy.sh up` → 等待端口 → `./deploy.sh test e2e --skip-build` → `./deploy.sh down`

> **deploytest 全程基于 Docker Compose**，不依赖 k8s，所有服务在容器内运行。

### E2E 覆盖范围

| 服务 | 测试内容 |
|------|---------|
| **Center (Raft)** | 节点选举、日志复制、服务注册/发现、集群状态查询、配置同步 |
| **HelloHttp** | GET/POST/PUT/DELETE 各方法、参数校验、错误处理 |
| **HelloHttps** | TLS 握手、证书校验、API 端点、异常断连 |
| **HelloWs** | WebSocket 连接→消息收发→断开、异常重连 |
| **Interface** | API 端点、参数校验、错误处理、插件加载→卸载 |
| **跨服务** | Manager-Worker 通信、心跳机制、全链路交互 |
| **Worker 优雅重启** | SIGTERM → 排空在途连接 → Manager 自动重启 Worker → 新 Worker 正常服务；验证：新 Worker 进程存在 + curl 正常 + 无 FATAL 日志 |
| **性能** | QPS、延迟 P99、内存占用 (真实 I/O) |

### 未覆盖项（待补充 E2E）

| 项目 | 当前状态 | 说明 |
|------|---------|------|
| **Lua SendToNodeType** | ✅ 已覆盖 | 9/9 E2E 通过（2026-06-25）含 fire-forget/async/async_target 全模式；#113 端口冲突已修 |
| **SO 热更新** | ✅ 已覆盖 | build-so → admin API list/extract/deploy → 文件系统部署 全链路；ReloadModule/版本变更触发 Worker 重载（2026-06-25） |
| **Lua 热更新** | ✅ 已覆盖 | etcd 版本触发 → Manager Watch → `CMD_REQ_RELOAD_LUA` → Worker `ReloadScript()`（不动 VM/SO/进程）；`lua_echo` 从 `"ok"` → `"V14_VM_ONLY"` 无停机（2026-06-29） |
| **etcd 节点注册完整性** | ✅ 已覆盖 | S1 稳定性测试验证，5 node_type 全部注册 + lease 有效（2026-06-25） |
| **多端点 failover** | ✅ 已覆盖 | chaos_etcd 17/17 通过：etcd1 停止→etcd2/3 续命→恢复；全集群重启；数据清空从零重建（2026-06-25） |

### 第三步：Smoke 测试

```bash
# 先跑环境预检（替代原来手动的 admin.py nodes）
bash tests/check_env.sh
# 任何一项红 → 停止，定位根因并修复，不得靠重启蒙混

# 预检全绿后再跑 smoke
tests/test_smoke.sh 2>&1 | tee /tmp/smoke_$(date +%Y%m%d_%H%M%S).txt
# 预期：0 失败；有任何失败 = 未通过
```

### 测试后清理
```bash
./deploy.sh clean        # 一键清理 build/ + Docker + tmp
```

### 规则
- 单元测试通过不算整体通过，E2E 必须也通过
- **E2E 通过不算 smoke 通过** — E2E 和 smoke 覆盖范围不同，必须分开跑、分开确认
- **smoke 有任何失败项 = 未通过** — 禁止只报总数（"15/18"），必须逐条列出失败项及原因
- **必须先确认所有预期服务节点已注册到 etcd，再开始 smoke 测试** — 若节点缺失，停止测试先排查注册问题，不得继续跑并把失败归咎于"超时"
- 失败则分析日志、修复、重试，最多 3 次
- 部分通过 = 未通过，要么全通要么明确列出未通过项及原因
- 模拟测试通过 ≠ 测试通过，硬件限制的标注"当前环境无法测试"及原因
- git add + commit + push 所有改动
---

## dockercomposeregression — Docker Compose 全量回归测试

触发词：`dockercomposeregression` / `docker compose 回归` / `全量回归`

### 测试前：清理环境

长生命周期的容器会积累残留状态（死 fd、残留 Step、Manager 反复 kill 记录），测试前必须 `down` 后 `up`，不能用 `restart`。

```bash
# ⚠️ 必须 down+up，不能用 restart — 后者不清容器，残留状态堆积
docker compose -p thunder-deploy -f docker/docker-compose.yml down
docker compose -p thunder-deploy -f docker/docker-compose.yml up -d
sleep 15  # 等所有服务就绪 + etcd 注册完成
```

### 执行流程

```bash
# 1. 清理 + 重建环境（down → up，非 restart）
docker compose -p thunder-deploy -f docker/docker-compose.yml down
docker compose -p thunder-deploy -f docker/docker-compose.yml up -d
sleep 15

# 2. 环境预检（任何一项红都必须修，不准靠重启蒙混）
bash tests/check_env.sh

# 3. 如果预检不通过 → 定位根因 → 修复 → 重新 down+up → 重新预检 → 通过后继续

# 4. 全量回归（单元 + Smoke + Lua 热重载 E2E）
python3 -m pytest tests/unit/ -q            # Python 单元测试
bash tests/test_smoke.sh                     # 冒烟测试
python3 tests/e2e/test_lua_hotreload_e2e_standalone.py  # Lua 热重载 E2E（admin API 推送 → 响应验证）
```

### 测试后：恢复环境

```bash
# 清理容器（不留残留状态给下次测试）
docker compose -p thunder-deploy -f docker/docker-compose.yml down
# 如需保留数据（mysql/redis 的 volume），加 --volumes=false
```

### 预检标准（check_env.sh）

| 检查项 | 通过条件 | 不通过时怎么做 |
|--------|---------|---------------|
| 端口 | 7 个端口全部 LISTEN | 查 docker compose ps，查服务日志 |
| etcd 注册 | 5 种 node_type 全部在线 | 查 etcd health，查 Manager 日志 |
| etcd 集群 | 3 节点全部 healthy | 查 etcd 容器日志 |
| Worker CPU | < 90% | >90% = busy loop，查 Worker 日志找 root cause |

### 🚫 禁止的行为

- 预检红了 → 重启 → 绿了 → 继续（没找根因）
- 某服务挂了 → `docker restart X` → 不管为什么挂
- "暂时性"失败 → 重跑通过 → 当修好了
- 把失败归咎于"暂时性问题"而不验证修复后的稳定性

### 根因分析流程

1. 查挂掉服务的日志：`docker compose logs <service> --tail 50`
2. 查 Worker/Manager 日志：`tail -50 deploy/<Svc>/log/Hello_robot_W0.log`
3. 检查 OOM、端口冲突、依赖未就绪
4. 定位到具体代码行或配置错误
5. 修复 → 重新预检 → 全量回归
6. 记录根因到 GitHub Issue

---

## k8sregression — K8s 部署 + 全量回归测试

> ⚠️ 这里的 k8s 指标准 Kubernetes（kubeadm/GKE/AKS），不是 K3s。
> 本地开发机装的是 K3s（`systemctl status k3s`），仅用于快速验证 YAML 语法和单 Pod 测试。
> 全量 K8s 回归测试需要在真实 kubeadm 集群上跑。

当用户说"k8sregression"或"k8s 回归测试"时：

### 第零步：确认环境

```bash
# 先确认是不是标准 K8s（不是 K3s）
kubectl get node -o wide
# 看 VERSION 列: v1.x.x+k3s1 = K3s（仅开发验证），v1.x.x = 标准 K8s（可跑回归）
```

### 第一步：构建 + 代码级测试

```bash
cd /home/tommychen/thunder/build
cmake --build . -j$(nproc)            # 全量构建，0 error
make install                           # 安装到 deploy 目录
ctest --test-dir code/test -j$(nproc)  # C++ gtest (331 cases)，99%+ 通过
python3 -m pytest tests/unit/ -q      # Python 单元测试 (133 cases)，全部通过
```

### 第二步：部署到 k8s

```bash
kubectl apply -f k8s/                 # 部署/更新所有服务
kubectl -n thunder rollout status deployment --timeout=120s  # 等待就绪
```

前置条件:
- k8s node 无 DiskPressure taint
- PV `thunder-plugins` 已就绪 (hostPath 或 NFS)
- NodePort: hello=30006, interface=30008, ws=30010, https=30043

### 第三步：端口转发 (NodePort 不可达时)

```bash
nohup python3 /tmp/k8s_fwd.py > /tmp/k8s_fwd.log 2>&1 &
```

### 第四步：配置测试并执行 E2E

```bash
cd /home/tommychen/thunder
# 指向 k8s NodePort
sed -i 's|127.0.0.1|192.168.3.61|g' tests/e2e/conftest.py
sed -i 's|https://127.0.0.1:27443|https://192.168.3.61:30043|' tests/e2e/test_https_hello.py
sed -i 's|27006|30006|g; s|27008|30008|g; s|27443|30043|g' tests/e2e/conftest.py
# 修复 sed 导致的 URL 断裂
sed -i 's|"http://192.168.3.61:|"http://|g' tests/e2e/conftest.py
sed -i '34s|"http://27008|"http://192.168.3.61:27008|' tests/e2e/conftest.py
sed -i '43s|"http://{p}|"http://192.168.3.61:{p}|g' tests/e2e/conftest.py

# 执行
python3 -m pytest tests/e2e/ -v --tb=line -m "integration or smoke" --mode=external

# 恢复
git checkout -- tests/e2e/conftest.py tests/e2e/test_https_hello.py
```

### E2E 覆盖范围

| 分组 | 用例数 | 预期 |
|------|:------:|------|
| HTTP hello | 4 | ✅ 全部通过 |
| HTTPS hello | 3 | ✅ SSL 证书正确时通过 |
| Interface chain | 5 | ✅ 4/5 (1 etcd 路由预存) |
| WS hello | 4 | ✅ 全部通过 |
| MultiCenter | 2 | ✅ 全部通过 |
| Stress | 1 | ✅ 通过 |
| WRK smoke | 2 | ✅ 通过 |
| etcd admin | 5 | ✅ 3 passed, 2 skipped (无注册数据+单节点) |
| **合计** | **21+** | **21/22 通过，1 预存失败** |

### 验收标准

- **构建**: 0 error
- **C++ gtest**: 99%+ (331 tests, 允许 SoDownload 预存失败)
- **Python unit**: 133/133 通过
- **E2E**: 20/21 通过 (允许 genkey_verifykey etcd 路由预存失败)
- 失败项标注原因 + 是否与本次改动相关

## 触发词：rearrange

当用户说 **rearrange** 时，执行以下流程：

### 适用场景
某个目录下有一堆内容重叠、未分类的 `.md` 文件，需要按功能重组。

### 核心原则
- **新文件 = 速查笔记风格**：精炼、结构化、方便面试前快速翻阅
- **有价值信息补回对应主题文件**：旧文件中的详细原理、完整示例、深入分析，不丢弃，直接补充到新文件对应章节中
- 宁可使单文件变大，也不丢失原理和例子

### 执行步骤

1. **读取所有文件**：读取目标目录下所有 `.md` 文件的内容（注意大文件分段读取）

2. **内容归类**：分析每份文件的主题和重叠点，设计功能分组方案

3. **去重合并 + 提取有价值信息**：
   - 同类内容合并，重复部分只保留最完整的一处
   - **同时将以下内容提取出来**，等新文件创建后补回：
     - 原理性长篇讲解（如"为什么这样设计"、"底层机制分析"）
     - 完整的可运行代码示例（非片段）
     - 对比分析（如 "A vs B 优缺点详解"）
     - 面试深挖中可能问到的扩展知识点
   - 新文件先只保留：核心结论 + 关键代码片段 + 对比表格 + 注意事项

4. **创建新文件**：
   - 创建 `00-总览.md` 作为索引总领文件（含文件地图、全景图、阅读路径）
   - 按功能创建 `01-*.md` 到 `N-*.md`，每份文件自成体系（核心原理 + 关键代码 + 注意事项）
   - 面试考点汇总到最后一篇

5. **将提取的有价值信息补回对应文件**：
   - 原理说明 → 补到对应主题文件的对应章节下
   - 完整示例 → 补到对应主题文件的代码示例区
   - 扩展知识点 → 补到对应文件的「深入理解」或「常见陷阱」章节
   - 确保新文件内容充实，不依赖外部文档

6. **旧文件清理**：确认新文件写完后，删除所有原始旧文件

7. **更新 CLAUDE.md 目录结构**：将新的目录结构反映到本文档的仓库目录结构中

### 文件命名规则

重组后的文件使用 `{序号}-{技术栈前缀}-{主题}.md` 格式：

```
02-go-并发编程.md    # go 技术栈
01-cpp-C++基础语法.md # cpp 技术栈
```

- **技术栈前缀**：当目录名称不能直接体现技术归属时（如 `go/` 目录下的文件在文件浏览器中可能脱离目录上下文），在序号后加技术栈前缀（如 `go`、`cpp`）
- **不需要前缀**：如果目录名本身就是技术名（如 `cpp/`），且文件在目录内引用无歧义，可省略前缀
- **一致性**：同一目录下所有文件保持统一的命名风格

### 要点列举必须带示例

列出多个技术要点时（如「六种逃逸场景」「五种实现方式」等），**每个要点必须附带独立代码示例**，不能用一行注释笼统带过。

❌ 反例（只有名词，无代码）：
```markdown
**六种逃逸场景**：返回指针、interface 调用、闭包、channel 发指针、大对象、切片扩容。
```

✅ 正例（逐条展开，每项有独立代码）：
```markdown
**六种逃逸场景**（含示例）：

```go
// 1. 返回指针
func escape1() *int {
    x := 42
    return &x  // x 逃逸到堆
}

// 2. interface 调用
func escape2() {
    x := 42
    fmt.Println(x)  // x 逃逸（fmt 参数为 interface{}）
}
// ... 其余逐条列出
```
```

**例外**：纯名词罗列（如文件列表、目录结构）不需要逐条代码。

### 禁止「其他」兜底分类

重构或增强文件时，**禁止**出现笼统的兜底章节（如 `### X.Y 其他重要特性` / `### X.Y 其他实用特性`），必须将杂项逐条拆解为**独立子节**（`#### X.Y.Z 具体名称`），每节包含：

| 要素 | 说明 |
|:----|:-----|
| **解决的问题** | 为什么需要这个特性/概念，解决了什么痛点 |
| **完整代码示例** | 含输入/输出/正反对比的可工作代码 |
| **性能/注意事项** | 零开销保证、常见陷阱、选型建议 |

❌ 反例（笼统堆砌）：
```markdown
### 1.5 其他重要特性（含示例）
```cpp
// nullptr —— 类型安全空指针
// enum class —— 强类型枚举
// constexpr —— 编译期计算
```
```

✅ 正例（逐条展开）：
```markdown
### 1.5 类型安全与枚举增强

#### 1.5.1 nullptr — 类型安全空指针

**解决的问题**：`NULL` 本质是整数 `0`，重载解析中会意外匹配 `int` 版本。

```cpp
void foo(int);  void foo(char*);
foo(NULL);      // 调用 foo(int) —— 危险！
foo(nullptr);   // 调用 foo(char*) —— 正确
```

**性能**：零开销抽象，运行时就是 `0`。

---
```

---

### 触发词：rearrange docs
- docs 目录编号前缀扁平化(如 `01-xxx.md`, `02-xxx.md`), 无子目录
- 参考模板: english-learner/docs/architecture/
### 触发词：logs / 日志
- 用 tests/logs.sh 查看, 支持 --logic/--interface/--etcd 指定节点
### 触发词：smoke / 冒烟
- tests/test_smoke.sh --hello/--interface/--etcd 分段测试
### 触发词：chaos / 混沌
- tests/chaos_etcd.sh 三个场景(停服/重启/灾难)

### 触发词：issus / 问题 / bug

- 所有 bug/优化/设计问题用 **GitHub Issue** 记录
- `gh issue create --title "xxx" --body "xxx" --label bug`
- 修复后 `gh issue close <id> --comment "已修复: commit <sha>"`
### 触发词：代码移动
- `git mv` 移动文件,同步修正所有 include 和 CMakeLists
- 全量构建 + 冒烟验证无回归
### 触发词：删代码
- 先确认零引用 → `grep -rn` 全局搜索 → 再删
- 测试文件如引用也一并清理

### 触发词：designdoc / 设计文档 / 写设计

用于撰写或重构架构/组件设计文档。以下规则来自 `docs/architecture/12-work-stealing-threadpool.md` 的多次迭代经验。

**铁律：对比 > 描述。简易聚在一起的对比表，远好于长篇细节描述。**

**文档骨架**（7 章固定结构）：

```
§1 设计背景    — 这个组件解决什么问题，在系统里的位置
§2 现存问题    — 当前的痛点（量化数据，不要只有定性描述）
§3 方案选型    — 业界方案 A/B 的诞生背景 + 对比表 + 本项目约束 → 选型结论
§4 核心设计    — 数据流全景 → 数据结构 → 关键流程 → 数据竞争分析
§5 性能实测    — 真实的 benchmark 数据，不写"预期提升 X%"
§6 兼容性与风险 — API 兼容 + 风险表
§7 参考资料    — 源码路径 + 论文
```

**每节规则**：

| 规则 | 说明 |
|------|------|
| **每节开头一句话点明** | "这节讲什么、为什么重要"，读者扫一眼就能决定要不要细读 |
| **对比表暴露差异** | 跟业界方案比、跟旧实现比、不同路径比——表格比文字直观十倍 |
| **伪代码做逻辑，不做装饰图** | 算法的 ASCII 图通常冗余，伪代码 + 行内注释足够说清楚 |
| **核心决策用 `>` blockquote 标注** | `> **核心决策**：xxxx`，读者扫 blockquote 就能抓到所有关键 trade-off |
| **数据流路径表必备** | 一张表列出所有路径 × 触发条件 × 设计意图，这是全文最有价值的一张表 |
| **性能数据必须落地** | 不写"预期降低 20~40%"，直接用 bench 实测数据（ns/op、加速比） |
| **先约束后方案** | 把"不能做什么"列清楚，选型理由自然成立 |
| **≤400 行，≤8 个代码块** | 超了就是冗余，裁剪到只保留核心 |

**反例**（不要这样写）：

```
❌ "Thunder 用了两组 deque 因为事件循环的 push 和 thief 的 steal_into 
    会竞争同一个 tail，而 Go 的每个 P 只写自己的 LRQ 所以没有这个问题……(500字)"
    
✅ 一张三行对比表：
    Go      | 每个 P 自己     | 自己的 LRQ           | N 个队列
    TBB     | 每个 worker     | 自己的 deque         | N 个队列
    Thunder | 一个事件循环    | 任意 worker 的 deque | 2N 个队列
   → 根因：生产者不是队列拥有者
```
- 没测就是没测, 别填假数据
- 对比测试要保证只有一个变量不同 (如 body 大小变化、其他条件一致)
- 每次改 backend 配置后等 5 秒让服务重启完成
- 结果直接写入对应文档, 别存脑子里

### 🚫 禁止回退
- **禁止 git reset/rebase 丢弃代码** — 除非用户明确要求
- **禁止 git checkout 覆盖修改** — 所有文件变动必须经过确认
- **禁止 rebase skip** — 冲突时合并解决, 不跳过有效提交
- **禁止 revert file moves/refactors** — 原因: 上次 rebase skip 导致 io/ register/ 目录丢失

### 提交规范 (Commit Rules)
- **只能 git merge，禁止 git rebase** — rebase 会改写历史, 丢弃本地提交
- **有冲突必须手动解决** — 不允许 --skip / --abort / --force
- **解决冲突后立即验证** — 全量编译 + 冒烟测试
- **每步提交前确认工作树干净** — git status 检查无遗漏


### testnewfunc 触发词 (Thunder)

当用户说"testnewfunc"时，执行以下流程：

**1. 定位改动范围**
```bash
git diff HEAD --stat          # 未提交更改
git log --oneline -3           # 最近提交
```
确定影响范围：code/Net | code/Hello* | deploy/admin-web | k8s | build

**2. 全量构建**
```bash
./deploy.sh build              # cmake + make + install, 必须 0 error 0 warning
```

**3. C++ 单元测试**
```bash
ctest -j4 --output-on-failure  # 从 build/code/test 目录运行
```
- 328 项必须 100% 通过
- 失败项逐一排查，不允许跳过

**4. Python 单元测试**
```bash
cd tests && python -m pytest pytest/ -v
```

**5. k8s 部署**（涉及 k8s 配置或部署文件时）
```bash
kubectl apply -f k8s/
kubectl -n thunder rollout restart deployment thunder-admin-web
```

**6. Admin 功能测试**（涉及 Admin 页面改动时）
- 页面可访问: `curl http://127.0.0.1:30090/index.html` → HTTP 200
- SO 镜像列表: `curl http://127.0.0.1:30090/api/so-images` → 返回 JSON
- SO 文件列表: `curl http://IP:8090/api/so-files?image=xxx` → 返回 .so 列表
- SO 提取: `curl -X POST http://IP:8090/api/so-extract ...` → 本地+NFS 双写验证
- 页面功能: grep 检查 selectSoImage / extractAndRefresh / triggerUpdate 等函数存在
- **必须真实请求，禁止 mock**

**7. SO 镜像构建**（涉及 so-images 或 deploy.sh 改动时）
```bash
./deploy.sh build-so all        # 全量构建
./deploy.sh build-so HelloHttp_ModuleHello  # 单独构建
```
- 首次构建 → 全量通过
- 二次构建 → 全部跳过(无变化)

**8. 回归测试（影响范围内的旧功能）**
- 分析改动影响范围，列出受影响的旧功能
- 跑受影响的相关测试
- 不跑全量回归（除非用户明确要求）

**9. 端到端测试（新增/修改的功能）**
- 针对本次改动的功能点，明确列出测试场景
- 实际跑通完整链路，展示运行输出
- 跑不通就说明具体卡在哪，不要跳过

**测试输出要求**:
- 每个测试项必须展示：命令 + 完整输出 + 结果
- 通过 ✅ / 失败 ❌ / 跳过 ⏭ 必须明确标注
- 部分通过 = 未通过，必须列出原因
- 构建失败、ctest 失败 = 阻塞，先修复再继续

**禁止的测试方式**:
- ❌ 只跑 ctest 就说"测试通过"
- ❌ curl 健康检查就说"功能正常"
- ❌ 改完代码不跑测试就提交
- ❌ 说"已验证"但不展示完整输出
- ❌ 部分通过就说"测试通过"
- ❌ E2E 通过就说"smoke 也通过"（两者覆盖范围不同，必须分别跑）
- ❌ smoke 有失败项却汇报"全部通过"或只报通过数不报失败数
- ❌ 节点未注册到 etcd 就开始跑 smoke，把路由超时当"预期失败"忽略

## Agent 行为准则

### 1. 先思考再编码（Think Before Coding）
- 不确定时必须停下来问，不能猜，不能假设
- 存在多种理解时列出选项让用户选，不要替用户做决定
- 发现更简单的方案时主动说出来，不要默默选最复杂的路
- 把 trade-off 摆出来，不要隐藏困惑

### 2. 简洁优先（Simplicity First）
- 50 行能写完绝不写 200 行
- 没人要求的"灵活性"和"可配置"不加
- 不可能发生的异常场景不做错误处理
- 不为未来可能的需求提前写代码

### 3. 精准修改（Surgical Changes）
- 只动被要求动的部分，不顺手优化相邻代码
- 匹配项目已有的代码风格，哪怕觉得自己写得更好
- 看到不相关的问题提一嘴就行，别动手改
- 每一行改动都能追溯到用户的原始请求

### 4. 部署规则：只能动目标组件，不能碰其他 Pod

**🚫 禁止：**
- `kubectl delete pod --all` — 会误删 etcd/mysql/redis，导致集群不可用
- `kubectl scale deploy --all --replicas=0` — 会导致所有网关 etcd 注册过期

**✅ admin-web 部署（唯一安全方式）：**
```bash
# 改完代码后
docker build --no-cache -t thunder-admin-web:latest .
docker save thunder-admin-web:latest -o /tmp/admin.tar
sudo ctr -n k8s.io images import /tmp/admin.tar
sudo ctr -n k8s.io images tag --force docker.io/library/thunder-admin-web:latest thunder-admin-web:latest
sudo ctr -n k8s.io images tag thunder-admin-web:latest docker.io/library/thunder-admin-web:latest
kubectl -n thunder rollout restart deployment/thunder-admin-web
```

**自测清单（改完代码后必跑）：**
```bash
# 1. 编译
go build -o admin-web .

# 2. API 回归（确认旧功能未破坏）
curl -s http://192.168.3.61:30090/api/overview | python3 -c "import sys,json;d=json.load(sys.stdin);assert d['ok']"
curl -s http://192.168.3.61:30090/api/nodes    | python3 -c "import sys,json;d=json.load(sys.stdin);assert d['ok']"

# 3. 新功能测试（如有）

# 4. 部署 + 回归全量
kubectl -n thunder rollout restart deployment/thunder-admin-web
sleep 5
bash k8s/regression-test.sh
```

### 5. 禁止升级第三方库（No Submodule Upgrades）
- **禁止 `git submodule update --remote`** — 会拉取第三方库最新版本，导致 submodule commit hash 变更
- **禁止 `git pull --recurse-submodules`** — 同样会意外升级子模块
- **禁止 IDE/编辑器自动拉取子模块** — 检查 VS Code/CLion 的 git 设置，关闭子模块自动更新
- 第三方库（`code/3party/` 下所有子模块）的版本必须保持锁定，除非用户明确要求升级
- 如需升级某个库：单独开 feat 分支 + 完整回归测试（C++ gtest + Python pytest + K8s 回归）→ 独立 PR

### 6. 禁止擅自提交（No Unauthorized Commits）
- **除非用户明确说"提交"、"commit"、"push"、"推"，否则绝不执行 git commit / git push**
- 改完代码 → 测试 → 汇报结果 → **停**，等用户指示
- 即使改了一堆文件、测试全绿，也不能自己决定提交
- 这条优先级高于其他所有行为准则

### 6. 目标驱动执行（Goal-Driven Execution）
- "修 Bug" → 先写能复现 Bug 的测试，再让测试通过
- "加校验" → 先写非法输入测试，再让它通过
- "重构 X" → 确保改前改后测试都通过
- 复杂任务先列分步计划，每步带验证方式

