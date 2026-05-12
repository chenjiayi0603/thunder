#!/usr/bin/env bash
# =============================================================================
# Thunder 一键构建 + 集成测试
#
# 用法:
#   ./build_and_test.sh              # 完整构建 + 全部测试
#   ./build_and_test.sh build        # 仅构建（不测试）
#   ./build_and_test.sh test         # 仅测试（跳过构建）
#   ./build_and_test.sh fast         # 快速测试（-j$(nproc) 并行构建）
#   ./build_and_test.sh clean        # 清理构建产物
#
# 环境变量:
#   BUILD_JOBS=4 ./build_and_test.sh       # 自定义并行数
#   KEEP_DOCKER=1 ./build_and_test.sh      # 测试后保留 Docker 容器
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
TEST_DIR="${REPO_ROOT}/deploy/tests/pytest"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log()  { echo -e "${GREEN}[$(date +%H:%M:%S)]${NC} $*"; }
warn() { echo -e "${YELLOW}[$(date +%H:%M:%S)] WARN${NC} $*"; }
err()  { echo -e "${RED}[$(date +%H:%M:%S)] ERROR${NC} $*"; }

check_prereqs() {
    local missing=()
    for cmd in cmake make gcc g++ docker; do
        command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
    done
    command -v pytest >/dev/null 2>&1 || missing+=("pytest (pip install pytest requests)")

    if [[ ${#missing[@]} -gt 0 ]]; then
        err "缺少依赖: ${missing[*]}"
        exit 1
    fi
    # git submodule
    if [[ ! -f "${REPO_ROOT}/code/3party/c-ares/CMakeLists.txt" ]]; then
        warn "第三方子模块未初始化，正在 git submodule update --init --recursive ..."
        git -C "${REPO_ROOT}" submodule update --init --recursive
    fi
}

do_configure() {
    log "cmake 配置 (${BUILD_TYPE}) ..."
    cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
}

do_build() {
    log "cmake 构建 (-j${BUILD_JOBS}) ..."
    cmake --build "${BUILD_DIR}" -j "${BUILD_JOBS}"
}

do_install() {
    log "cmake install → deploy/ ..."
    cmake --install "${BUILD_DIR}"
}

do_clean() {
    log "清理构建产物 ..."
    rm -rf "${BUILD_DIR}"
    log "清理完成"
}

do_test() {
    log "运行集成测试 ..."
    cd "${TEST_DIR}"
    local args=("-v" "-s" "-m" "integration or smoke" "--mode=local")
    if [[ "${KEEP_DOCKER:-0}" == "1" ]]; then
        args+=("--keep-docker")
    fi
    pytest "${args[@]}"
    log "测试完成"
}

do_build_and_test() {
    local start_ts
    start_ts=$(date +%s)

    check_prereqs
    do_configure
    do_build
    do_install
    do_test

    local end_ts
    end_ts=$(date +%s)
    log "总耗时: $(( (end_ts - start_ts) / 60 )) 分 $(( (end_ts - start_ts) % 60 )) 秒"
}

main() {
    cd "${REPO_ROOT}"
    case "${1:-all}" in
        all|build+test)
            do_build_and_test
            ;;
        build)
            check_prereqs
            do_configure
            do_build
            do_install
            log "构建完成，产物在 deploy/ 目录"
            ;;
        test)
            do_test
            ;;
        fast)
            BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
            do_build_and_test
            ;;
        clean)
            do_clean
            ;;
        -h|--help|help)
            sed -n '2,20p' "$0" | grep -v '^#'
            ;;
        *)
            err "未知命令: ${1:-}"
            sed -n '5,10p' "$0"
            exit 1
            ;;
    esac
}

main "$@"
