# Thunder 构建与安装（精简）

更全的 CMake 选项见 **`cmake/BUILD.md`**；第三方构建细节见 **`code/3party/readme.md`**。协程基类从 `CoroutineState` 迁到 **`StepCo20`** 的说明见 **`docs/StepCo20-coroutine-migration.md`**。

---

## 一键（仓库根执行）

首次请先装 **OpenSSL 开发包**；需能完整编译 **`code/3party/protobuf`**。

```bash
git submodule update --init --recursive \
  && cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  && cmake --build build --target thirdparty_deploy -j1 \
  && cmake --build build -j1 \
  && cmake --install build
```

上式第 4 步构建的是默认目标 **all**，已包含 **Proto**（按需调用 **`code/3party/protobuf/build/protoc`** 生成 **`coor.pb`**），**一般不必**再单独执行 `cmake --build build --target Proto`。若只改了 **`code/Proto/coor.proto`**，可单独：`cmake --build build --target thunder_proto_gen -j1` 或 `--target Proto`。

默认 **`-j1`**，减轻磁盘与 IO 压力；若本机 IO 足够可改为 **`-j$(nproc)`** 等加速。

仅重编主工程、第三方已部署过时，在已有 **`build/`** 下：

```bash
cmake --build build -j1 && cmake --install build
```

---

## 一键等价的分步命令（可逐段复制）

```bash
# 拉取 code/3party 等子模块（log4cplus 含嵌套 threadpool，须 --recursive）
git submodule update --init --recursive
```

```bash
# 在 build/ 生成工程；RelWithDebInfo = 接近 Release 优化 + 调试符号，便于 gdb
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

```bash
# 编译第三方并部署到 code/3party/lib、deploy/3lib；protoc 在 code/3party/protobuf/build
cmake --build build --target thirdparty_deploy -j1
```

```bash
# 编译主工程（Net、各节点、插件等）
cmake --build build -j1
```

```bash
# 安装到 deploy/（默认前缀即为 deploy/）
cmake --install build
```

```bash
# 仅重编并安装 Interface / Hello 插件（改 code/Interface 或 code/Hello 后，不必整工程重编）
# deploy/Interface/plugins/ModuleInterface.so
cmake --build build --target InterfacePlugins -j1
# deploy/HelloHttp/plugins/ModuleHello.so
cmake --build build --target HelloPlugins -j1 
```

---

## 第三方库版本（简约）

版本以各子模块 **当前 `HEAD`** 为准（**`.gitmodules`**），勿随意换成上游未验证的「最新版」。

```bash
git submodule status code/3party
```

```bash
cd code/3party/protobuf && git describe --tags --always
```

- **Protobuf**：只用 **`code/3party/protobuf/build`** 里的 protoc / libprotobuf / absl，勿与系统旧版混用。  
- **OpenSSL**：非常规路径配置 **`-DOPENSSL_ROOT_DIR=...`**。  
- **jemalloc**：非子模块，系统包或自行放入 **`deploy/3lib`**。  

示例快照（随子模块变化）：protobuf `v33-dev-…`、curl `curl-8_19_0-…`、mariadb `v3.4.8-…`、cryptopp `CRYPTOPP_8_9_0-…` 等。

---

## 常用单独目标（可选）

只编第三方、不拷贝到 lib/3lib：

```bash
cmake --build build --target thunder_3party_all -j1
```

只由 **`code/Proto/coor.proto`** 生成 **`code/Proto/src/*.pb.{cc,h}`**：

```bash
cmake --build build --target thunder_proto_gen -j1
```

生成并编译 **libProto.so**：

```bash
cmake --build build --target Proto -j1
```

只编某个节点或库（示例）：

```bash
cmake --build build --target Net -j1
```

```bash
cmake --build build --target Hello -j1
```

```bash
cmake --build build --target HelloPlugins -j1
```

```bash
cmake --build build --target InterfacePlugins -j1
```

更多 target 见 **`deploy/deploy.md`**。

---

## 测试

测试脚本统一在 **`tests/`** 目录，提供一键入口。

### 一键测试

```bash
# 全部测试 (单元 → E2E)
./tests/run_all.sh

# 构建
./deploy.sh build

# 单元测试 (C++ gtest + Python, ~45s, 零外部依赖)
./deploy.sh test unit

# E2E 集成测试 (需 Docker)
./deploy.sh test e2e

# 全部测试 (unit + e2e)
./deploy.sh test

# 快速模式 (仅 unit)
./deploy.sh test unit

### 单元测试 (纯 Python, 零外部依赖)

```bash
cd tests && python3 -m pytest unit/ -v
# 5 模块 64 用例, ~14 秒完成
```

### E2E 集成测试 (需 Docker 栈)

```bash
cd tests && python3 -m pytest e2e/ -v -s -m "integration or smoke" --mode=local
```

### 性能基准测试 (需 wrk)

```bash
./tests/benchmark/run_quick_bench.sh
```

详细性能数据见 `docs/performance_benchmark_2026-05-13.md`。

### 性能调优 (压测前必读)

> 测试机硬件: i9-12900H (6P+8E, 20逻辑核, 最大5.0GHz), 30GB DDR4, NVMe SSD, Ubuntu 26.04 LTS

#### 1. CPU governor

默认 `powersave` 会将空闲核心降到 400MHz，严重影响单线程事件驱动吞吐（Thunder 实测 −9.7%, Nginx 仅 −1.1%，因 Thunder protobuf/JSON 路径对 CPU 频率更敏感）。

```bash
# 检查当前模式
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

# 设为 performance（需 root）
echo "performance" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# 验证频率已提升
grep MHz /proc/cpuinfo | sort -t: -k2 -n | tail -5
```

> **注意**: i9-12900H 等混合架构 CPU 有 P-core (性能核) 和 E-core (能效核)。Worker 是单线程，若被调度到 E-core 或低频 P-core，吞吐会大幅下降。建议同时用 `taskset` 绑核。

#### 2. I/O Backend 选择

`conf/Hello.json` → `io_backend`:

| 值 | 说明 | 适用场景 |
|----|------|---------|
| `"ev"` | libev/epoll **(默认)** | **推荐**，HTTP echo 场景最优 (216k)，高/低频均稳定 |
| `"native_uring"` | io_uring | 大文件传输/批量异步 I/O 场景，kernel≥5.10 |
| `"asio_uring"` | asio+io_uring | 需编译选项 `-DTHUNDER_IO_ASIO_URING=ON` |
| `"dpdk"` | DPDK | 需编译选项 `-DTHUNDER_IO_DPDK=ON` |

```json
// 推荐:
{ "io_backend": "ev" }

// 大文件传输/批量 I/O:
{ "io_backend": "native_uring" }
```

> 实测 ev (epoll) 在小消息 HTTP echo 场景下优于 native_uring (216k vs 183k)，io_uring 的 SQE 构造开销 > 节省的 syscall 次数。详见 `docs/reports/thunder_vs_nginx_benchmark_20260526.md` §1.1。

#### 5. 对比测试注意事项

与 Nginx/其他服务对比时必须**同一 CPU governor**：

```
performance 公平对比 (5/28, P-core 绑核):
  Thunder Fast Path (ev)  216.0k  (109%)  🏆 超越 Nginx
  Nginx  POST              198.2k  (100%)
  Thunder 完整路径 (ev)     132.5k  (67%)
  
powersave 公平对比:
  Nginx  POST              192.9k  (100%)
  Thunder Fast Path        184.8k  (96%)
  Thunder 完整路径         132.1k  (68%)
```

> **对比测试必须同 governor + 同核心**。若一方 performance 一方 powersave，或一方 P-core 一方 E-core，偏差可达 15-20%。

#### 3. 日志级别

压测时必须用 `INFO` 或更高级别，`TRACE` 会导致 70% 性能下降（实测 64k vs 216k）。

```json
{ "log_level": "INFO" }
```

#### 4. 进程绑核 (i9-12900H 混合架构必须)

> i9-12900H: 6 P-core (max 5.0GHz) + 8 E-core (max 3.8GHz)。Worker 默认可能调度到 E-core，吞吐损失 ~17%。

**核心拓扑**:

```
P-core (12 逻辑核, 4.9-5.0GHz): cpu0-11  (cpu4-7 最高频 5.0GHz)
E-core (8 逻辑核, 3.8GHz):     cpu12-19
```

**绑核命令**:

```bash
# 查看 Worker 当前跑在哪个核
ps -eo pid,psr,comm | grep Hello_robot_W0

# 绑到 P-core 4-9 (3个物理 P-core, 含最高频 5.0GHz)
taskset -cp 4-9 $(pgrep Hello_robot_W0)

# 验证
taskset -cp $(pgrep Hello_robot_W0)
# → current affinity list: 4-9

# 启动时绑核 (推荐)
taskset -c 4-9 ./bin/HelloHttp conf/Hello.json

# wrk 绑到其他 P-core 避免竞争
taskset -c 0-3 wrk -t4 -c100 ...
```

---

## 部署与验证

```bash
( cd deploy && ./nodes.sh restart all )
```

启停见 **`deploy/deploy.md`**。端口检查：`ss -tlnp | grep 127.0.0.1`。示例：`curl -s -X POST "http://127.0.0.1:27008/Interface/gentoken" -H "Content-Type: application/json" -d '{"option":"GenKey"}'`。

### 配置文件 IP 占位符机制 (#39)

所有配置文件使用 `0.0.0.0` 占位符，运行时由启动脚本替换为实际 IP：

```json
// deploy/*/conf/*.json — 源文件使用占位符
{
  "access_host": "0.0.0.0",
  "inner_host": "0.0.0.0"
}
```

**docker-compose** — 启动命令自动检测主机 IP 并注入：
```bash
MY_IP=$(hostname -I | cut -d' ' -f1)
sed "s|0.0.0.0|$MY_IP|g" conf/*.json > /tmp/conf/*.json
THUNDER_CONF_DIR=/tmp/conf ./node.sh start
```

**k8s** — 使用 Pod IP 环境变量：
```bash
sed -i "s|0.0.0.0|$POD_IP|g" /tmp/conf/*.json
```

所有 `node.sh` 支持 `THUNDER_CONF_DIR` 环境变量覆盖配置目录，无需修改源文件。

### etcd 多端点故障转移 (#40)

配置支持逗号分隔多 etcd 节点，故障时自动轮转：

```json
// 单节点 (docker-compose)
"etcd_endpoints": "http://127.0.0.1:2379"

// 多节点 (生产集群, 自动故障转移)
"etcd_endpoints": "http://etcd-0:2379,etcd-1:2379,etcd-2:2379"
```

| 场景 | k8s | docker-compose | 裸机多 etcd |
|------|-----|---------------|------------|
| 配置 | `thunder-etcd.thunder:2379` | `127.0.0.1:2379` | `host1:2379,host2:2379` |
| 高可用 | ClusterIP 负载均衡 | 单节点 | 逗号分隔 + 自动轮转 |

故障检测: 3s keepalive 心跳 → 连续 5 次失败 (~15s) → `TryNextEndpoint()` 切换到下一端点。

### etcd Admin 管理界面 (#41, #42, #43)

纯静态 HTML 页面，浏览器直接打开，直连 etcd：

```
deploy/Interface/confweb/index.html  (13KB)
     │
     │ AJAX fetch()
     ▼
etcd:2379  ← 直接调 etcd v3 REST API
```

**功能**:
- 🖥 **节点** tab — 注册节点列表 (type/IP:Port/Node ID/Worker)
- ⚙ **配置** tab — etcd 配置读取、modal 编辑、新增
- 📋 **版本历史** — 每次修改自动保存旧值，支持回滚
- 📊 **状态** tab — etcd 健康检查 + 10s 自动刷新

**访问方式**:

```bash
# 本地 — 浏览器直接打开文件
firefox deploy/Interface/confweb/index.html

# 内网 — 启动 HTTP 文件服务器
cd deploy/Interface/confweb
python3 -m http.server 8080 --bind 0.0.0.0 &
# 内网其他机器访问: http://<本机IP>:8080/index.html?etcd=<本机IP>:2379

# 内网访问需 etcd 改为监听 0.0.0.0:
# docker-compose.yml → --listen-client-urls=http://0.0.0.0:2379

# k8s — port-forward
kubectl port-forward -n thunder svc/thunder-etcd 2379:2379
```

### k8s 部署

详见 **`k8s/README.md`** — 含架构拓扑、服务发现、配置注入、NodePort 网络设计。

```bash
# 快速部署
kubectl apply -f k8s/namespace.yaml
kubectl apply -f k8s/etcd-local.yaml
kubectl apply -f k8s/redis.yaml && kubectl apply -f k8s/mysql.yaml
kubectl apply -f k8s/logic-deployment.yaml
kubectl apply -f k8s/interface-deployment.yaml
kubectl apply -f k8s/hello-deployment.yaml
```
