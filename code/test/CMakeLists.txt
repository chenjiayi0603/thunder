include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
enable_testing()

# ---- 目标 1：纯协程机制测试（无 Net 依赖） ----
add_executable(thunder_test_coroutine
  coroutine/test_coroutine20.cpp
)
target_include_directories(thunder_test_coroutine PRIVATE
  "${THUNDER_NET}/include/coro"
)
target_compile_features(thunder_test_coroutine PRIVATE cxx_std_20)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(thunder_test_coroutine PRIVATE -fcoroutines)
endif()
target_link_libraries(thunder_test_coroutine PRIVATE GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(thunder_test_coroutine)

# ---- 目标 2：StepCo20 / StepCo20Func / HttpRespAwaiter ----
# 与 Hello 一致：libNet.so 仅为部分子目录，完整符号需编入全部 Net 源码，故此处不链接 Net 共享库。
file(GLOB_RECURSE THUNDER_NET_ALL_SOURCES_FOR_TEST CONFIGURE_DEPENDS
  "${THUNDER_NET_SRC}/*.cpp"
  "${THUNDER_NET_SRC}/*.cc")
# 不可与 GTest::gtest_main 同时链接 Net 的 main.cpp（会抢 main、要求 argc==2）
list(FILTER THUNDER_NET_ALL_SOURCES_FOR_TEST EXCLUDE REGEX "/main\\.cpp$")
add_executable(thunder_test_step_co20
  step/test_step_co20.cpp
  ${THUNDER_NET_ALL_SOURCES_FOR_TEST}
)
thunder_apply_common_compile_options(thunder_test_step_co20)
target_compile_definitions(thunder_test_step_co20 PRIVATE THUNDER_SUPPRESS_NETDEFINE_AUTO_DEBUG)
thunder_target_include_net(thunder_test_step_co20)
thunder_link_thirdparty_shared(thunder_test_step_co20)
target_link_libraries(thunder_test_step_co20 PRIVATE GTest::gtest_main)
# 链接后立刻 discover 会执行二进制，需能加载 mariadb/protobuf 等 .so；改为 PRE_TEST，并补 BUILD_RPATH 便于本地/ctest 运行
set(_thunder_test_step_rpath
  "${THUNDER_3PARTY}/lib"
  "${THUNDER_3PARTY}/lib/mariadb"
  "${THUNDER_3PARTY}/protobuf/build"
  "${THUNDER_3PARTY}/protobuf/build/third_party/utf8_range"
  "${THUNDER_3PARTY}/protobuf/build/_deps/absl-build"
)
set_target_properties(thunder_test_step_co20 PROPERTIES BUILD_RPATH "${_thunder_test_step_rpath}")
gtest_discover_tests(thunder_test_step_co20 DISCOVERY_MODE PRE_TEST)
