# 简单的 CMake 配置用于测试 Google Test 集成
cmake_minimum_required(VERSION 3.12)
project(SimpleTest CXX)

# 设置 C++20 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找 Google Test
find_package(GTest REQUIRED)
include(GoogleTest)

# 添加简单的测试程序
add_executable(simple_test test_simple_integration.cpp)
target_link_libraries(simple_test GTest::gtest GTest::gtest_main)

# 自动发现测试
gtest_discover_tests(simple_test)

# 启用测试
enable_testing()