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

## code/make.sh 封装

在 `code/` 下可使用 `./make.sh`（内部即上述 `cmake` 命令），例如：

- `./make.sh all` — 运行 `gen_proto` + 全量编译  
- `./make.sh Util` / `Proto` / `Net` / `plugin` — 单 target  
- `./make.sh clean` / `install` / `first` — 见 `make.sh` 头部注释  

多节点插件（Center/Logic/Interface 等子目录 Makefile）仍用 `code/plugins.sh`；Hello 的 `ModuleHello` 优先用 CMake。协议生成与编译：`cd code && ./make.sh Proto`。

## 集成测试（Docker）

仓库根目录保留 **`Dockerfile.test`**，供 `integration-test/docker-compose.test.yml` 构建测试镜像。根目录已移除旧的 `Dockerfile` / `docker-compose.yml`、一键诊断脚本与 `QUICK_*` 文档；日常构建以本节 CMake 命令为准。
