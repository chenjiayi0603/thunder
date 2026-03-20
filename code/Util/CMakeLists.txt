# libUtil.so — 与 code/Util/makefile 源文件与链接顺序对齐（第三方库路径同 makefile）
# 说明：Mysql/Curl 等符号在 makefile 中未链入 libUtil，由最终可执行/插件在运行时与其它 .so 一并解析；
#      若链接器报未定义符号，可保持与 makefile 一致的 --allow-shlib-undefined 行为。

set(UTIL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")

file(GLOB_RECURSE UTIL_SOURCES CONFIGURE_DEPENDS
  "${UTIL_SRC}/*.cpp"
  "${UTIL_SRC}/*.cc"
  "${UTIL_SRC}/*.c"
)

add_library(Util SHARED ${UTIL_SOURCES})
set_target_properties(Util PROPERTIES OUTPUT_NAME "Util")

# 与 Util/makefile 一致：%.c 规则使用 $(CXX) 编译，实为按 C++ 编译
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
  # 与历史 makefile 一致：libUtil 内部分符号由主程序/其它 so 提供
  "LINKER:--allow-shlib-undefined"
)

target_link_directories(Util PRIVATE
  "${THUNDER_3PARTY}/lib"
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
  target_link_directories(Util PRIVATE /usr/lib64)
endif()

find_package(Threads REQUIRED)

# makefile: -lev -ljemalloc -lbson2 -lmongoc2 -lpthread -lc -ldl -lrt
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
