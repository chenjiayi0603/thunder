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

# liburing: NativeUringIoBackend 编译/链接必备（仅 Linux kernel >= 5.1）
# 从子模块 code/3party/liburing 编译：CMake 配置阶段先跑 configure 生成头文件，再编译源码
if((THUNDER_IO_URING OR THUNDER_IO_ASIO_URING) AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_LIBURING_DIR ${CMAKE_SOURCE_DIR}/code/3party/liburing)
  if(EXISTS ${_LIBURING_DIR}/src/queue.c)
    # configure 生成 compat.h 和 io_uring_version.h（写入子模块 src/include/liburing/）
    if(NOT EXISTS ${_LIBURING_DIR}/src/include/liburing/compat.h)
      message(STATUS "liburing: 运行 configure 生成兼容头文件…")
      execute_process(
        COMMAND ${_LIBURING_DIR}/configure
        WORKING_DIRECTORY ${_LIBURING_DIR}
        RESULT_VARIABLE _liburing_cfg_ret
        OUTPUT_QUIET ERROR_QUIET
      )
      if(NOT _liburing_cfg_ret EQUAL 0)
        message(FATAL_ERROR "liburing configure 失败（返回 ${_liburing_cfg_ret}）")
      endif()
    endif()

    add_library(thunder_liburing STATIC
      ${_LIBURING_DIR}/src/queue.c
      ${_LIBURING_DIR}/src/register.c
      ${_LIBURING_DIR}/src/setup.c
      ${_LIBURING_DIR}/src/syscall.c
      ${_LIBURING_DIR}/src/version.c
    )
    target_include_directories(thunder_liburing PUBLIC ${_LIBURING_DIR}/src/include)
    target_compile_options(thunder_liburing PRIVATE -D_GNU_SOURCE)
    set(LIBURING_INCLUDE_DIRS ${_LIBURING_DIR}/src/include CACHE PATH "liburing include path" FORCE)
    set(LIBURING_LIBRARY thunder_liburing CACHE STRING "liburing target" FORCE)
    message(STATUS "liburing 2.14: building from submodule (code/3party/liburing)")
  else()
    message(WARNING "liburing submodule 未初始化 — 请执行: git submodule update --init code/3party/liburing")
    set(THUNDER_IO_URING OFF CACHE BOOL "" FORCE)
    set(THUNDER_IO_ASIO_URING OFF CACHE BOOL "" FORCE)
  endif()
elseif(THUNDER_IO_URING OR THUNDER_IO_ASIO_URING)
  message(STATUS "liburing: 非 Linux 平台，禁用 io_uring 后端")
  set(THUNDER_IO_URING OFF CACHE BOOL "" FORCE)
  set(THUNDER_IO_ASIO_URING OFF CACHE BOOL "" FORCE)
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
  add_subdirectory(code/HelloHttp)
  add_subdirectory(code/HelloHttps)
  add_subdirectory(code/HelloWs)
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


if(THUNDER_BUILD_HELLO_PLUGINS)
  if(TARGET ModuleHelloHttp)
    install(TARGETS ModuleHelloHttp LIBRARY DESTINATION HelloHttp/plugins)
  endif()
  if(TARGET ModuleHelloHttps)
    install(TARGETS ModuleHelloHttps LIBRARY DESTINATION HelloHttps/plugins)
  endif()
  if(TARGET CmdHelloWs)
    install(TARGETS CmdHelloWs LIBRARY DESTINATION HelloWs/plugins)
  endif()
  if(TARGET ModuleShakeWs)
    install(TARGETS ModuleShakeWs LIBRARY DESTINATION HelloWs/plugins)
  endif()
endif()

if(THUNDER_BUILD_NODE_PLUGINS)
  if(TARGET CmdGetToken)
    install(TARGETS CmdGetToken LIBRARY DESTINATION Logic/plugins)
  endif()
  if(TARGET ModuleInterface)
    install(TARGETS ModuleInterface LIBRARY DESTINATION Interface/plugins)
  endif()
endif()
