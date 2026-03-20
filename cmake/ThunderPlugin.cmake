# 与 code/Hello/CMakeLists.txt 中 ModuleHello 对齐：节点插件 .so 的公共链接与第三方路径
function(thunder_plugin_link_net_proto_util _target)
  if(NOT TARGET Net OR NOT TARGET Proto OR NOT TARGET Util)
    message(FATAL_ERROR "thunder_plugin_link_net_proto_util requires targets Net, Proto, Util")
  endif()
  thunder_resolve_protobuf_shared_libs(_protobuf_lib _utf8_validity)
  target_link_directories(${_target} PRIVATE
    "${THUNDER_3PARTY}/lib"
    "${THUNDER_3PARTY}/protobuf/build"
    "${THUNDER_3PARTY}/protobuf/build/third_party/utf8_range"
  )
  target_link_libraries(${_target} PRIVATE
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
    target_link_directories(${_target} PRIVATE /usr/lib64)
  endif()
endfunction()

# ModuleHello 与 Logic/Interface/Center 插件共用的头文件搜索路径（不含各插件私有子目录）
function(thunder_plugin_include_base _target)
  target_include_directories(${_target} PRIVATE
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
    ${ARGN}
  )
endfunction()
