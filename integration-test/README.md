# Thunder 框架集成测试系统

## 概述

Thunder 框架的容器化集成测试系统，使用 Google Test 框架和 Docker Compose 提供完整的测试环境。

## 特性

- ✅ **容器化测试环境**：使用 Docker Compose 管理测试依赖服务
- ✅ **Google Test 集成**：标准的 C++ 测试框架
- ✅ **自动数据清理**：每次测试后自动清理测试数据
- ✅ **HTML 测试报告**：生成美观的 HTML 测试报告
- ✅ **代码覆盖率**：支持代码覆盖率分析和报告
- ✅ **外部服务模拟**：支持 WireMock 和 MockServer 模拟外部 API
- ✅ **资源限制**：可配置的 CPU/内存资源限制
- ✅ **CI/CD 集成**：支持 GitHub Actions、GitLab CI 等

## 目录结构

```
integration-test/
├── docker-compose.test.yml      # 测试环境 Docker Compose 配置
├── scripts/
│   ├── run-tests.sh            # 容器内测试运行脚本
│   └── setup-test-env.sh       # 测试环境设置脚本
├── src/
│   ├── integration_test_base.hpp # 测试基类
│   ├── integration_test_base.cpp # 测试基类实现
│   └── main_test.cpp           # 主测试文件
├── test_services/
│   ├── redis/                  # Redis 测试配置
│   ├── mysql/                  # MySQL 测试配置
│   └── mock-http/              # HTTP Mock 配置
└── README.md                   # 本文档
```

## 快速开始

### 1. 设置测试环境

```bash
# 运行设置脚本
cd integration-test
./scripts/setup-test-env.sh
```

### 2. 运行本地测试

```bash
# 使用提供的脚本运行测试
./run-local-tests.sh
```

### 3. 查看测试报告

测试完成后，报告将生成在 `./test-reports/` 目录中：
- `test-report.html` - HTML 测试报告
- `coverage-report.html` - 代码覆盖率报告
- `test-results.xml` - JUnit 格式测试结果

也可以通过 Web 查看：http://localhost:8081

## 测试环境配置

### 环境变量

| 变量名 | 默认值 | 描述 |
|--------|--------|------|
| `CLEANUP_DATA` | `true` | 是否清理测试数据 |
| `GENERATE_HTML_REPORT` | `true` | 是否生成 HTML 报告 |
| `LOG_LEVEL` | `INFO` | 日志级别 (DEBUG/INFO/WARN/ERROR) |
| `TEST_TIMEOUT_SECONDS` | `300` | 测试超时时间（秒） |
| `MAX_CONCURRENT_COROUTINES` | `1000` | 最大并发协程数 |

### 服务配置

测试环境包含以下服务：

1. **Redis** (`test-redis:6379`) - 内存数据库
2. **MySQL** (`test-mysql:3306`) - 关系数据库
3. **WireMock** (`test-http-mock:8080`) - HTTP API 模拟
4. **MockServer** (`test-external-api:1080`) - 外部 API 模拟
5. **Nginx** (`test-report-viewer:80`) - 报告查看器

## 编写测试

### 测试基类

所有测试都应继承自 `IntegrationTestBase`：

```cpp
#include "integration_test_base.hpp"

class MyIntegrationTest : public IntegrationTestBase {
protected:
    void SetUp() override {
        IntegrationTestBase::SetUp();
        // 测试特定的初始化
    }
    
    void TearDown() override {
        // 测试特定的清理
        IntegrationTestBase::TearDown();
    }
    
    void TestMyFeature() {
        // 测试逻辑
    }
};
```

### 测试用例

使用 Google Test 宏定义测试用例：

```cpp
TEST_F(MyIntegrationTest, TestFeatureA) {
    TestMyFeature();
}

TEST_F(MyIntegrationTest, TestFeatureB) {
    // 直接测试逻辑
    EXPECT_TRUE(SomeCondition());
}
```

### 测试工具方法

`IntegrationTestBase` 提供以下工具方法：

- `GenerateTestId()` - 生成唯一测试 ID
- `CreateTestCoroutine()` - 创建测试协程
- `WaitForCoroutineCompletion()` - 等待协程完成
- `GetRedisConnectionInfo()` - 获取 Redis 连接信息
- `GetMysqlConnectionInfo()` - 获取 MySQL 连接信息
- `CleanupTestData()` - 清理测试数据

## CI/CD 集成

### GitHub Actions 示例

```yaml
name: Thunder Integration Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    
    steps:
    - uses: actions/checkout@v3
    
    - name: Set up Docker
      run: |
        sudo apt-get update
        sudo apt-get install -y docker-compose
    
    - name: Run integration tests
      run: ./run-ci-tests.sh
    
    - name: Upload test reports
      uses: actions/upload-artifact@v3
      with:
        name: test-reports
        path: ./test-reports/
```

### GitLab CI 示例

```yaml
stages:
  - test

integration-test:
  stage: test
  image: docker:latest
  services:
    - docker:dind
  variables:
    DOCKER_HOST: tcp://docker:2375
    DOCKER_DRIVER: overlay2
  script:
    - apk add --no-cache docker-compose bash
    - ./run-ci-tests.sh
  artifacts:
    paths:
      - test-reports/
    when: always
```

## 高级配置

### 自定义 Mock 服务

在 `test_services/mock-http/mappings/` 目录中添加 WireMock 映射文件：

```json
{
  "request": {
    "method": "GET",
    "url": "/api/custom"
  },
  "response": {
    "status": 200,
    "jsonBody": {
      "message": "Custom response"
    }
  }
}
```

### 资源限制调整

在 `docker-compose.test.yml` 中调整资源限制：

```yaml
services:
  thunder-test:
    deploy:
      resources:
        limits:
          cpus: '4'      # CPU 核心数
          memory: 8GB    # 内存限制
```

### 自定义测试报告

修改 `run-tests.sh` 中的报告生成逻辑：

```bash
# 生成自定义报告格式
gcovr -r /app --html --html-details \
  --html-title "Thunder Coverage Report" \
  -o "${TEST_REPORT_DIR}/custom-coverage.html"
```

## 故障排除

### 常见问题

1. **Docker 容器启动失败**
   - 检查 Docker 服务是否运行：`docker ps`
   - 检查端口冲突：`netstat -tulpn | grep :8080`

2. **测试依赖服务连接失败**
   - 检查服务健康状态：`docker-compose ps`
   - 查看服务日志：`docker-compose logs test-redis`

3. **测试报告未生成**
   - 检查 Python 依赖：`pip3 list | grep gtest2html`
   - 检查文件权限：`ls -la test-reports/`

4. **测试超时**
   - 增加超时时间：`export TEST_TIMEOUT_SECONDS=600`
   - 检查网络连接和资源使用

### 调试模式

启用调试日志：

```bash
export LOG_LEVEL=DEBUG
./run-local-tests.sh
```

### 清理测试环境

完全清理测试环境：

```bash
docker-compose -f integration-test/docker-compose.test.yml down -v
rm -rf test-reports/
```

## 性能测试

### 基准测试配置

```bash
# 设置性能测试参数
export MAX_CONCURRENT_COROUTINES=5000
export TEST_TIMEOUT_SECONDS=600

# 运行性能测试
./run-local-tests.sh
```

### 监控测试资源

```bash
# 监控 Docker 容器资源使用
docker stats

# 查看测试容器日志
docker-compose logs -f thunder-test
```

## 贡献指南

1. 遵循现有的测试代码风格
2. 为新的测试功能添加文档
3. 确保测试可重复运行
4. 添加适当的错误处理
5. 更新 README 中的相关部分

## 许可证

Thunder 框架集成测试系统遵循与主项目相同的许可证。