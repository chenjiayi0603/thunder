# =============================================================================
# Thunder 第三方库统一构建（与 .gitmodules 子模块对应，参考 readme1.md）
#
# 推荐：在仓库根与主工程同一 build 目录配置（由根 CMakeLists.txt add_subdirectory 引入）：
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build build --parallel --target thirdparty_deploy
#
# 亦可单独在本目录配置（与上等价，二进制目录不同）：
#   cmake -S . -B build-3party && cmake --build build-3party --target thirdparty_deploy
#
# 产物：
#   - 各库安装到 <build>/code/3party/stage/（从仓库根配置时）或 build-3party/stage/
#   - thirdparty_deploy：复制 .so 等到 code/3party/lib 与 deploy/3lib（覆盖），
#     头文件合并到 code/3party/include；protobuf 仍使用源码树下 protobuf/build（主工程链接 absl）。
#
# 说明：jemalloc 未在 .gitmodules 中，请使用系统包或自行安装后放入 deploy/3lib。
# =============================================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20)
endif()
project(Thunder3Party C CXX)

include(ExternalProject)

find_package(OpenSSL REQUIRED)

get_filename_component(THUNDER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(THUNDER_3PARTY "${CMAKE_CURRENT_SOURCE_DIR}")
set(THUNDER_DEPLOY "${THUNDER_ROOT}/deploy")
set(EP_STAGE "${CMAKE_CURRENT_BINARY_DIR}/stage")
set(EP_BUILD_ROOT "${CMAKE_CURRENT_BINARY_DIR}/ep")

# 子工程（mariadb / curl / mongo）需要合理的 OpenSSL 前缀；非标准路径请显式传入：
#   cmake -DOPENSSL_ROOT_DIR=/path/to/openssl ...
if(NOT OPENSSL_ROOT_DIR)
  set(OPENSSL_ROOT_DIR "/usr" CACHE PATH "OpenSSL root for 3party child builds")
endif()

set(CMAKE_BUILD_TYPE "${CMAKE_BUILD_TYPE}" CACHE STRING "Build type" FORCE)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

message(STATUS "Thunder3Party: THUNDER_ROOT=${THUNDER_ROOT}")
message(STATUS "Thunder3Party: STAGE (install prefix)=${EP_STAGE}")
message(STATUS "Thunder3Party: OPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}")

# ExternalProject 子工程内单独执行 FindOpenSSL 时，在部分环境下会失败 -> HAVE_OPENSSL 未定义，
# 进而 ma_crypt.h 中 MA_HASH_CTX 等全部缺失。必须把父工程已找到的 OpenSSL 显式传入。
set(_THUNDER_EP_OPENSSL_ARGS "-DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}")
if(DEFINED OPENSSL_INCLUDE_DIR)
  list(APPEND _THUNDER_EP_OPENSSL_ARGS "-DOPENSSL_INCLUDE_DIR=${OPENSSL_INCLUDE_DIR}")
endif()
if(DEFINED OPENSSL_CRYPTO_LIBRARY)
  list(APPEND _THUNDER_EP_OPENSSL_ARGS "-DOPENSSL_CRYPTO_LIBRARY=${OPENSSL_CRYPTO_LIBRARY}")
endif()
if(DEFINED OPENSSL_SSL_LIBRARY)
  list(APPEND _THUNDER_EP_OPENSSL_ARGS "-DOPENSSL_SSL_LIBRARY=${OPENSSL_SSL_LIBRARY}")
endif()

file(MAKE_DIRECTORY "${EP_STAGE}/lib" "${EP_STAGE}/include")

macro(_check_sub path name)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "Missing ${name}: ${path}\n  Run: git submodule update --init (see .gitmodules)")
  endif()
endmacro()

_check_sub("${THUNDER_3PARTY}/c-ares/CMakeLists.txt" "c-ares")
_check_sub("${THUNDER_3PARTY}/libev/configure" "libev")
_check_sub("${THUNDER_3PARTY}/cryptopp/GNUmakefile" "cryptopp")
_check_sub("${THUNDER_3PARTY}/hiredis-vip/Makefile" "hiredis-vip")
_check_sub("${THUNDER_3PARTY}/mariadb-connector-c/CMakeLists.txt" "mariadb-connector-c")
_check_sub("${THUNDER_3PARTY}/mongo-c-driver/CMakeLists.txt" "mongo-c-driver")
_check_sub("${THUNDER_3PARTY}/log4cplus/CMakeLists.txt" "log4cplus")
_check_sub("${THUNDER_3PARTY}/etcd-cpp-apiv3/CMakeLists.txt" "etcd-cpp-apiv3")
if(NOT EXISTS "${THUNDER_3PARTY}/log4cplus/threadpool/ThreadPool.h")
  message(FATAL_ERROR
    "log4cplus 缺少嵌套子模块 threadpool（需 ThreadPool.h）。请在仓库根执行:\n"
    "  git submodule update --init --recursive code/3party/log4cplus\n"
    "或进入目录后:\n"
    "  cd code/3party/log4cplus && git submodule update --init threadpool catch")
endif()
_check_sub("${THUNDER_3PARTY}/curl/CMakeLists.txt" "curl")
_check_sub("${THUNDER_3PARTY}/protobuf/CMakeLists.txt" "protobuf")

# 通用 CMake 子工程参数
set(_ep_cmake
  "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
  "-DCMAKE_INSTALL_PREFIX:PATH=${EP_STAGE}"
  "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
  "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
)

# --- c-ares ---
ExternalProject_Add(ep_c_ares
  PREFIX "${EP_BUILD_ROOT}/c-ares"
  SOURCE_DIR "${THUNDER_3PARTY}/c-ares"
  BINARY_DIR "${EP_BUILD_ROOT}/c-ares/build"
  CMAKE_ARGS
    ${_ep_cmake}
    -DCARES_SHARED=ON
    -DCARES_STATIC=OFF
    -DCARES_BUILD_TESTS=OFF
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel 1
  INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
)

# --- libev（autotools）---
ExternalProject_Add(ep_libev
  PREFIX "${EP_BUILD_ROOT}/libev"
  SOURCE_DIR "${THUNDER_3PARTY}/libev"
  CONFIGURE_COMMAND
    ${CMAKE_COMMAND} -E env "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}"
    <SOURCE_DIR>/configure --prefix=${EP_STAGE}
  BUILD_COMMAND ${CMAKE_COMMAND} -E chdir <SOURCE_DIR> ${CMAKE_MAKE_PROGRAM} -j1
  INSTALL_COMMAND ${CMAKE_COMMAND} -E chdir <SOURCE_DIR> ${CMAKE_MAKE_PROGRAM} install
  BUILD_IN_SOURCE TRUE
)

# --- Crypto++（GNUmakefile dynamic）---
ExternalProject_Add(ep_cryptopp
  PREFIX "${EP_BUILD_ROOT}/cryptopp"
  SOURCE_DIR "${THUNDER_3PARTY}/cryptopp"
  CONFIGURE_COMMAND ${CMAKE_COMMAND} -E true
  # 必须给 CXXFLAGS 加引号，否则 -fPIC 会被 make 解析成 -f PIC（误把 PIC 当 makefile）
  BUILD_COMMAND
    bash -c "cd <SOURCE_DIR> && ${CMAKE_MAKE_PROGRAM} -f GNUmakefile dynamic \"CXXFLAGS=-Wno-narrowing -fPIC\" -j1"
  INSTALL_COMMAND
    bash -c "mkdir -p '${EP_STAGE}/lib' && shopt -s nullglob && cp -a <SOURCE_DIR>/libcryptopp.so* '${EP_STAGE}/lib/' 2>/dev/null || true"
  BUILD_IN_SOURCE TRUE
)

# --- hiredis-vip（Makefile）---
ExternalProject_Add(ep_hiredis_vip
  PREFIX "${EP_BUILD_ROOT}/hiredis-vip"
  SOURCE_DIR "${THUNDER_3PARTY}/hiredis-vip"
  CONFIGURE_COMMAND ${CMAKE_COMMAND} -E true
  BUILD_COMMAND ${CMAKE_COMMAND} -E env "CC=${CMAKE_C_COMPILER}" ${CMAKE_COMMAND} -E chdir <SOURCE_DIR> ${CMAKE_MAKE_PROGRAM} -j1 "CFLAGS=-Wno-implicit-function-declaration"
  INSTALL_COMMAND
    ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
    ${CMAKE_MAKE_PROGRAM} install "PREFIX=${EP_STAGE}"
  BUILD_IN_SOURCE TRUE
)

# --- MariaDB Connector/C ---
ExternalProject_Add(ep_mariadb
  PREFIX "${EP_BUILD_ROOT}/mariadb"
  SOURCE_DIR "${THUNDER_3PARTY}/mariadb-connector-c"
  BINARY_DIR "${EP_BUILD_ROOT}/mariadb/build"
  CMAKE_ARGS
    ${_ep_cmake}
    ${_THUNDER_EP_OPENSSL_ARGS}
    -DWITH_SSL=OPENSSL
    -DWITH_UNIT_TESTS=OFF
    # 关闭 -Werror，避免上游未使用函数等在 GCC 下导致整库编译失败
    -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel 1
  INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
)

# --- mongo-c-driver（libmongoc / libbson，输出名 mongoc2 / bson2 由版本号决定）---
ExternalProject_Add(ep_mongo_c_driver
  PREFIX "${EP_BUILD_ROOT}/mongo-c-driver"
  SOURCE_DIR "${THUNDER_3PARTY}/mongo-c-driver"
  BINARY_DIR "${EP_BUILD_ROOT}/mongo-c-driver/build"
  CMAKE_ARGS
    ${_ep_cmake}
    ${_THUNDER_EP_OPENSSL_ARGS}
    -DENABLE_MONGOC=ON
    -DENABLE_TESTS=OFF
    -DENABLE_EXAMPLES=OFF
    -DENABLE_MAN_PAGES=OFF
    -DENABLE_HTML_DOCS=OFF
    -DENABLE_SSL=OPENSSL
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel 1
  INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
)

# --- log4cplus ---
ExternalProject_Add(ep_log4cplus
  PREFIX "${EP_BUILD_ROOT}/log4cplus"
  SOURCE_DIR "${THUNDER_3PARTY}/log4cplus"
  BINARY_DIR "${EP_BUILD_ROOT}/log4cplus/build"
  CMAKE_ARGS
    ${_ep_cmake}
    -DBUILD_SHARED_LIBS=ON
    -DLOG4CPLUS_BUILD_TESTING=OFF
    -DLOG4CPLUS_BUILD_LOGGINGSERVER=OFF
    -DWITH_UNIT_TESTS=OFF
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel 1
  INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
)

# --- curl（依赖已安装到 STAGE 的 c-ares）---
ExternalProject_Add(ep_curl
  DEPENDS ep_c_ares
  PREFIX "${EP_BUILD_ROOT}/curl"
  SOURCE_DIR "${THUNDER_3PARTY}/curl"
  BINARY_DIR "${EP_BUILD_ROOT}/curl/build"
  CMAKE_ARGS
    ${_ep_cmake}
    ${_THUNDER_EP_OPENSSL_ARGS}
    "-DCMAKE_PREFIX_PATH=${EP_STAGE}"
    -DBUILD_SHARED_LIBS=ON
    -DBUILD_CURL_EXE=ON
    -DCURL_USE_OPENSSL=ON
    -DCURL_USE_LIBSSH2=OFF
    # 新版 curl 默认 CURL_USE_LIBPSL=ON 且 find_package(REQUIRED)，未装 libpsl-dev 会配置失败
    -DCURL_USE_LIBPSL=OFF
    # 避免依赖系统 libidn2-dev（无则仅关闭 IDN，Thunder 一般不需要）
    -DUSE_LIBIDN2=OFF
    -DENABLE_CURL_MANUAL=OFF
    -DBUILD_LIBCURL_DOCS=OFF
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel 1
  INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
)

# --- protobuf（构建目录固定在源码树 protobuf/build，供主工程 ThunderCommon.cmake 查找 absl）---
ExternalProject_Add(ep_protobuf
  PREFIX "${EP_BUILD_ROOT}/protobuf"
  SOURCE_DIR "${THUNDER_3PARTY}/protobuf"
  BINARY_DIR "${THUNDER_3PARTY}/protobuf/build"
  CMAKE_ARGS
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DCMAKE_INSTALL_PREFIX=${EP_STAGE}"
    -Dprotobuf_BUILD_TESTS=OFF
    -Dprotobuf_BUILD_CONFORMANCE=OFF
    -Dprotobuf_BUILD_EXAMPLES=OFF
    -Dprotobuf_BUILD_SHARED_LIBS=ON
    -Dprotobuf_FORCE_FETCH_DEPENDENCIES=ON
    "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
    "-DFetchContent_SOURCE_DIR_absl=${THUNDER_3PARTY}/protobuf/build/_deps/absl-src"
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel 1
  INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
)

# --- gRPC（静态库，供 etcd-cpp-apiv3 静态链入 libetcd-cpp-api-core.so）---
# 不作为子模块（仓库过大）；GIT_SHALLOW=ON 在构建时下载指定 tag。
# 依赖顺序：ep_c_ares、ep_protobuf 先安装到 EP_STAGE，gRPC 通过
#   -DgRPC_PROTOBUF_PROVIDER=package / -DgRPC_CARES_PROVIDER=package 复用它们。
# absl 随 protobuf install 一起进入 EP_STAGE，gRPC 也通过 CMAKE_PREFIX_PATH 找到它。
ExternalProject_Add(ep_grpc
  DEPENDS ep_c_ares ep_protobuf
  GIT_REPOSITORY https://github.com/grpc/grpc.git
  GIT_TAG        v1.66.5
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
  PREFIX         "${EP_BUILD_ROOT}/grpc"
  BINARY_DIR     "${EP_BUILD_ROOT}/grpc/build"
  CMAKE_ARGS
    ${_ep_cmake}
    ${_THUNDER_EP_OPENSSL_ARGS}
    "-DCMAKE_PREFIX_PATH=${EP_STAGE}"
    -DBUILD_SHARED_LIBS=OFF
    -DgRPC_PROTOBUF_PROVIDER=package
    -DgRPC_ABSL_PROVIDER=package
    -DgRPC_CARES_PROVIDER=package
    -DgRPC_SSL_PROVIDER=package
    -DgRPC_ZLIB_PROVIDER=package
    -DgRPC_RE2_PROVIDER=module
    -DgRPC_BUILD_TESTS=OFF
    -DgRPC_BUILD_GRPC_CPP_PLUGIN=ON
    -DgRPC_BUILD_GRPC_CSHARP_PLUGIN=OFF
    -DgRPC_BUILD_GRPC_NODE_PLUGIN=OFF
    -DgRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN=OFF
    -DgRPC_BUILD_GRPC_PHP_PLUGIN=OFF
    -DgRPC_BUILD_GRPC_PYTHON_PLUGIN=OFF
    -DgRPC_BUILD_GRPC_RUBY_PLUGIN=OFF
  BUILD_COMMAND   ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel 1
  INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
)

# --- etcd-cpp-apiv3 v0.15.4（子模块，仅 core：SyncClient + Watcher）---
# BUILD_ETCD_CORE_ONLY=ON → 不需要 cpprestsdk（异步 HTTP 客户端），仅编译 gRPC 传输层。
# gRPC 静态链入 libetcd-cpp-api-core.so；protobuf 仍为动态链接（libprotobuf.so.33.x）。
ExternalProject_Add(ep_etcd_cpp_apiv3
  DEPENDS ep_protobuf ep_grpc
  PREFIX     "${EP_BUILD_ROOT}/etcd-cpp-apiv3"
  SOURCE_DIR "${THUNDER_3PARTY}/etcd-cpp-apiv3"
  BINARY_DIR "${EP_BUILD_ROOT}/etcd-cpp-apiv3/build"
  CMAKE_ARGS
    ${_ep_cmake}
    "-DCMAKE_PREFIX_PATH=${EP_STAGE}"
    "-DProtobuf_ROOT=${EP_STAGE}"
    -DBUILD_ETCD_CORE_ONLY=ON
    -DBUILD_ETCD_TESTS=OFF
    -DBUILD_SHARED_LIBS=ON
    -DETCD_W_STRICT=OFF
  BUILD_COMMAND   ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel 1
  INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
)

add_custom_target(thunder_3party_all DEPENDS
  ep_c_ares
  ep_libev
  ep_cryptopp
  ep_hiredis_vip
  ep_mariadb
  ep_mongo_c_driver
  ep_log4cplus
  ep_curl
  ep_protobuf
  ep_grpc
  ep_etcd_cpp_apiv3
)

# 使用 bash 显式执行，避免依赖 chmod +x（WSL/Windows 挂载盘或部分 FS 上 chmod 会失败导致整 target 报错）
add_custom_target(thirdparty_deploy
  COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/deploy_thirdparty.sh"
          "${THUNDER_ROOT}"
          "${EP_STAGE}"
          "${THUNDER_3PARTY}"
          "${THUNDER_DEPLOY}/3lib"
          "${THUNDER_3PARTY}/protobuf/build"
  DEPENDS thunder_3party_all
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMENT "Deploy third-party libs to 3party/lib, include, deploy/3lib"
  VERBATIM
)

message(STATUS "Targets: thunder_3party_all (build all), thirdparty_deploy (copy to lib/include/deploy/3lib)")
