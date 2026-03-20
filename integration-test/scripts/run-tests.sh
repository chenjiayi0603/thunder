#!/bin/bash

# Thunder 框架集成测试运行脚本
# 在 Docker 容器中运行集成测试并生成报告

set -e

echo "=========================================="
echo "Thunder Framework Integration Tests"
echo "=========================================="
echo "Start time: $(date)"
echo ""

# 检查环境变量
echo "Environment Configuration:"
echo "  CLEANUP_DATA: ${CLEANUP_DATA:-true}"
echo "  GENERATE_HTML_REPORT: ${GENERATE_HTML_REPORT:-true}"
echo "  TEST_REPORT_DIR: ${TEST_REPORT_DIR:-/app/test-reports}"
echo "  LOG_LEVEL: ${LOG_LEVEL:-INFO}"
echo ""

# 创建报告目录
mkdir -p "${TEST_REPORT_DIR}"

# 等待依赖服务就绪
echo "Waiting for test dependencies to be ready..."
if [ -n "${REDIS_HOST}" ] && [ -n "${REDIS_PORT}" ]; then
    echo "  Waiting for Redis at ${REDIS_HOST}:${REDIS_PORT}..."
    timeout 30 bash -c 'until nc -z $0 $1; do sleep 1; done' "${REDIS_HOST}" "${REDIS_PORT}" 2>/dev/null || {
        echo "Redis is not ready after 30 seconds"
        exit 1
    }
    echo "  Redis is ready"
fi

if [ -n "${MYSQL_HOST}" ] && [ -n "${MYSQL_PORT}" ]; then
    echo "  Waiting for MySQL at ${MYSQL_HOST}:${MYSQL_PORT}..."
    timeout 60 bash -c 'until nc -z $0 $1; do sleep 1; done' "${MYSQL_HOST}" "${MYSQL_PORT}" 2>/dev/null || {
        echo "MySQL is not ready after 60 seconds"
        exit 1
    }
    echo "  MySQL is ready"
fi

if [ -n "${HTTP_MOCK_HOST}" ] && [ -n "${HTTP_MOCK_PORT}" ]; then
    echo "  Waiting for HTTP Mock at ${HTTP_MOCK_HOST}:${HTTP_MOCK_PORT}..."
    timeout 30 bash -c 'until nc -z $0 $1; do sleep 1; done' "${HTTP_MOCK_HOST}" "${HTTP_MOCK_PORT}" 2>/dev/null || {
        echo "HTTP Mock is not ready after 30 seconds"
        exit 1
    }
    echo "  HTTP Mock is ready"
fi

echo "All dependencies are ready"
echo ""

# 运行测试
echo "Running integration tests..."
echo ""

TEST_EXECUTABLE="./integration_test"
if [ ! -f "${TEST_EXECUTABLE}" ]; then
    echo "Error: Test executable not found at ${TEST_EXECUTABLE}"
    echo "Available files in current directory:"
    ls -la
    exit 1
fi

# 设置测试参数
TEST_ARGS="--gtest_output=xml:${TEST_REPORT_DIR}/test-results.xml"
TEST_ARGS="${TEST_ARGS} --gtest_color=yes"
TEST_ARGS="${TEST_ARGS} --gtest_print_time=1"

if [ "${LOG_LEVEL}" = "DEBUG" ]; then
    TEST_ARGS="${TEST_ARGS} --gtest_verbose=1"
fi

# 执行测试
echo "Test command: ${TEST_EXECUTABLE} ${TEST_ARGS}"
echo ""

${TEST_EXECUTABLE} ${TEST_ARGS}
TEST_EXIT_CODE=$?

echo ""
echo "Test execution completed with exit code: ${TEST_EXIT_CODE}"
echo ""

# 生成测试报告
if [ "${GENERATE_HTML_REPORT}" = "true" ]; then
    echo "Generating HTML test report..."
    
    # 检查是否安装了 gtest2html
    if command -v gtest2html >/dev/null 2>&1; then
        gtest2html "${TEST_REPORT_DIR}/test-results.xml" -o "${TEST_REPORT_DIR}/test-report.html"
        echo "  Test report generated: ${TEST_REPORT_DIR}/test-report.html"
    else
        echo "  Warning: gtest2html not installed, skipping HTML report generation"
        echo "  Install with: pip3 install gtest2html"
    fi
    
    # 生成覆盖率报告（如果启用了覆盖率编译）
    if [ -f "integration_test.gcda" ]; then
        echo "Generating coverage report..."
        
        if command -v gcovr >/dev/null 2>&1; then
            # 生成 HTML 覆盖率报告
            gcovr -r /app --html --html-details -o "${TEST_REPORT_DIR}/coverage-report.html"
            echo "  Coverage report generated: ${TEST_REPORT_DIR}/coverage-report.html"
            
            # 生成 XML 覆盖率报告（用于 CI）
            gcovr -r /app --xml -o "${TEST_REPORT_DIR}/coverage.xml"
            echo "  Coverage XML generated: ${TEST_REPORT_DIR}/coverage.xml"
            
            # 生成文本摘要
            gcovr -r /app > "${TEST_REPORT_DIR}/coverage-summary.txt"
            echo "  Coverage summary: ${TEST_REPORT_DIR}/coverage-summary.txt"
        else
            echo "  Warning: gcovr not installed, skipping coverage report generation"
            echo "  Install with: pip3 install gcovr"
        fi
    fi
fi

# 生成测试摘要
echo ""
echo "=========================================="
echo "Test Summary"
echo "=========================================="
echo "End time: $(date)"
echo "Exit code: ${TEST_EXIT_CODE}"

if [ -f "${TEST_REPORT_DIR}/test-results.xml" ]; then
    # 解析测试结果XML
    if command -v python3 >/dev/null 2>&1; then
        python3 -c "
import xml.etree.ElementTree as ET
import sys

try:
    tree = ET.parse('${TEST_REPORT_DIR}/test-results.xml')
    root = tree.getroot()
    
    tests = int(root.attrib.get('tests', 0))
    failures = int(root.attrib.get('failures', 0))
    errors = int(root.attrib.get('errors', 0))
    skipped = int(root.attrib.get('skipped', 0))
    time = float(root.attrib.get('time', 0))
    
    print(f'Total tests: {tests}')
    print(f'Failures: {failures}')
    print(f'Errors: {errors}')
    print(f'Skipped: {skipped}')
    print(f'Time: {time:.2f} seconds')
    
    if failures == 0 and errors == 0:
        print('Result: PASS')
    else:
        print('Result: FAIL')
        
except Exception as e:
    print(f'Error parsing test results: {e}')
    sys.exit(1)
"
    fi
fi

echo ""
echo "Test reports available in: ${TEST_REPORT_DIR}"
ls -la "${TEST_REPORT_DIR}/" 2>/dev/null || echo "No reports generated"

# 清理测试数据
if [ "${CLEANUP_DATA}" = "true" ]; then
    echo ""
    echo "Cleaning up test data..."
    # 这里可以添加清理Redis、MySQL等测试数据的逻辑
    echo "Test data cleanup completed"
fi

echo ""
echo "=========================================="

# 返回测试退出码
exit ${TEST_EXIT_CODE}