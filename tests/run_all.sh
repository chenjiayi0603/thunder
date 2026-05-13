#!/usr/bin/env bash
# Thunder 一键测试入口
#
# 用法:
#   ./tests/run_all.sh                        # 全部测试 (unit + e2e)
#   ./tests/run_all.sh unit                   # 仅单元测试 (零外部依赖, 14s)
#   ./tests/run_all.sh e2e                    # 仅端到端测试 (需 Docker)
#   ./tests/run_all.sh bench                  # 仅性能测试 (需 wrk + HelloHttp)
#   MODE=external ./tests/run_all.sh e2e      # e2e external 模式
#   ./tests/run_all.sh build                  # 全量构建 + 测试
#   ./tests/run_all.sh fast                   # 快速模式 (跳过耗时测试)
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TESTS_DIR="${REPO_ROOT}/tests"

print_banner() {
    echo ""
    echo "=============================================="
    echo "  Thunder Test Suite"
    echo "  $(date '+%Y-%m-%d %H:%M:%S')"
    echo "=============================================="
}

run_unit() {
    echo ""
    echo "--- [Unit Tests] Python 单元测试 (零外部依赖) ---"
    cd "${TESTS_DIR}"
    python3 -m pytest unit/ -v --tb=short
    echo "--- 单元测试完成 ---"
}

run_e2e() {
    local mode="${MODE:-local}"
    local pytest_expr="integration or smoke"
    
    echo ""
    echo "--- [E2E Tests] 端到端集成测试 (mode=${mode}) ---"
    
    if [[ "${mode}" == "local" ]]; then
        echo "需要 Docker 栈, 正在检查..."
        command -v docker >/dev/null 2>&1 || {
            echo "错误: 未找到 docker, local 模式需要 Docker" >&2
            exit 1
        }
    fi
    
    cd "${TESTS_DIR}"
    local args=("-v" "-s" "-m" "${pytest_expr}" "--mode=${mode}")
    if [[ "${KEEP_DOCKER:-0}" == "1" ]]; then
        args+=("--keep-docker")
    fi
    
    python3 -m pytest e2e/ "${args[@]}"
    echo "--- E2E 测试完成 ---"
}

run_bench() {
    echo ""
    echo "--- [Benchmark] 性能基准测试 ---"
    local bench_script="${TESTS_DIR}/benchmark/run_quick_bench.sh"
    
    if [[ ! -x "${bench_script}" ]]; then
        echo "跳过: 基准测试脚本不存在或不可执行 (需 wrk 工具)" >&2
        return 0
    fi
    
    command -v wrk >/dev/null 2>&1 || {
        echo "跳过: 未安装 wrk, 无法运行性能测试" >&2
        echo "安装: sudo apt install wrk" >&2
        return 0
    }
    
    cd "${TESTS_DIR}/benchmark"
    bash "${bench_script}"
    echo "--- 性能测试完成 ---"
}

run_build() {
    echo ""
    echo "--- [Build] 全量构建 + 安装 ---"
    cd "${REPO_ROOT}"
    
    # 检查子模块
    if [[ ! -f code/3party/libev/ev.c ]]; then
        echo "初始化 git 子模块..."
        git submodule update --init --recursive
    fi
    
    # 构建
    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build --target thirdparty_deploy -j1
    cmake --build build -j1
    cmake --install build
    echo "--- 构建完成 ---"
}

# ---- main ----
print_banner

case "${1:-all}" in
    unit)
        run_unit
        ;;
    e2e)
        run_e2e
        ;;
    bench)
        run_bench
        ;;
    build)
        run_build
        run_unit
        run_e2e
        ;;
    fast)
        run_unit
        echo "快速模式: 跳过 E2E 和性能测试"
        ;;
    all|*)
        run_unit
        run_e2e
        ;;
esac

echo ""
echo "=============================================="
echo "  全部测试完成"
echo "=============================================="
