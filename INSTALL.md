# Thunder 构建与安装（精简）

更全的 CMake 选项见 **`cmake/BUILD.md`**；第三方构建细节见 **`code/3party/readme.md`**。

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
# deploy/Hello/plugins/ModuleHello.so 
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

## 脚本（联调 / 压测，可选）

需已 **`cmake --install`** 到 **`deploy/`**。在 **`deploy/`** 下执行；更多参数见各脚本文件头注释。

**`test_helloserver.sh`** — 启动 Hello，并做一次极短 HTTP 冒烟。

```bash
cd deploy
./tests/test_helloserver.sh
```

**`test_interfaceserver.sh`** — 按顺序拉起 Center / Logic / Interface，并做 Interface 联调冒烟。

```bash
cd deploy
./tests/test_interfaceserver.sh
```

**`test_helloserver_wrk.sh`** — wrk 压测 Hello（须先已启动 Hello）。样例输出见 **`deploy/tests/wrk_test_result.md`**。

```bash
cd deploy
./tests/test_helloserver_wrk.sh
```

---

## 部署与验证

```bash
( cd deploy && ./nodes.sh restart all )
```

启停见 **`deploy/deploy.md`**。端口检查：`lsof -Pni4 | grep LISTEN`。示例：`curl "http://127.0.0.1:27008/Interface/gentoken"`（按本机配置改 IP/端口）。
