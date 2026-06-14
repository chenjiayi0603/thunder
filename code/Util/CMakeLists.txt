# libUtil.so — 源文件由 GLOB 收集；Mysql/Curl 等符号未链入 libUtil，由最终可执行/插件在运行时与其它 .so 一并解析

set(UTIL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")

file(GLOB_RECURSE UTIL_SOURCES CONFIGURE_DEPENDS
  "${UTIL_SRC}/*.cpp"
  "${UTIL_SRC}/*.cc"
  "${UTIL_SRC}/*.c"
)

add_library(Util SHARED ${UTIL_SOURCES})
set_target_properties(Util PROPERTIES OUTPUT_NAME "Util")

# .c 后缀按 C++ 编译（与历史行为一致）
foreach(_src IN LISTS UTIL_SOURCES)
  if(_src MATCHES "\\.[cC]$")
    set_source_files_properties("${_src}" PROPERTIES LANGUAGE CXX)
  endif()
endforeach()

target_compile_features(Util PUBLIC cxx_std_20)

target_compile_definitions(Util PRIVATE _LINUX_OS_)

target_compile_options(Util PRIVATE
  -Wall
  -Wno-format
  -fno-strict-aliasing
  -Wno-sign-compare
)

target_include_directories(Util PUBLIC
  "${UTIL_SRC}"
  "${THUNDER_3PARTY}"      # 供 threadpool.h 找 concurrentqueue.h
)

target_include_directories(Util PRIVATE
  "${THUNDER_3PARTY}/include"
  "${THUNDER_3PARTY}/include/mariadb"
  "${THUNDER_3PARTY}/include/mongoc/src/libbson/src"
  "${THUNDER_3PARTY}/include/mongoc/src/libbson/src/bson"
  "${THUNDER_3PARTY}/include/mongoc"
  "${THUNDER_3PARTY}/include/bson"
)

target_link_options(Util PRIVATE
  "LINKER:-soname,libUtil.so"
  # libUtil 内部分符号由主程序/其它 so 提供
  "LINKER:--allow-shlib-undefined"
)

target_link_directories(Util PRIVATE
  "${THUNDER_3PARTY}/lib"
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
  target_link_directories(Util PRIVATE /usr/lib64)
endif()

find_package(Threads REQUIRED)

# 链接：ev jemalloc bson2 mongoc2 pthread dl rt …
target_link_libraries(Util PRIVATE
  ev
  jemalloc
  bson2
  mongoc2
  Threads::Threads
  dl
  rt
)

if(THUNDER_DEPLOY_AUTO)
  include("${CMAKE_SOURCE_DIR}/cmake/ThunderDeploy.cmake")
  thunder_deploy_copy(Util "lib/libUtil.so")
endif()
