# Thunder CMake 构建

在**仓库根目录**（与 `CMakeLists.txt` 同级）执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
cmake --install build
```

默认 `CMAKE_INSTALL_PREFIX` 为 `deploy/`（与旧 `make install` 布局一致），且 `THUNDER_DEPLOY_AUTO=ON` 时编译成功后会再拷贝到 `deploy/`（见 `ThunderDeploy.cmake`）。

## 常用选项

| CMake 选项 | 含义 |
|------------|------|
| `-DTHUNDER_BUILD_CENTER=ON` | 生成 `Center` 可执行文件（默认开） |
| `-DTHUNDER_BUILD_HELLO_PLUGINS=ON` | 生成 `ModuleHello.so`（默认开） |
| `-DTHUNDER_BUILD_NODE_PLUGINS=ON` | 生成 Logic/Interface/Center 节点 `.so` 到 `deploy/plugins/...`（默认开） |
| `-DTHUNDER_DEPLOY_AUTO=OFF` | 关闭 POST_BUILD 自动复制到 `deploy/` |
| `-DTHUNDER_INCLUDE_3PARTY=OFF` | 不加载 `code/3party` 超级构建（无 `thirdparty_deploy` 等目标） |

## 协议生成目标（与 `thirdparty_deploy` 用法类似）

在已 `cmake -S . -B build` 的前提下：

```bash
# 只从 .proto 生成 / 更新 code/Proto/src/*.pb.{cc,h}（不链接 libProto.so）
cmake --build build --target thunder_proto_gen -j1

# 生成并编译 libProto.so（含 POST_BUILD 复制到 deploy 等）
cmake --build build --target Proto -j1
```

**说明**：`thunder_proto_gen` 与 `thirdparty_deploy` 一样，都是「显式命名的 aggregate target」，便于单独跑；全量 `cmake --build build` 编主工程时会编 `Proto`，仍会按需触发生成步骤。

## 常用命令对照

- **全量编译**：`cmake --build build -j"$(nproc)"`  
- **协议生成**：在 **`code/Proto/CMakeLists.txt`** 中由 `add_custom_command` 调用 `protoc` 生成 `coor.pb.{cc,h}`；改 **`code/Proto/coor.proto`** 后推荐 **`cmake --build build --target thunder_proto_gen`**（或 **`--target Proto`** 以生成并链接库）。需先有 **`code/3party/protobuf/build/protoc`**（先 `thirdparty_deploy` 或编 `ep_protobuf`）。  
- **单 target**：`cmake --build build --target Util|Proto|Net|Hello|Center|HelloPlugins|LogicPlugins|InterfacePlugins|CenterPlugins|...`（各节点全部插件可用聚合 target `*Plugins`，见各 `code/<节点>/CMakeLists.txt`）  
- **清理**：`cmake --build build --target clean`  
- **首次部署运行**（编译安装后）：`cmake --install build`，再在 `deploy/` 下按需 `./nodes.sh restart all`（见 `INSTALL.md`）  

若缺少 `code/3party/lib`，可手动：`ln -sfn ../../deploy/3lib "$(pwd)/code/3party/lib"`（在仓库根执行时注意路径），或先完成 **`thirdparty_deploy`**。

## 第三方库（`code/3party`，已由根 `CMakeLists.txt` 引入）

子模块见 **`.gitmodules`**（**不要**在命令里写字面量 `...`，须列出子模块路径或一次性拉全部）。先拉子模块再配置：

```bash
git submodule update --init --recursive
```

与主工程**同一** `build` 目录下编译并部署到 **`code/3party/lib`**、**`code/3party/include`**、**`deploy/3lib`**（覆盖同名文件）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel --target thirdparty_deploy
```

首次在**未编译** `code/3party/protobuf/build` 时，`cmake -S . -B build` 仍可**完成配置**；若尚未生成 `libprotobuf.so`，配置结束时会提示先执行 `cmake --build build --target ep_protobuf`（或整包 `thunder_3party_all` / `thirdparty_deploy`），再编主工程（`Util` / `Proto` / `Net` 等）。

- 中间安装前缀为 **`build/code/3party/stage/`**（不是单独 `build-3party`）。  
- **`thunder_3party_all`**：只编第三方，不复制。  
- 第三方目标默认 **不** 随 `cmake --build build` 的默认 `all` 一起编（`EXCLUDE_FROM_ALL`），需显式指定 target。

亦可仅在 `code/3party` 下单独配置（等价，生成目录为 `code/3party/build-3party/`）：见 **`code/3party/readme.md`**。历史逐步安装步骤见 **`code/3party/readme1.md`**。

## 集成测试（Docker）

原 **`Dockerfile.test`** + **`integration-test/`** 已移除（未接入 CI / 日常构建）。若需容器化联调，请自行补充 compose 与镜像定义。根目录已移除旧的 `Dockerfile` / `docker-compose.yml`、一键诊断脚本与 `QUICK_*` 文档；日常构建以本节 CMake 命令为准。
