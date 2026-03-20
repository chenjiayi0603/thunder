# Hello 插件：ModuleHello.so（与 ModuleHello/Makefile 对齐：链接 libNet + libProto）
if(NOT TARGET Net)
  message(FATAL_ERROR "Hello plugins require target 'Net' from code/Net")
endif()
if(NOT TARGET Proto)
  message(FATAL_ERROR "Hello plugins require target 'Proto' from code/Proto")
endif()
if(NOT TARGET Util)
  message(FATAL_ERROR "Hello plugins require target 'Util' from code/Util")
endif()

set(_mh_dir "${CMAKE_CURRENT_SOURCE_DIR}/src/ModuleHello")
file(GLOB MODULE_HELLO_SRCS CONFIGURE_DEPENDS "${_mh_dir}/*.cpp")

set(_hello_src "${CMAKE_CURRENT_SOURCE_DIR}/src")
add_library(ModuleHello SHARED
  ${MODULE_HELLO_SRCS}
  "${_hello_src}/CustomLogger.cpp"
  "${_hello_src}/HelloSession.cpp"
)

set_target_properties(ModuleHello PROPERTIES OUTPUT_NAME "ModuleHello" PREFIX "")

target_compile_features(ModuleHello PRIVATE cxx_std_20)
target_compile_definitions(ModuleHello PRIVATE _LINUX_OS_ NODE_BEAT=10.0)

target_include_directories(ModuleHello PRIVATE
  "${_mh_dir}"
  "${_hello_src}"
  "${THUNDER_PROTO}/src"
  "${THUNDER_PROTO}/include"
  "${THUNDER_NET}/include"
  "${THUNDER_NET}/src"
  "${THUNDER_UTIL}/src"
  "${THUNDER_UTIL}/src/Util"
  "${THUNDER_3PARTY}/protobuf/src"
  "${THUNDER_3PARTY}/protobuf/build/_deps/absl-src"
  "${THUNDER_3PARTY}/include"
  "${THUNDER_3PARTY}/include/mariadb"
)

find_library(_protobuf_lib protobuf PATHS "${THUNDER_3PARTY}/protobuf/build" NO_DEFAULT_PATH REQUIRED)
find_library(_utf8_validity utf8_validity PATHS "${THUNDER_3PARTY}/protobuf/build/third_party/utf8_range" NO_DEFAULT_PATH REQUIRED)

target_link_directories(ModuleHello PRIVATE
  "${THUNDER_3PARTY}/lib"
  "${THUNDER_3PARTY}/protobuf/build"
  "${THUNDER_3PARTY}/protobuf/build/third_party/utf8_range"
)

target_link_libraries(ModuleHello PRIVATE
  Net
  Proto
  Util
  hiredis_vip
  cryptopp
  ${_protobuf_lib}
  ${_utf8_validity}
  log4cplus
  jemalloc
  pthread
  dl
  rt
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
  target_link_directories(ModuleHello PRIVATE /usr/lib64)
endif()

if(THUNDER_DEPLOY_AUTO)
  include("${CMAKE_SOURCE_DIR}/cmake/ThunderDeploy.cmake")
  thunder_deploy_copy(ModuleHello "Hello/plugins/ModuleHello.so")
endif()
