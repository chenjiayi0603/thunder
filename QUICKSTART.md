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

> 确认 `./deploy.sh status` 里各节点端口已监听再执行。

### 1. HTTP Echo

```bash
curl -s http://127.0.0.1:27006/hello/hello -d '{"option":"Echo"}'
# 预期: {"code":0,"msg":"ok"}
```

### 2. 协程挂起/恢复验证

```bash
curl -s http://127.0.0.1:27006/hello/hello -d '{"option":"TestHelloPoolCpu"}'
# 预期: {"option":"TestHelloPoolCpu","checksum":786432}
```

### 3. Interface→Logic 全链路（POST）

```bash
curl -s http://127.0.0.1:27008/Interface/gentoken \
  -d '{"option":"GenKey"}'
# 预期: {"code":0,"token":"...","key":"...","msg":"success"}
```

### 4. Interface→Logic 全链路（GET）

```bash
curl -s -X GET http://127.0.0.1:27008/Interface/gentoken \
  -H "Content-Type: application/json" \
  -d '{"option":"GenKey"}'
# 预期: {"code":0,"token":"...","key":"...","msg":"success"}
```

### 5. Token 校验（非法 token 应返回业务错误）

```bash
curl -s http://127.0.0.1:27008/Interface/gentoken \
  -d '{"option":"VerifyKey","token":"bad","key":"bad"}'
# 预期: {"code":1}
```

### 6. WebSocket 握手

```bash
curl -si \
  -H "Upgrade: websocket" \
  -H "Connection: Upgrade" \
  -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" \
  -H "Sec-WebSocket-Version: 13" \
  http://127.0.0.1:27010/hello/shake --max-time 3 2>&1 | head -1
# 预期: HTTP/1.1 101 Switching Protocols
```

### 7. HTTPS Echo

```bash
curl -sk https://127.0.0.1:27443/hello/hello -d '{"option":"Echo"}'
# 预期: {"code":0,"msg":"ok"}
```

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
