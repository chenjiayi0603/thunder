#!/bin/bash

# Thunder 测试环境设置脚本
# 用于在本地或 CI 环境中设置测试环境

set -e

echo "=========================================="
echo "Thunder Test Environment Setup"
echo "=========================================="

# 检查 Docker 和 Docker Compose
if ! command -v docker &> /dev/null; then
    echo "Error: Docker is not installed"
    exit 1
fi

if ! command -v docker-compose &> /dev/null; then
    echo "Error: Docker Compose is not installed"
    exit 1
fi

echo "Docker and Docker Compose are available"
echo ""

# 创建测试报告目录
TEST_REPORT_DIR="./test-reports"
mkdir -p "${TEST_REPORT_DIR}"
echo "Test report directory created: ${TEST_REPORT_DIR}"
echo ""

# 创建 Mock 服务配置文件
echo "Creating mock service configurations..."

# WireMock 映射配置
MOCK_HTTP_DIR="./test_services/mock-http"
mkdir -p "${MOCK_HTTP_DIR}/mappings" "${MOCK_HTTP_DIR}/__files"

# 创建示例映射
cat > "${MOCK_HTTP_DIR}/mappings/test-api.json" << 'EOF'
{
  "request": {
    "method": "GET",
    "url": "/api/v1/test"
  },
  "response": {
    "status": 200,
    "headers": {
      "Content-Type": "application/json"
    },
    "jsonBody": {
      "status": "success",
      "message": "Test API response",
      "data": {
        "id": 1,
        "name": "Test Item"
      }
    }
  }
}
EOF

cat > "${MOCK_HTTP_DIR}/mappings/health-check.json" << 'EOF'
{
  "request": {
    "method": "GET",
    "url": "/health"
  },
  "response": {
    "status": 200,
    "headers": {
      "Content-Type": "application/json"
    },
    "jsonBody": {
      "status": "healthy",
      "timestamp": "{{now}}"
    }
  }
}
EOF

echo "Mock HTTP configurations created"
echo ""

# 创建 MockServer 配置
cat > "${MOCK_HTTP_DIR}/mockserver-config.json" << 'EOF'
[
  {
    "httpRequest": {
      "method": "GET",
      "path": "/external/api/v1/data"
    },
    "httpResponse": {
      "statusCode": 200,
      "headers": {
        "Content-Type": ["application/json"]
      },
      "body": {
        "type": "JSON",
        "json": "{\"data\": \"mock external api response\"}"
      }
    }
  },
  {
    "httpRequest": {
      "method": "POST",
      "path": "/external/api/v1/process"
    },
    "httpResponse": {
      "statusCode": 201,
      "headers": {
        "Content-Type": ["application/json"]
      },
      "body": {
        "type": "JSON",
        "json": "{\"status\": \"processed\", \"id\": \"{{randomString 10}}\"}"
      }
    }
  }
]
EOF

echo "MockServer configurations created"
echo ""

# 创建 Nginx 配置用于报告查看
cat > "./test_services/nginx.conf" << 'EOF'
events {
    worker_connections 1024;
}

http {
    include /etc/nginx/mime.types;
    default_type application/octet-stream;

    server {
        listen 80;
        server_name localhost;

        location / {
            root /usr/share/nginx/html;
            index index.html;
            autoindex on;
            autoindex_exact_size off;
            autoindex_localtime on;
        }

        # 防止显示隐藏文件
        location ~ /\. {
            deny all;
        }
    }
}
EOF

echo "Nginx configuration created"
echo ""

# 创建测试环境变量文件
cat > ".env.test" << 'EOF'
# Thunder 测试环境变量
CLEANUP_DATA=true
GENERATE_HTML_REPORT=true
LOG_LEVEL=INFO
TEST_TIMEOUT_SECONDS=300
MAX_CONCURRENT_COROUTINES=1000

# 服务连接信息（在 docker-compose 中会被覆盖）
REDIS_HOST=test-redis
REDIS_PORT=6379
REDIS_PASSWORD=

MYSQL_HOST=test-mysql
MYSQL_PORT=3306
MYSQL_USER=thunder_test_user
MYSQL_PASSWORD=thunder_test_pass
MYSQL_DATABASE=thunder_test_db

HTTP_MOCK_HOST=test-http-mock
HTTP_MOCK_PORT=8080

EXTERNAL_API_HOST=test-external-api
EXTERNAL_API_PORT=1080
EOF

echo "Test environment file created: .env.test"
echo ""

# 创建本地测试运行脚本
cat > "run-local-tests.sh" << 'EOF'
#!/bin/bash

# 本地测试运行脚本
# 使用 Docker Compose 运行集成测试

set -e

echo "Running Thunder integration tests locally..."

# 加载环境变量
if [ -f .env.test ]; then
    export $(cat .env.test | grep -v '^#' | xargs)
fi

# 构建并运行测试
docker-compose -f integration-test/docker-compose.test.yml down -v
docker-compose -f integration-test/docker-compose.test.yml build
docker-compose -f integration-test/docker-compose.test.yml up --abort-on-container-exit --exit-code-from thunder-test

TEST_EXIT_CODE=$?

echo ""
echo "Test execution completed with exit code: ${TEST_EXIT_CODE}"

# 显示测试报告位置
if [ -d "./test-reports" ]; then
    echo ""
    echo "Test reports available in: ./test-reports"
    echo "To view reports, open: http://localhost:8081"
    echo ""
    echo "Report files:"
    ls -la ./test-reports/
fi

exit ${TEST_EXIT_CODE}
EOF

chmod +x "run-local-tests.sh"
echo "Local test runner created: run-local-tests.sh"
echo ""

# 创建 CI 测试脚本
cat > "run-ci-tests.sh" << 'EOF'
#!/bin/bash

# CI 环境测试运行脚本
# 用于 GitHub Actions、GitLab CI 等

set -e

echo "Running Thunder integration tests in CI environment..."

# 设置环境变量
export CLEANUP_DATA=true
export GENERATE_HTML_REPORT=true
export LOG_LEVEL=INFO

# 构建并运行测试
docker-compose -f integration-test/docker-compose.test.yml down -v
docker-compose -f integration-test/docker-compose.test.yml build --no-cache
docker-compose -f integration-test/docker-compose.test.yml up --abort-on-container-exit --exit-code-from thunder-test

TEST_EXIT_CODE=$?

echo ""
echo "Test execution completed with exit code: ${TEST_EXIT_CODE}"

# 收集测试报告
if [ -d "./test-reports" ]; then
    echo "Collecting test reports..."
    
    # 上传测试报告到 CI 系统
    if [ -n "${GITHUB_ACTIONS}" ]; then
        echo "Uploading test reports to GitHub Actions..."
        # GitHub Actions 特定的上传逻辑
    fi
    
    if [ -n "${GITLAB_CI}" ]; then
        echo "Uploading test reports to GitLab CI..."
        # GitLab CI 特定的上传逻辑
    fi
    
    # 显示测试摘要
    if [ -f "./test-reports/test-results.xml" ]; then
        echo ""
        echo "Test Results Summary:"
        grep -E "(testsuite|testcase|failure|error)" "./test-reports/test-results.xml" | head -20
    fi
fi

exit ${TEST_EXIT_CODE}
EOF

chmod +x "run-ci-tests.sh"
echo "CI test runner created: run-ci-tests.sh"
echo ""

echo "=========================================="
echo "Test environment setup completed!"
echo "=========================================="
echo ""
echo "Available commands:"
echo "  ./run-local-tests.sh    - 在本地运行集成测试"
echo "  ./run-ci-tests.sh       - 在 CI 环境运行集成测试"
echo "  docker-compose -f integration-test/docker-compose.test.yml up - 手动启动测试环境"
echo ""
echo "Test reports will be available at:"
echo "  Local: ./test-reports/"
echo "  Web: http://localhost:8081 (after running tests)"
echo ""
echo "To clean up test environment:"
echo "  docker-compose -f integration-test/docker-compose.test.yml down -v"
echo ""