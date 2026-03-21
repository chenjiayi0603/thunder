# Thunder 工程公共路径与编译选项（由根 CMakeLists include）
set(THUNDER_ROOT "${CMAKE_SOURCE_DIR}" CACHE INTERNAL "")
set(THUNDER_CODE "${THUNDER_ROOT}/code" CACHE INTERNAL "")
set(THUNDER_3PARTY "${THUNDER_CODE}/3party" CACHE INTERNAL "")
set(THUNDER_UTIL "${THUNDER_CODE}/Util" CACHE INTERNAL "")
set(THUNDER_NET "${THUNDER_CODE}/Net" CACHE INTERNAL "")
set(THUNDER_NET_SRC "${THUNDER_NET}/src" CACHE INTERNAL "")
set(THUNDER_PROTO "${THUNDER_CODE}/Proto" CACHE INTERNAL "")
set(THUNDER_DEPLOY "${THUNDER_ROOT}/deploy" CACHE INTERNAL "")

# 与历史 Net 主程序一致的宏与选项
function(thunder_apply_common_compile_options _target)
  target_compile_definitions(${_target} PRIVATE
    _GNU_SOURCE=1
    _REENTRANT
    _LINUX_OS_
    NODE_BEAT=10.0
    WORKER_OVERDUE=60.0
  )
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${_target} PRIVATE
      -Wno-deprecated-declarations
      -Wno-pmf-conversions
      -Wno-error=format-security
    )
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      target_compile_options(${_target} PRIVATE -fcoroutines)
    endif()
  endif()
endfunction()

# 供可执行文件 / 插件使用的头文件路径
function(thunder_target_include_net _target)
  target_include_directories(${_target} PRIVATE
    "${THUNDER_NET}/include"
    "${THUNDER_NET}/src"
    "${THUNDER_UTIL}/src"
    "${THUNDER_UTIL}/src/Util"
    "${THUNDER_3PARTY}/protobuf/src"
    "${THUNDER_3PARTY}/protobuf/build/_deps/absl-src"
    "${THUNDER_3PARTY}/include"
    "${THUNDER_3PARTY}/include/mariadb"
    "${THUNDER_3PARTY}/include/mongoc"
    "${THUNDER_3PARTY}/include/bson"
    "${THUNDER_3PARTY}/include/libev"
  )
endfunction()

# Abseil：protobuf 可能编出静态 .a 或共享 .so（BUILD_SHARED_LIBS=ON 时多为 .so）
# 注意：不可用 `absl_*/*.a` —— 实际路径为 absl-build/absl/hash/、absl/status/ 等，下划线通配会漏掉整棵子树。
function(thunder_collect_absl_archives _outvar)
  file(GLOB_RECURSE _absl LIST_DIRECTORIES false
    "${THUNDER_3PARTY}/protobuf/build/_deps/absl-build/**/libabsl*.a")
  list(SORT _absl)
  set(${_outvar} "${_absl}" PARENT_SCOPE)
endfunction()

function(thunder_collect_absl_shared_libs _outvar)
  file(GLOB_RECURSE _so LIST_DIRECTORIES false
    "${THUNDER_3PARTY}/protobuf/build/_deps/absl-build/**/libabsl*.so")
  # 只保留 soname 主名（如 libabsl_hash.so），去掉 libabsl_*.so.2505.* 避免重复链接
  list(FILTER _so EXCLUDE REGEX "\\.so\\.[0-9]")
  list(SORT _so)
  set(${_outvar} "${_so}" PARENT_SCOPE)
endfunction()

# 解析 libprotobuf / libutf8_validity：若尚未编译 ep_protobuf，find_library 会得到 NOTFOUND，
# 退化为约定路径，使「先 cmake 配置、再编第三方、再编主工程」可行。
function(thunder_resolve_protobuf_shared_libs _out_pb _out_utf8)
  set(_pbdir "${THUNDER_3PARTY}/protobuf/build")
  set(_u8dir "${_pbdir}/third_party/utf8_range")
  find_library(_pb protobuf PATHS "${_pbdir}" NO_DEFAULT_PATH)
  find_library(_u8 utf8_validity PATHS "${_u8dir}" NO_DEFAULT_PATH)
  if(NOT _pb)
    set(_pb "${_pbdir}/libprotobuf.so")
  endif()
  if(NOT _u8)
    set(_u8 "${_u8dir}/libutf8_validity.so")
  endif()
  set(${_out_pb} "${_pb}" PARENT_SCOPE)
  set(${_out_utf8} "${_u8}" PARENT_SCOPE)
  file(GLOB _pb_exists "${_pbdir}/libprotobuf.so*")
  if(NOT _pb_exists AND NOT THUNDER_PROTOBUF_HINT_SHOWN)
    message(STATUS "Thunder: 尚未在 ${_pbdir} 发现 libprotobuf，主工程链接前请先: cmake --build <build> --target ep_protobuf")
    set(THUNDER_PROTOBUF_HINT_SHOWN 1 CACHE INTERNAL "shown protobuf build hint once")
  endif()
endfunction()

# 第三方链接顺序（SHARED 目标）
# 注意：protobuf + utf8_validity + 全部 libabsl*.a 必须包在同一 -Wl,--start-group 内，
# 否则 CMake 会把 --start-group/--end-group 排到最前且中间为空，导致 absl 符号未解析。
function(thunder_link_thirdparty_shared _target)
  thunder_collect_absl_archives(_absl)
  if(NOT _absl)
    thunder_collect_absl_shared_libs(_absl)
  endif()
  find_package(Threads REQUIRED)
  find_package(OpenSSL REQUIRED)

  if(NOT TARGET Util)
    find_library(_util_lib Util PATHS "${THUNDER_UTIL}/lib" NO_DEFAULT_PATH REQUIRED)
  endif()
  thunder_resolve_protobuf_shared_libs(_protobuf_lib _utf8_validity)

  target_link_directories(${_target} PRIVATE
    "${THUNDER_3PARTY}/lib"
    "${THUNDER_3PARTY}/protobuf/build"
    "${THUNDER_3PARTY}/protobuf/build/third_party/utf8_range"
    "${THUNDER_3PARTY}/protobuf/build/_deps/absl-build"
  )

  # Util / DB / 其它（在 absl 组之前；cryptopp 在 protobuf 前）
  if(TARGET Util)
    target_link_libraries(${_target} PRIVATE Util)
  else()
    target_link_directories(${_target} PRIVATE "${THUNDER_UTIL}/lib")
    target_link_libraries(${_target} PRIVATE ${_util_lib})
  endif()
  target_link_libraries(${_target} PRIVATE
    mariadb
    hiredis_vip
    cryptopp
  )

  # protobuf + utf8_validity + absl 同一组，消除循环依赖
  if(_absl)
    target_link_libraries(${_target} PRIVATE
      "-Wl,--start-group"
      ${_protobuf_lib}
      ${_utf8_validity}
      ${_absl}
      "-Wl,--end-group"
    )
  else()
    target_link_libraries(${_target} PRIVATE ${_protobuf_lib} ${_utf8_validity})
  endif()

  target_link_libraries(${_target} PRIVATE
    log4cplus
    ev
    jemalloc
    cares
    curl
    mongoc2
    bson2
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    z
    dl
    rt
  )

  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    target_link_directories(${_target} PRIVATE /usr/lib64)
  endif()
endfunction()
