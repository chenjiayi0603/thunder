# libProto.so：.proto 由 CMake 在构建前生成（当前仅 coor.proto：Raft 等）
set(_proto_root "${CMAKE_CURRENT_SOURCE_DIR}")
set(_proto_src "${_proto_root}/src")
set(_protoc "${THUNDER_3PARTY}/protobuf/build/protoc")
set(_proto_ld_path
  "${THUNDER_3PARTY}/protobuf/build:${THUNDER_3PARTY}/protobuf/build/third_party/utf8_range"
)

set(_proto_names coor)
set(_PROTO_INPUTS "")
foreach(_n IN LISTS _proto_names)
  list(APPEND _PROTO_INPUTS "${_proto_root}/${_n}.proto")
endforeach()

set(_PROTO_GEN_CC "")
foreach(_n IN LISTS _proto_names)
  list(APPEND _PROTO_GEN_CC "${_proto_src}/${_n}.pb.cc")
endforeach()
set(_PROTO_GEN_H "")
foreach(_n IN LISTS _proto_names)
  list(APPEND _PROTO_GEN_H "${_proto_src}/${_n}.pb.h")
endforeach()
set(_PROTO_GEN_ALL ${_PROTO_GEN_CC} ${_PROTO_GEN_H})

# LD_LIBRARY_PATH：与旧 gen_proto/regen 脚本一致，便于动态链接的 protoc
if(DEFINED ENV{LD_LIBRARY_PATH} AND NOT "$ENV{LD_LIBRARY_PATH}" STREQUAL "")
  set(_proto_env_ld "${_proto_ld_path}:$ENV{LD_LIBRARY_PATH}")
else()
  set(_proto_env_ld "${_proto_ld_path}")
endif()

add_custom_command(
  OUTPUT ${_PROTO_GEN_ALL}
  COMMAND ${CMAKE_COMMAND} -E env "LD_LIBRARY_PATH=${_proto_env_ld}"
          "${_protoc}"
          "-I${_proto_root}"
          "--cpp_out=${_proto_src}"
          ${_PROTO_INPUTS}
  WORKING_DIRECTORY ${_proto_root}
  DEPENDS
    ${_PROTO_INPUTS}
  COMMENT "Generate protobuf C++ from coor.proto (protoc)"
  VERBATIM
)

# 显式目标：与 thirdparty_deploy 类似，可单独执行「只生成 .pb 源、不链接 libProto.so」
#   cmake --build build --target thunder_proto_gen -j1
add_custom_target(thunder_proto_gen
  DEPENDS ${_PROTO_GEN_ALL}
  COMMENT "Regenerate code/Proto/src/*.pb.{cc,h} from .proto"
)

add_library(Proto SHARED ${_PROTO_GEN_CC})
set_target_properties(Proto PROPERTIES OUTPUT_NAME "Proto")

set_source_files_properties(${_PROTO_GEN_CC} PROPERTIES GENERATED TRUE)

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

thunder_resolve_protobuf_shared_libs(_protobuf_lib _utf8_validity)

target_link_libraries(Proto PRIVATE ${_protobuf_lib} ${_utf8_validity} pthread c dl rt)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
  target_link_directories(Proto PRIVATE /usr/lib64)
endif()

if(THUNDER_DEPLOY_AUTO)
  include("${CMAKE_SOURCE_DIR}/cmake/ThunderDeploy.cmake")
  thunder_deploy_copy(Proto "lib/libProto.so")
endif()
