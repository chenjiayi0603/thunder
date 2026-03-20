#include "integration_test_base.hpp"
#include <gtest/gtest.h>

// 协程集成测试
TEST_F(CoroutineIntegrationTest, BasicCoroutineLifecycle) {
    TestBasicCoroutineLifecycle();
}

TEST_F(CoroutineIntegrationTest, CoroutineErrorHandling) {
    TestCoroutineErrorHandling();
}

TEST_F(CoroutineIntegrationTest, MultipleCoroutineConcurrency) {
    TestMultipleCoroutineConcurrency();
}

// HTTP 集成测试
TEST_F(HttpIntegrationTest, HttpGetAsyncSuccess) {
    TestHttpGetAsync();
}

TEST_F(HttpIntegrationTest, HttpPostAsyncWithBody) {
    TestHttpPostAsync();
}

TEST_F(HttpIntegrationTest, HttpTimeoutHandling) {
    TestHttpTimeout();
}

TEST_F(HttpIntegrationTest, HttpErrorHandling) {
    TestHttpErrorHandling();
}

// Redis 集成测试
TEST_F(RedisIntegrationTest, RedisBasicOperations) {
    TestRedisBasicOperations();
}

TEST_F(RedisIntegrationTest, RedisHashOperations) {
    TestRedisHashOperations();
}

TEST_F(RedisIntegrationTest, RedisConnectionPool) {
    TestRedisConnectionPool();
}

TEST_F(RedisIntegrationTest, RedisErrorHandling) {
    TestRedisErrorHandling();
}

// 端到端集成测试
TEST_F(EndToEndIntegrationTest, CompleteBusinessFlow) {
    TestCompleteBusinessFlow();
}

TEST_F(EndToEndIntegrationTest, ErrorPropagationChain) {
    TestErrorPropagationChain();
}

TEST_F(EndToEndIntegrationTest, PerformanceBenchmark) {
    TestPerformanceBenchmark();
}

// 主函数
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    
    // 设置测试环境
    ::testing::GTEST_FLAG(output) = "xml:test-results.xml";
    ::testing::GTEST_FLAG(color) = "yes";
    ::testing::GTEST_FLAG(print_time) = 1;
    
    // 根据环境变量设置详细程度
    const char* log_level = std::getenv("LOG_LEVEL");
    if (log_level != nullptr && std::string(log_level) == "DEBUG") {
        ::testing::GTEST_FLAG(verbose) = 1;
    }
    
    std::cout << "==========================================" << std::endl;
    std::cout << "Thunder Framework Integration Tests" << std::endl;
    std::cout << "==========================================" << std::endl;
    std::cout << "Test Configuration:" << std::endl;
    std::cout << "  CLEANUP_DATA: " << (std::getenv("CLEANUP_DATA") ? std::getenv("CLEANUP_DATA") : "true") << std::endl;
    std::cout << "  GENERATE_HTML_REPORT: " << (std::getenv("GENERATE_HTML_REPORT") ? std::getenv("GENERATE_HTML_REPORT") : "true") << std::endl;
    std::cout << "  LOG_LEVEL: " << (log_level ? log_level : "INFO") << std::endl;
    std::cout << "==========================================" << std::endl;
    
    int result = RUN_ALL_TESTS();
    
    // 输出测试摘要
    std::cout << "==========================================" << std::endl;
    std::cout << "Test Summary" << std::endl;
    std::cout << "==========================================" << std::endl;
    
    const ::testing::UnitTest* unit_test = ::testing::UnitTest::GetInstance();
    std::cout << "Total tests: " << unit_test->total_test_count() << std::endl;
    std::cout << "Passed tests: " << unit_test->successful_test_count() << std::endl;
    std::cout << "Failed tests: " << unit_test->failed_test_count() << std::endl;
    
    if (unit_test->failed_test_count() > 0) {
        std::cout << "Failed test cases:" << std::endl;
        for (int i = 0; i < unit_test->total_test_case_count(); ++i) {
            const ::testing::TestCase* test_case = unit_test->GetTestCase(i);
            for (int j = 0; j < test_case->total_test_count(); ++j) {
                const ::testing::TestInfo* test_info = test_case->GetTestInfo(j);
                if (test_info->result()->Failed()) {
                    std::cout << "  - " << test_case->name() << "." << test_info->name() << std::endl;
                }
            }
        }
    }
    
    std::cout << "==========================================" << std::endl;
    
    return result;
}