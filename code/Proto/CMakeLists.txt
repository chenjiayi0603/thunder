# libProto.so（与 code/Proto/src/Makefile 一致）
file(GLOB PROTO_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc")

add_library(Proto SHARED ${PROTO_SRC})
set_target_properties(Proto PROPERTIES OUTPUT_NAME "Proto")

target_compile_features(Proto PRIVATE cxx_std_20)
target_compile_definitions(Proto PRIVATE _LINUX_OS_)

target_include_directories(Proto PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${THUNDER_NET}/src"
  "${THUNDER_3PARTY}/protobuf/src"
  "${THUNDER_3PARTY}/protobuf/build/_deps/absl-src"
  "${THUNDER_3PARTY}/include"
)

find_library(_protobuf_lib protobuf PATHS "${THUNDER_3PARTY}/protobuf/build" NO_DEFAULT_PATH REQUIRED)
find_library(_utf8_validity utf8_validity PATHS "${THUNDER_3PARTY}/protobuf/build/third_party/utf8_range" NO_DEFAULT_PATH REQUIRED)

target_link_libraries(Proto PRIVATE ${_protobuf_lib} ${_utf8_validity} pthread c dl rt)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
  target_link_directories(Proto PRIVATE /usr/lib64)
endif()

if(THUNDER_DEPLOY_AUTO)
  include("${CMAKE_SOURCE_DIR}/cmake/ThunderDeploy.cmake")
  thunder_deploy_copy(Proto "lib/libProto.so")
endif()
