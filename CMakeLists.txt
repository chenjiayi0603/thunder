cmake_minimum_required(VERSION 3.20)
project(Thunder LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# 默认安装到 deploy/
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/deploy" CACHE PATH "Install prefix (bin/lib)" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(ThunderCommon)

option(THUNDER_BUILD_CENTER "Build Center 可执行文件（定义 NODE_TYPE_CENTER）" OFF)
option(THUNDER_BUILD_HELLO_PLUGINS "Build Hello ModuleHello.so 插件" ON)
option(THUNDER_BUILD_NODE_PLUGINS "Build Logic/Interface 节点 .so 插件（deploy/plugins/...）" ON)
option(THUNDER_DEPLOY_AUTO "编译成功后自动复制到 deploy/（见 cmake/ThunderDeploy.cmake）" ON)
# 第三方库超级构建（code/3party/CMakeLists.txt），默认仅注册目标，不随主工程 all 一起编；见 target thirdparty_deploy
option(THUNDER_INCLUDE_3PARTY "包含第三方库 CMake 目标（thunder_3party_all、thirdparty_deploy）" ON)
option(THUNDER_IO_ASIO_URING "启用 standalone Asio io_uring I/O 后端" OFF)
option(THUNDER_IO_URING "启用原生 io_uring I/O 后端 (NativeUringIoBackend)" ON)

if(THUNDER_IO_ASIO_URING)
  add_compile_definitions(THUNDER_IO_ASIO_URING ASIO_STANDALONE ASIO_HAS_IO_URING ASIO_DISABLE_EPOLL)
endif()

# liburing: NativeUringIoBackend 编译/链接必备
if(THUNDER_IO_URING OR THUNDER_IO_ASIO_URING)
  # 尝试 pkg-config
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBURING QUIET liburing)
  endif()
  if(LIBURING_FOUND)
    message(STATUS "liburing: pkg-config ${LIBURING_VERSION}")
    set(LIBURING_INCLUDE_DIRS "${LIBURING_INCLUDE_DIRS}" CACHE PATH "liburing include path")
    set(LIBURING_LIBRARY_DIRS "${LIBURING_LIBRARY_DIRS}" CACHE PATH "liburing library path")
  else()
    # 回退：找头文件和库
    find_path(LIBURING_INCLUDE_DIRS liburing.h
      HINTS "$ENV{HOME}/.local/include" "/usr/local/include"
      DOC "liburing include path")
    find_library(LIBURING_LIBRARY uring
      HINTS "$ENV{HOME}/.local/lib" "/usr/local/lib"
      DOC "liburing library")
    if(LIBURING_INCLUDE_DIRS)
      message(STATUS "liburing include: ${LIBURING_INCLUDE_DIRS}")
    else()
      message(WARNING "liburing.h 未找到 — NativeUringIoBackend 将编译失败。请安装 liburing-dev 或设置 LIBURING_INCLUDE_DIRS。")
    endif()
    if(LIBURING_LIBRARY)
      get_filename_component(LIBURING_LIBRARY_DIRS "${LIBURING_LIBRARY}" DIRECTORY)
      message(STATUS "liburing library: ${LIBURING_LIBRARY} (dir: ${LIBURING_LIBRARY_DIRS})")
    else()
      message(WARNING "liburing.so 未找到 — 链接将失败。请安装 liburing-dev 或设置 LIBURING_LIBRARY。")
    endif()
  endif()
endif()

find_package(Threads REQUIRED)

if(THUNDER_INCLUDE_3PARTY)
  add_subdirectory(code/3party EXCLUDE_FROM_ALL)
endif()

add_subdirectory(code/Util)
add_subdirectory(code/Proto)
add_subdirectory(code/Net)

option(THUNDER_BUILD_TESTS "Build unit tests" OFF)
if(THUNDER_BUILD_TESTS)
  add_subdirectory(code/test)
endif()

if(THUNDER_BUILD_HELLO_PLUGINS)
  add_subdirectory(code/Hello)
endif()

if(THUNDER_BUILD_NODE_PLUGINS)
  add_subdirectory(code/Logic)
  add_subdirectory(code/Interface)
endif()

# cmake --install 与 POST_BUILD 复制布局一致（路径相对于安装前缀，默认 deploy/）
install(TARGETS Hello RUNTIME DESTINATION HelloHttp/bin)
install(TARGETS Hello RUNTIME DESTINATION HelloWs/bin RENAME HelloWs)
install(TARGETS Hello RUNTIME DESTINATION HelloHttps/bin RENAME HelloHttps)
install(TARGETS Util Net Proto LIBRARY DESTINATION lib)


if(THUNDER_BUILD_HELLO_PLUGINS AND TARGET ModuleHello)
  install(TARGETS ModuleHello LIBRARY DESTINATION HelloHttp/plugins)
  install(TARGETS ModuleHello LIBRARY DESTINATION HelloHttps/plugins)
endif()

if(THUNDER_BUILD_NODE_PLUGINS)
  if(TARGET CmdGetToken)
    install(TARGETS CmdGetToken LIBRARY DESTINATION Logic/plugins)
  endif()
  if(TARGET ModuleInterface)
    install(TARGETS ModuleInterface LIBRARY DESTINATION Interface/plugins)
  endif()
endif()
