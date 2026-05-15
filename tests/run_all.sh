#!/usr/bin/env bash
# Thunder 一键测试入口
#
# 用法:
#   ./tests/run_all.sh                        # 全部测试 (unit + e2e)
#   ./tests/run_all.sh unit                   # 仅单元测试 (零外部依赖, ~14s)
#   ./tests/run_all.sh e2e                    # 仅端到端测试 (需 Docker)
#   ./tests/run_all.sh bench                  # 仅性能测试 (需 wrk)
#   ./tests/run_all.sh build                  # 仅构建 + 安装 (不跑测试)
#   ./tests/run_all.sh build+test             # 构建 + 安装 + 全部测试
#   ./tests/run_all.sh fast                   # 快速模式: 仅单元测试 (跳过 E2E)
#   ./tests/run_all.sh clean                  # 清理构建产物 (rm -rf build/)
#   MODE=external ./tests/run_all.sh e2e      # e2e external 模式
#   KEEP_DOCKER=1 ./tests/run_all.sh e2e      # 测试后保留 Docker 容器
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

check_prereqs() {
    local missing=()
    for cmd in cmake make gcc g++ docker; do
        command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
    done
    command -v pytest >/dev/null 2>&1 || missing+=("pytest (pip install pytest requests)")
    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "错误: 缺少依赖: ${missing[*]}" >&2
        exit 1
    fi
    if [[ ! -f "${REPO_ROOT}/code/3party/c-ares/CMakeLists.txt" ]]; then
        echo "第三方子模块未初始化，正在 git submodule update --init --recursive ..."
        git -C "${REPO_ROOT}" submodule update --init --recursive
    fi
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
    local args=("-v" "-s" "-m" "integration or smoke" "--mode=${mode}")
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
        echo "跳过: 基准测试脚本不存在或不可执行" >&2
        return 0
    fi

    command -v wrk >/dev/null 2>&1 || {
        echo "跳过: 未安装 wrk (sudo apt install wrk)" >&2
        return 0
    }

    bash "${bench_script}"
    echo "--- 性能测试完成 ---"
}

run_build() {
    echo ""
    echo "--- [Build] 全量构建 + 安装 ---"
    cd "${REPO_ROOT}"

    check_prereqs

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build --target thirdparty_deploy -j1
    cmake --build build -j1
    cmake --install build
    echo "--- 构建完成 ---"
}

run_clean() {
    echo ""
    echo "--- [Clean] 清理构建产物 ---"
    rm -rf "${REPO_ROOT}/build"
    echo "--- 清理完成 ---"
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
        ;;
    build+test)
        run_build
        run_unit
        run_e2e
        ;;
    fast)
        run_unit
        echo "快速模式: 跳过 E2E 和性能测试"
        ;;
    clean)
        run_clean
        ;;
    all|*)
        run_unit
        run_e2e
        ;;
esac

echo ""
echo "=============================================="
echo "  完成"
echo "=============================================="
