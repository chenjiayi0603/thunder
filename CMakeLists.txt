cmake_minimum_required(VERSION 3.20)
project(Thunder LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# 默认安装到 deploy/（与 Makefile install 一致）
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/deploy" CACHE PATH "Install prefix (bin/lib)" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(ThunderCommon)

option(THUNDER_BUILD_CENTER "Build Center 可执行文件（-DNODE_TYPE_CENTER，对应 makefile.center）" ON)
option(THUNDER_BUILD_HELLO_PLUGINS "Build Hello ModuleHello.so 插件" ON)
option(THUNDER_BUILD_NODE_PLUGINS "Build Logic/Interface/Center 节点 .so 插件（deploy/plugins/...）" ON)
option(THUNDER_DEPLOY_AUTO "编译成功后自动复制到 deploy/（见 cmake/ThunderDeploy.cmake）" ON)
# 第三方库超级构建（code/3party/CMakeLists.txt），默认仅注册目标，不随主工程 all 一起编；见 target thirdparty_deploy
option(THUNDER_INCLUDE_3PARTY "包含第三方库 CMake 目标（thunder_3party_all、thirdparty_deploy）" ON)

find_package(Threads REQUIRED)

if(THUNDER_INCLUDE_3PARTY)
  add_subdirectory(code/3party EXCLUDE_FROM_ALL)
endif()

add_subdirectory(code/Util)
add_subdirectory(code/Proto)
add_subdirectory(code/Net)

if(THUNDER_BUILD_HELLO_PLUGINS)
  add_subdirectory(code/Hello)
endif()

if(THUNDER_BUILD_NODE_PLUGINS)
  add_subdirectory(code/Logic)
  add_subdirectory(code/Interface)
  add_subdirectory(code/Center)
endif()

# cmake --install 与 POST_BUILD 复制布局一致（路径相对于安装前缀，默认 deploy/）
install(TARGETS Hello RUNTIME DESTINATION Hello/bin)
install(TARGETS Util Net Proto LIBRARY DESTINATION lib)

if(THUNDER_BUILD_CENTER AND TARGET Center)
  install(TARGETS Center RUNTIME DESTINATION bin)
endif()

if(THUNDER_BUILD_HELLO_PLUGINS AND TARGET ModuleHello)
  install(TARGETS ModuleHello LIBRARY DESTINATION Hello/plugins)
endif()

if(THUNDER_BUILD_NODE_PLUGINS)
  if(TARGET CmdGetToken)
    install(TARGETS CmdGetToken LIBRARY DESTINATION plugins/Logic)
  endif()
  if(TARGET ModuleInterface)
    install(TARGETS ModuleInterface LIBRARY DESTINATION plugins/Interface)
  endif()
  if(TARGET CmdElection)
    install(TARGETS
      CmdElection CmdNodeDisconnect CmdNodeRegister CmdNodeReport ModuleAdmin
      LIBRARY DESTINATION plugins/Center)
  endif()
endif()
