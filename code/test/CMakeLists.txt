include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
enable_testing()

# ===== 公共变量 =====
set(_thunder_test_rpath
  "${THUNDER_3PARTY}/lib"
  "${THUNDER_3PARTY}/lib/mariadb"
  "${THUNDER_3PARTY}/protobuf/build"
  "${THUNDER_3PARTY}/protobuf/build/third_party/utf8_range"
  "${THUNDER_3PARTY}/protobuf/build/_deps/absl-build"
)

# ---- 目标 1：纯协程机制测试（无 Net 链接依赖） ----
add_executable(thunder_test_coroutine
  coroutine/test_coroutine20.cpp
)
target_include_directories(thunder_test_coroutine PRIVATE
  "${THUNDER_NET}/include"
  "${THUNDER_NET}/include/coro"
)
target_compile_features(thunder_test_coroutine PRIVATE cxx_std_20)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(thunder_test_coroutine PRIVATE -fcoroutines)
endif()
target_link_libraries(thunder_test_coroutine PRIVATE GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(thunder_test_coroutine)

# ---- 目标 2：Center Raft 纯逻辑（无 Net 依赖）----
add_executable(thunder_test_center_raft
  center/test_center_raft.cpp
)
target_compile_features(thunder_test_center_raft PRIVATE cxx_std_20)
target_link_libraries(thunder_test_center_raft PRIVATE GTest::gtest_main)
gtest_discover_tests(thunder_test_center_raft)

# ---- 目标 3：StepCo20 / StepCo20Func / HttpRespAwaiter ----
file(GLOB_RECURSE THUNDER_NET_ALL_SOURCES_FOR_TEST CONFIGURE_DEPENDS
  "${THUNDER_NET_SRC}/*.cpp"
  "${THUNDER_NET_SRC}/*.cc")
list(FILTER THUNDER_NET_ALL_SOURCES_FOR_TEST EXCLUDE REGEX "/main\\.cpp$")

add_library(thunder_net_sources_for_test OBJECT ${THUNDER_NET_ALL_SOURCES_FOR_TEST})
thunder_apply_common_compile_options(thunder_net_sources_for_test)
target_compile_definitions(thunder_net_sources_for_test PRIVATE THUNDER_SUPPRESS_NETDEFINE_AUTO_DEBUG)
thunder_target_include_net(thunder_net_sources_for_test)
target_compile_features(thunder_net_sources_for_test PRIVATE cxx_std_20)

add_executable(thunder_test_step_co20
  step/test_step_co20.cpp
  $<TARGET_OBJECTS:thunder_net_sources_for_test>
)
target_compile_features(thunder_test_step_co20 PRIVATE cxx_std_20)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(thunder_test_step_co20 PRIVATE -fcoroutines)
endif()
thunder_target_include_net(thunder_test_step_co20)
thunder_link_thirdparty_shared(thunder_test_step_co20)
target_link_libraries(thunder_test_step_co20 PRIVATE GTest::gtest_main)

# ---- 目标 4：Net Interface ----
add_executable(thunder_test_net_interface
  interface/test_net_interface.cpp
  $<TARGET_OBJECTS:thunder_net_sources_for_test>
)
target_compile_features(thunder_test_net_interface PRIVATE cxx_std_20)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(thunder_test_net_interface PRIVATE -fcoroutines)
endif()
thunder_target_include_net(thunder_test_net_interface)
thunder_link_thirdparty_shared(thunder_test_net_interface)
target_link_libraries(thunder_test_net_interface PRIVATE GTest::gtest_main GTest::gmock)

# ---- 目标 5：Thunder ORM ----
add_executable(thunder_test_orm
  orm/test_thunder_orm.cpp
)
target_compile_features(thunder_test_orm PRIVATE cxx_std_20)
target_include_directories(thunder_test_orm PRIVATE
  "${THUNDER_UTIL}/src"
  "${THUNDER_3PARTY}/include"
  "${THUNDER_3PARTY}/include/mariadb"
)
target_link_directories(thunder_test_orm PRIVATE
  "${THUNDER_3PARTY}/lib"
)
find_package(Threads REQUIRED)
target_link_libraries(thunder_test_orm PRIVATE
  Util
  GTest::gtest_main
  mariadb
  hiredis_vip
  curl
  Threads::Threads
)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
  target_link_directories(thunder_test_orm PRIVATE /usr/lib64)
endif()
set_target_properties(thunder_test_orm PROPERTIES BUILD_RPATH "${_thunder_test_rpath}")
gtest_discover_tests(thunder_test_orm DISCOVERY_MODE PRE_TEST)

# ===== 新增 R2 单元测试 =====

# ---- CBuffer 环形缓冲区 ----
add_executable(thunder_test_util_buffer
  util/test_util_buffer.cpp
)
target_include_directories(thunder_test_util_buffer PRIVATE
  "${THUNDER_UTIL}/src"
)
target_compile_features(thunder_test_util_buffer PRIVATE cxx_std_20)
target_link_libraries(thunder_test_util_buffer PRIVATE GTest::gtest_main)
gtest_discover_tests(thunder_test_util_buffer)

# ---- CJsonObject JSON 工具 ----
add_executable(thunder_test_util_json
  util/test_util_json.cpp
)
target_include_directories(thunder_test_util_json PRIVATE
  "${THUNDER_UTIL}/src"
)
target_compile_features(thunder_test_util_json PRIVATE cxx_std_20)
target_link_directories(thunder_test_util_json PRIVATE
  "${THUNDER_3PARTY}/lib"
)
target_link_libraries(thunder_test_util_json PRIVATE
  GTest::gtest_main
  Util
  mariadb
  hiredis_vip
  curl
  Threads::Threads
)
gtest_discover_tests(thunder_test_util_json)

# ---- ThreadPool 线程池 ----
add_executable(thunder_test_util_threadpool
  util/test_util_threadpool.cpp
)
target_include_directories(thunder_test_util_threadpool PRIVATE
  "${THUNDER_UTIL}/src"
)
target_compile_features(thunder_test_util_threadpool PRIVATE cxx_std_20)
target_link_libraries(thunder_test_util_threadpool PRIVATE
  GTest::gtest_main
  Threads::Threads
)
gtest_discover_tests(thunder_test_util_threadpool)

# ---- Proto 序列化 (coor/msg) ----
add_executable(thunder_test_proto
  proto/test_proto_coor.cpp
  "${THUNDER_PROTO}/src/coor.pb.cc"
  $<TARGET_OBJECTS:thunder_net_sources_for_test>
)
target_compile_features(thunder_test_proto PRIVATE cxx_std_20)
thunder_target_include_net(thunder_test_proto)
target_include_directories(thunder_test_proto PRIVATE
  "${THUNDER_PROTO}/src"
  "${THUNDER_PROTO}/include"
)
thunder_link_thirdparty_shared(thunder_test_proto)
target_link_libraries(thunder_test_proto PRIVATE GTest::gtest_main)
set_target_properties(thunder_test_proto PROPERTIES BUILD_RPATH "${_thunder_test_rpath}")
gtest_discover_tests(thunder_test_proto DISCOVERY_MODE PRE_TEST)

# ---- ProtoCodec 编解码 ----
add_executable(thunder_test_codec_proto
  codec/test_codec_proto.cpp
  $<TARGET_OBJECTS:thunder_net_sources_for_test>
)
target_compile_features(thunder_test_codec_proto PRIVATE cxx_std_20)
thunder_target_include_net(thunder_test_codec_proto)
thunder_link_thirdparty_shared(thunder_test_codec_proto)
target_link_libraries(thunder_test_codec_proto PRIVATE GTest::gtest_main)
set_target_properties(thunder_test_codec_proto PROPERTIES BUILD_RPATH "${_thunder_test_rpath}")
gtest_discover_tests(thunder_test_codec_proto DISCOVERY_MODE PRE_TEST)

# ---- Session ----
add_executable(thunder_test_session
  session/test_session.cpp
  $<TARGET_OBJECTS:thunder_net_sources_for_test>
)
target_compile_features(thunder_test_session PRIVATE cxx_std_20)
thunder_target_include_net(thunder_test_session)
thunder_link_thirdparty_shared(thunder_test_session)
target_link_libraries(thunder_test_session PRIVATE GTest::gtest_main)
set_target_properties(thunder_test_session PROPERTIES BUILD_RPATH "${_thunder_test_rpath}")
gtest_discover_tests(thunder_test_session DISCOVERY_MODE PRE_TEST)

# ---- ConnectionAttr ----
add_executable(thunder_test_connection
  connection/test_connection.cpp
)
target_include_directories(thunder_test_connection PRIVATE
  "${THUNDER_NET}/include"
  "${THUNDER_UTIL}/src"
  "${THUNDER_3PARTY}/include"
  "${THUNDER_3PARTY}/include/libev"
  "${THUNDER_3PARTY}/include/mariadb"
)
target_compile_features(thunder_test_connection PRIVATE cxx_std_20)
target_link_libraries(thunder_test_connection PRIVATE GTest::gtest_main)
gtest_discover_tests(thunder_test_connection)

# ---- RedisOperator 存储协议 ----
add_executable(thunder_test_storage_redis
  storage/test_storage_redis.cpp
  $<TARGET_OBJECTS:thunder_net_sources_for_test>
)
target_compile_features(thunder_test_storage_redis PRIVATE cxx_std_20)
thunder_target_include_net(thunder_test_storage_redis)
thunder_link_thirdparty_shared(thunder_test_storage_redis)
target_link_libraries(thunder_test_storage_redis PRIVATE GTest::gtest_main)
set_target_properties(thunder_test_storage_redis PROPERTIES BUILD_RPATH "${_thunder_test_rpath}")
gtest_discover_tests(thunder_test_storage_redis DISCOVERY_MODE PRE_TEST)

# ---- RPATH 设置 ----
set_target_properties(thunder_test_step_co20 PROPERTIES BUILD_RPATH "${_thunder_test_rpath}")
set_target_properties(thunder_test_net_interface PROPERTIES BUILD_RPATH "${_thunder_test_rpath}")
gtest_discover_tests(thunder_test_step_co20 DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(thunder_test_net_interface DISCOVERY_MODE PRE_TEST)

# ---- MemOperator 存储协议 ----
add_executable(thunder_test_storage_mem
  storage/test_storage_mem.cpp
  $<TARGET_OBJECTS:thunder_net_sources_for_test>
)
target_compile_features(thunder_test_storage_mem PRIVATE cxx_std_20)
thunder_target_include_net(thunder_test_storage_mem)
thunder_link_thirdparty_shared(thunder_test_storage_mem)
target_link_libraries(thunder_test_storage_mem PRIVATE GTest::gtest_main)
set_target_properties(thunder_test_storage_mem PROPERTIES BUILD_RPATH "${_thunder_test_rpath}")
gtest_discover_tests(thunder_test_storage_mem DISCOVERY_MODE PRE_TEST)

# ---- HttpCodec 编解码 E2E ----
add_executable(thunder_test_codec_http
  codec/test_codec_http.cpp
  $<TARGET_OBJECTS:thunder_net_sources_for_test>
)
target_compile_features(thunder_test_codec_http PRIVATE cxx_std_20)
thunder_target_include_net(thunder_test_codec_http)
thunder_link_thirdparty_shared(thunder_test_codec_http)
target_link_libraries(thunder_test_codec_http PRIVATE GTest::gtest_main)
set_target_properties(thunder_test_codec_http PROPERTIES BUILD_RPATH "${_thunder_test_rpath}")
gtest_discover_tests(thunder_test_codec_http DISCOVERY_MODE PRE_TEST)

# ---- ClientMsgCodec 编解码 E2E ----
add_executable(thunder_test_codec_client
  codec/test_codec_client.cpp
  $<TARGET_OBJECTS:thunder_net_sources_for_test>
)
target_compile_features(thunder_test_codec_client PRIVATE cxx_std_20)
thunder_target_include_net(thunder_test_codec_client)
thunder_link_thirdparty_shared(thunder_test_codec_client)
target_link_libraries(thunder_test_codec_client PRIVATE GTest::gtest_main)
set_target_properties(thunder_test_codec_client PROPERTIES BUILD_RPATH "${_thunder_test_rpath}")
gtest_discover_tests(thunder_test_codec_client DISCOVERY_MODE PRE_TEST)

# ---- ConHash 一致性哈希 ----
add_executable(thunder_test_dispatcher_conhash
  dispatcher/test_conhash.cpp
  $<TARGET_OBJECTS:thunder_net_sources_for_test>
)
target_compile_features(thunder_test_dispatcher_conhash PRIVATE cxx_std_20)
thunder_target_include_net(thunder_test_dispatcher_conhash)
thunder_link_thirdparty_shared(thunder_test_dispatcher_conhash)
target_link_libraries(thunder_test_dispatcher_conhash PRIVATE GTest::gtest_main)
set_target_properties(thunder_test_dispatcher_conhash PROPERTIES BUILD_RPATH "${_thunder_test_rpath}")
gtest_discover_tests(thunder_test_dispatcher_conhash DISCOVERY_MODE PRE_TEST)

# ---- ShmRingQueue 共享内存队列 ----
add_executable(thunder_test_shm_queue
  labor/test_shm_queue.cpp
)
target_include_directories(thunder_test_shm_queue PRIVATE
  "${THUNDER_NET}/include"
  "${THUNDER_UTIL}/src"
  "${THUNDER_3PARTY}/include"
  "${THUNDER_3PARTY}/include/libev"
)
target_compile_features(thunder_test_shm_queue PRIVATE cxx_std_20)
target_link_libraries(thunder_test_shm_queue PRIVATE GTest::gtest_main Threads::Threads)
gtest_discover_tests(thunder_test_shm_queue)

# ---- TcpCenterConnector 单元测试 ----
add_executable(thunder_test_connector_tcp
  connector/test_connector_tcp.cpp
  $<TARGET_OBJECTS:thunder_net_sources_for_test>
)
target_compile_features(thunder_test_connector_tcp PRIVATE cxx_std_20)
thunder_target_include_net(thunder_test_connector_tcp)
thunder_link_thirdparty_shared(thunder_test_connector_tcp)
target_link_libraries(thunder_test_connector_tcp PRIVATE GTest::gtest_main)
set_target_properties(thunder_test_connector_tcp PROPERTIES BUILD_RPATH "${_thunder_test_rpath}")
gtest_discover_tests(thunder_test_connector_tcp DISCOVERY_MODE PRE_TEST)

# ---- E2E 容器烟雾测试 (需 THUNDER_E2E_ENABLED=1) ----
add_executable(thunder_test_e2e_smoke
  e2e/test_e2e_smoke.cpp
  $<TARGET_OBJECTS:thunder_net_sources_for_test>
)
target_compile_features(thunder_test_e2e_smoke PRIVATE cxx_std_20)
thunder_target_include_net(thunder_test_e2e_smoke)
thunder_link_thirdparty_shared(thunder_test_e2e_smoke)
target_link_libraries(thunder_test_e2e_smoke PRIVATE GTest::gtest_main curl)
set_target_properties(thunder_test_e2e_smoke PROPERTIES BUILD_RPATH "${_thunder_test_rpath}")
gtest_discover_tests(thunder_test_e2e_smoke DISCOVERY_MODE PRE_TEST)
