目录说明：
库目录：
 网络库       Net
 框架协议     proto
 框架通用库     Util
 第三方库     3party（统一编译见 3party/readme.md、3party/CMakeLists.txt）
 
节点目录：
中心节点     Center
测试demo节点     Hello 
逻辑服务器       Logic 
登录			Interface

编译（CMake，在仓库根执行）：
  cmake -S . -B build && cmake --build build -j$(nproc) && cmake --install build
  协议变更时： cmake --build build --target Proto（生成逻辑在 code/Proto/CMakeLists.txt）
  说明见仓库根 INSTALL.md、cmake/BUILD.md
