#ifndef INTEGRATION_TEST_BASE_HPP_
#define INTEGRATION_TEST_BASE_HPP_

#include <gtest/gtest.h>
#include <string>
#include <memory>
#include <chrono>
#include <random>
#include <ctime>

#include "step/CoroutineState.hpp"
#include "step/HttpAwaitable.hpp"
#include "step/RedisAwaitable.hpp"
#include "NetDefine.hpp"

namespace thunder {
namespace test {

/**
 * @brief 集成测试基类
 * @note 提供测试环境初始化、数据清理、工具方法等
 */
class IntegrationTestBase : public ::testing::Test {
protected:
    IntegrationTestBase();
    virtual ~IntegrationTestBase() = default;

    // Google Test 生命周期方法
    void SetUp() override;
    void TearDown() override;

    /**
     * @brief 生成唯一测试ID
     * @return 格式: "test_<timestamp>_<random>"
     */
    std::string GenerateTestId();

    /**
     * @brief 创建测试协程状态
     * @return 协程状态指针
     */
    std::unique_ptr<net::CoroutineState> CreateTestCoroutine();

    /**
     * @brief 等待协程完成
     * @param coroutine 协程状态
     * @param timeout_ms 超时时间（毫秒）
     * @return 是否成功完成
     */
    bool WaitForCoroutineCompletion(net::CoroutineState* coroutine, 
                                    int timeout_ms = 5000);

    /**
     * @brief 清理测试数据
     * @param test_id 测试ID
     */
    void CleanupTestData(const std::string& test_id);

    /**
     * @brief 设置测试数据
     * @param test_id 测试ID
     */
    void SetupTestData(const std::string& test_id);

    /**
     * @brief 获取测试环境变量
     * @param name 变量名
     * @param default_value 默认值
     * @return 环境变量值
     */
    std::string GetTestEnv(const std::string& name, 
                          const std::string& default_value = "");

    /**
     * @brief 检查是否启用数据清理
     */
    bool IsCleanupEnabled() const;

    /**
     * @brief 检查是否生成HTML报告
     */
    bool IsHtmlReportEnabled() const;

    /**
     * @brief 获取Redis连接信息
     */
    struct RedisConnectionInfo {
        std::string host;
        int port;
        std::string password;
    };
    RedisConnectionInfo GetRedisConnectionInfo();

    /**
     * @brief 获取MySQL连接信息
     */
    struct MysqlConnectionInfo {
        std::string host;
        int port;
        std::string user;
        std::string password;
        std::string database;
    };
    MysqlConnectionInfo GetMysqlConnectionInfo();

    /**
     * @brief 获取HTTP Mock服务信息
     */
    struct HttpMockInfo {
        std::string host;
        int port;
    };
    HttpMockInfo GetHttpMockInfo();

    /**
     * @brief 获取外部API Mock服务信息
     */
    struct ExternalApiMockInfo {
        std::string host;
        int port;
    };
    ExternalApiMockInfo GetExternalApiMockInfo();

    /**
     * @brief 记录测试开始
     */
    void RecordTestStart(const std::string& test_name);

    /**
     * @brief 记录测试结束
     */
    void RecordTestEnd(const std::string& test_name, bool success);

protected:
    std::string test_id_;
    std::chrono::steady_clock::time_point test_start_time_;
    std::mt19937 random_gen_;
    bool cleanup_enabled_;
    bool html_report_enabled_;
    
    // 测试统计
    struct TestStats {
        int total_tests = 0;
        int passed_tests = 0;
        int failed_tests = 0;
        std::chrono::milliseconds total_duration{0};
    };
    TestStats test_stats_;
};

/**
 * @brief 协程集成测试类
 */
class CoroutineIntegrationTest : public IntegrationTestBase {
protected:
    void SetUp() override;
    void TearDown() override;

    /**
     * @brief 测试基本协程生命周期
     */
    void TestBasicCoroutineLifecycle();

    /**
     * @brief 测试协程异常处理
     */
    void TestCoroutineErrorHandling();

    /**
     * @brief 测试多协程并发
     */
    void TestMultipleCoroutineConcurrency();
};

/**
 * @brief HTTP协程集成测试类
 */
class HttpIntegrationTest : public IntegrationTestBase {
protected:
    void SetUp() override;
    void TearDown() override;

    /**
     * @brief 测试HTTP GET请求
     */
    void TestHttpGetAsync();

    /**
     * @brief 测试HTTP POST请求
     */
    void TestHttpPostAsync();

    /**
     * @brief 测试HTTP超时处理
     */
    void TestHttpTimeout();

    /**
     * @brief 测试HTTP错误处理
     */
    void TestHttpErrorHandling();
};

/**
 * @brief Redis协程集成测试类
 */
class RedisIntegrationTest : public IntegrationTestBase {
protected:
    void SetUp() override;
    void TearDown() override;

    /**
     * @brief 测试Redis基本操作
     */
    void TestRedisBasicOperations();

    /**
     * @brief 测试Redis哈希操作
     */
    void TestRedisHashOperations();

    /**
     * @brief 测试Redis连接池
     */
    void TestRedisConnectionPool();

    /**
     * @brief 测试Redis错误处理
     */
    void TestRedisErrorHandling();
};

/**
 * @brief 端到端集成测试类
 */
class EndToEndIntegrationTest : public IntegrationTestBase {
protected:
    void SetUp() override;
    void TearDown() override;

    /**
     * @brief 测试完整业务流程
     */
    void TestCompleteBusinessFlow();

    /**
     * @brief 测试错误传播链
     */
    void TestErrorPropagationChain();

    /**
     * @brief 测试性能基准
     */
    void TestPerformanceBenchmark();
};

} // namespace test
} // namespace thunder

#endif // INTEGRATION_TEST_BASE_HPP_