# Thunder 工程公共路径与编译选项（由根 CMakeLists include）
set(THUNDER_ROOT "${CMAKE_SOURCE_DIR}" CACHE INTERNAL "")
set(THUNDER_CODE "${THUNDER_ROOT}/code" CACHE INTERNAL "")
set(THUNDER_3PARTY "${THUNDER_CODE}/3party" CACHE INTERNAL "")
set(THUNDER_UTIL "${THUNDER_CODE}/Util" CACHE INTERNAL "")
set(THUNDER_NET "${THUNDER_CODE}/Net" CACHE INTERNAL "")
set(THUNDER_NET_SRC "${THUNDER_NET}/src" CACHE INTERNAL "")
set(THUNDER_PROTO "${THUNDER_CODE}/Proto" CACHE INTERNAL "")
set(THUNDER_DEPLOY "${THUNDER_ROOT}/deploy" CACHE INTERNAL "")

# 与 makefile.other / makefile.center 对齐的宏与选项
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

# 供可执行文件 / 插件使用的头文件路径（与 Makefile INC 一致）
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

# Abseil 静态库（与 makefile.other 中 `find ... absl-build -name 'libabsl*.a'` 一致）
# 注意：不可用 `absl_*/*.a` —— 实际路径为 absl-build/absl/hash/、absl/status/ 等，下划线通配会漏掉整棵子树。
function(thunder_collect_absl_archives _outvar)
  file(GLOB_RECURSE _absl LIST_DIRECTORIES false
    "${THUNDER_3PARTY}/protobuf/build/_deps/absl-build/**/libabsl*.a")
  list(SORT _absl)
  set(${_outvar} "${_absl}" PARENT_SCOPE)
endfunction()

# 与 makefile.other 顺序一致的第三方链接（SHARED 目标）
# 注意：protobuf + utf8_validity + 全部 libabsl*.a 必须包在同一 -Wl,--start-group 内，
# 否则 CMake 会把 --start-group/--end-group 排到最前且中间为空，导致 absl 符号未解析。
function(thunder_link_thirdparty_shared _target)
  thunder_collect_absl_archives(_absl)
  find_package(Threads REQUIRED)
  find_package(OpenSSL REQUIRED)

  if(NOT TARGET Util)
    find_library(_util_lib Util PATHS "${THUNDER_UTIL}/lib" NO_DEFAULT_PATH REQUIRED)
  endif()
  find_library(_protobuf_lib protobuf PATHS "${THUNDER_3PARTY}/protobuf/build" NO_DEFAULT_PATH REQUIRED)
  find_library(_utf8_validity utf8_validity PATHS "${THUNDER_3PARTY}/protobuf/build/third_party/utf8_range" NO_DEFAULT_PATH REQUIRED)

  target_link_directories(${_target} PRIVATE
    "${THUNDER_3PARTY}/lib"
    "${THUNDER_3PARTY}/protobuf/build"
    "${THUNDER_3PARTY}/protobuf/build/third_party/utf8_range"
  )

  # Util / DB / 其它（在 absl 组之前，与 Makefile 一致：cryptopp 在 protobuf 前）
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

  # 与 makefile: -lprotobuf -lutf8_validity $(ABSL_LINK) 同一组，消除循环依赖
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
    leveldb
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
