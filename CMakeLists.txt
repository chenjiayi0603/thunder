cmake_minimum_required(VERSION 3.12)
project(Thunder CXX)

# 设置 C++20 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 添加编译选项
if(MSVC)
    add_compile_options(/W4 /std:c++latest)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -std=c++20)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcoroutines")
endif()

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 查找依赖
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# 包含目录
include_directories(code/Net/include)
include_directories(code/Net/src)
include_directories(code/Hello/include)
include_directories(code/Util/src)
include_directories(code/3party/include)
include_directories(code/3party/include/mariadb)
include_directories(code/3party/log4cplus/include)
include_directories(code/3party/protobuf/src)

# 链接目录
link_directories(${CMAKE_SOURCE_DIR}/deploy/3lib/lib)

# 核心网络库源文件
set(NET_SOURCES
    code/Net/src/step/StepState.cpp
    code/Net/src/step/StepCoroutine.cpp
    code/Net/src/step/HttpAwaitable.cpp
    code/Net/src/step/CoroutineState.cpp
    code/Net/src/step/HttpStep.cpp
    code/Net/src/codec/HttpCodec.cpp
)

# Hello 模块源文件
set(HELLO_SOURCES
    code/Hello/src/ModuleHello/StepHttpRequestCo.cpp
    code/Hello/src/ModuleHello/StepHttpRequestState.cpp
    code/Hello/src/ModuleHello/HttpRequestCo.cpp
    code/Hello/src/ModuleHello/HttpStateFuncCo.cpp
    code/Net/src/main.cpp
)

# 创建共享库
add_library(thunder_core ${NET_SOURCES})
target_link_libraries(thunder_core PUBLIC 
    OpenSSL::SSL 
    OpenSSL::Crypto 
    Threads::Threads
    log4cplus
    protobuf
)

# 创建可执行文件
add_executable(thunder_server ${HELLO_SOURCES})
target_link_libraries(thunder_server PRIVATE thunder_core)

# 测试选项
option(ENABLE_TESTS "Enable unit tests" ON)
option(ENABLE_INTEGRATION_TESTS "Enable integration tests" OFF)

if(ENABLE_TESTS)
    find_package(GTest REQUIRED)
    include(GoogleTest)
endif()

if(ENABLE_TESTS)
    # 现有单元测试（转换为 Google Test）
    add_executable(test_cotask test/test_cotask.cpp)
    target_link_libraries(test_cotask PRIVATE thunder_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_cotask)
    
    add_executable(test_http_awaitable test/test_http_awaitable.cpp)
    target_link_libraries(test_http_awaitable PRIVATE thunder_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_http_awaitable)
    
    # 启用测试
    enable_testing()
endif()

if(ENABLE_INTEGRATION_TESTS)
    # 集成测试
    add_executable(integration_test integration-test/src/integration_test_base.cpp)
    target_link_libraries(integration_test PRIVATE thunder_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(integration_test WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    
    # 覆盖率编译选项
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(integration_test PRIVATE -fprofile-arcs -ftest-coverage)
        target_link_libraries(integration_test PRIVATE gcov)
    endif()
endif()

# 安装目标
install(TARGETS thunder_server DESTINATION bin)
install(TARGETS thunder_core DESTINATION lib)
install(DIRECTORY code/Net/include/ DESTINATION include)