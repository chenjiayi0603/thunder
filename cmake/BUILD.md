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
| `-DTHUNDER_DEPLOY_AUTO=OFF` | 关闭 POST_BUILD 自动复制到 `deploy/` |
| `-DTHUNDER_INCLUDE_3PARTY=OFF` | 不加载 `code/3party` 超级构建（无 `thirdparty_deploy` 等目标） |

## code/make.sh 封装

在 `code/` 下可使用 `./make.sh`（内部即上述 `cmake` 命令），例如：

- `./make.sh all` — 运行 `gen_proto` + 全量编译  
- `./make.sh Util` / `Proto` / `Net` / `plugin` — 单 target  
- `./make.sh clean` / `install` / `first` — 见 `make.sh` 头部注释  

多节点插件（Center/Logic/Interface 等子目录 Makefile）仍用 `code/plugins.sh`；Hello 的 `ModuleHello` 优先用 CMake。协议生成与编译：`cd code && ./make.sh Proto`。

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

仓库根目录保留 **`Dockerfile.test`**，供 `integration-test/docker-compose.test.yml` 构建测试镜像。根目录已移除旧的 `Dockerfile` / `docker-compose.yml`、一键诊断脚本与 `QUICK_*` 文档；日常构建以本节 CMake 命令为准。
