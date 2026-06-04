# Thunder 快速上手

---

## 一、构建

### 冷启动（首次 / 三方库未编译）

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target thirdparty_deploy -j1   # 编译三方库，约 10~20 分钟
cmake --build build -j1                               # 编译主工程
cmake --install build
```

> ⚠️ **必须 `-j1`**，多线程编译会因磁盘 IO 瓶颈卡死。

### 日常重编（三方库已就绪）

```bash
cmake --build build -j1 && cmake --install build
```

### 只重编某模块

```bash
cmake --build build --target Net -j1                 # 网络框架核心
cmake --build build --target InterfacePlugins -j1    # Interface 插件
cmake --build build --target HelloPlugins -j1        # Hello 插件
```

### Proto 变更后

```bash
cmake --build build --target thunder_proto_gen -j1
cmake --build build -j1
```

---

## 二、启动 / 停止

```bash
./deploy.sh up          # 启动 Docker 集群（etcd + MySQL + Redis + 各节点）
./deploy.sh status      # 查看容器状态 + 监听端口
./deploy.sh restart     # 重启所有容器
./deploy.sh down        # 停止并清理
```

等待约 15 秒，所有服务进入 healthy 状态后再测试。

---

## 三、冒烟测试

```bash
./tests/test_smoke.sh
```

覆盖 HTTP / HTTPS / WebSocket / Interface→Logic 全链路（POST + GET）/ etcd，9 项全绿即通过。

---

## 四、自动化测试

```bash
# 单元测试（C++ + Python，零外部依赖，~45s）
./deploy.sh test unit

# E2E 集成测试（需 Docker，~3min）
./deploy.sh test e2e

# 全部（unit + E2E）
./deploy.sh test

# 跳过构建直接跑测试
./deploy.sh test unit --skip-build
./deploy.sh test e2e  --skip-build
```

---

## 五、清理

```bash
./deploy.sh clean       # 清理 build/ + Docker + tmp
```

---

## 端口速查

| 服务 | 协议 | 端口 |
|------|------|------|
| HelloHttp | HTTP | 27006 |
| HelloHttps | HTTPS | 27443 |
| HelloWs | WebSocket | 27010 |
| Interface | HTTP | 27008 |
| Logic | 内部 S2S | 16068 |
| etcd | HTTP | 2379 |
| Redis | TCP | 6379 |
| MySQL | TCP | 3306 |
