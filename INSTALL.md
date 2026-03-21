# Thunder 构建与安装

详细选项见 **`cmake/BUILD.md`**。第三方子模块与 **`code/3party`** 见 **`code/3party/readme.md`**。

---

## 1. 准备

```bash
git submodule update --init --recursive
```

首次需能使用 **`code/3party/protobuf/build/protoc`**（见下节「第三方」）。

---

## 2. 配置（仓库根）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

可选环境变量 / 选项示例：`JOBS`、`THUNDER_CMAKE_ARGS`、`-DTHUNDER_BUILD_CENTER=OFF`、`-DTHUNDER_BUILD_NODE_PLUGINS=OFF`、`-DTHUNDER_DEPLOY_AUTO=OFF` 等，见 **`cmake/BUILD.md`**。

---

## 3. 第三方库（与主工程同一 `build/`）

```bash
cmake --build build --target thirdparty_deploy -j1
```

仅编译、不拷贝到 `deploy/3lib`：`cmake --build build --target thunder_3party_all -j1`。

---

## 4. 协议 `.proto` → C++（可选单独跑）

改 **`code/Proto/*.proto`** 后，先生成源码再编主工程：

```bash
# 只生成 code/Proto/src/*.pb.{cc,h}（不编 libProto.so）
cmake --build build --target thunder_proto_gen -j1

# 生成并编译 libProto.so
cmake --build build --target Proto -j1
```

兼容：`bash code/Proto/regen_cpp.sh`（等价于 `thunder_proto_gen`）。

---

## 5. 全量编译与安装

```bash
cmake --build build -j"$(nproc)"
cmake --install build
```

默认安装前缀为 **`deploy/`**；`THUNDER_DEPLOY_AUTO=ON`（默认）时还会在构建成功后把产物拷到 `deploy/`（见 `cmake/ThunderDeploy.cmake`）。

单 target 示例：`Util`、`Net`、`Proto`、`Hello`、`Center`、`ModuleHello`、Logic/Interface/Center 插件（如 `CmdGetToken`、`ModuleInterface`、`CmdElection` 等）。

---

## 6. 首次部署到运行目录并启动节点（可选）

产物已由 **`cmake --install build`**（及默认 **`THUNDER_DEPLOY_AUTO`**）安装到 **`deploy/`**。启动多节点示例：

```bash
( cd deploy && ./restart_nodes.sh all )
```

日常启停可用 **`deploy/`** 下 **`start_nodes.sh` / `stop_nodes.sh` / `restart_nodes.sh`**，配置见 **`deploy/server_list.conf`**、**`deploy/server_dir.conf`**。说明见 **`deploy/deploy.md`**。

> 已移除旧版 **`install.sh` / `install_*.sh`**（与当前 CMake 安装重复）；请仅用 CMake 构建与安装。

---

## 7. 运行与简单验证

- 监听端口：`lsof -Pni4 | grep LISTEN`
- 日志示例：`deploy/Center/log/`、`deploy/Interface/log/`
- 示例 HTTP（IP 按本机调整）：

```bash
curl "http://127.0.0.1:27008/Interface/gentoken"
```
