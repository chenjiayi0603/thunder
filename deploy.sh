#!/usr/bin/env bash
###############################################################################
# Thunder — 一键部署与测试 Skill
#
# === 速度层级（从快到慢）===
#   ./deploy.sh test unit      最快   C++ gtest + Python unit   ~45s (零外部依赖)
#   ./deploy.sh test           默认   unit + Docker E2E          ~3min
#   ./deploy.sh test e2e       E2E    Docker 集成测试             ~3min
#   ./deploy.sh test bench     性能   wrk 压测                   ~1min
#
# === 常用命令 ===
#   ./deploy.sh build           cmake configure + build + install
#   ./deploy.sh test unit       C++ 单元测试 + Python 单元测试
#   ./deploy.sh test e2e        Docker 集成测试
#   ./deploy.sh test            全部测试 (unit + e2e)
#   ./deploy.sh up              启动 Docker 开发环境
#   ./deploy.sh down            停止并清理 Docker 环境
#   ./deploy.sh restart         重启 Docker 栈
#   ./deploy.sh status          查看服务状态
#   ./deploy.sh clean           清理 build/ + Docker + tmp
#
# === 选项 ===
#   --force          强制全量构建
#   --skip-build     跳过 cmake 构建
#   --keep-docker    E2E 后保留容器
#   --verbose        详细输出
###############################################################################
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"
BUILD_JOBS="${BUILD_JOBS:-1}"
DOCKER_DIR="${PROJECT_DIR}/docker"
TESTS_DIR="${PROJECT_DIR}/tests"

# ─── 颜色 ───────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
log()  { echo -e "${BLUE}▸${NC} $*"; }
ok()   { echo -e "${GREEN}✔${NC} $*"; }
warn() { echo -e "${YELLOW}⚠${NC} $*"; }
err()  { echo -e "${RED}✘${NC} $*"; }

# ─── 参数解析 ───────────────────────────────────
CMD="${1:-help}"; shift || true
FORCE=false; SKIP_BUILD=false; KEEP_DOCKER=false; VERBOSE=false; MODE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --force) FORCE=true ;;
        --skip-build) SKIP_BUILD=true ;;
        --keep-docker) KEEP_DOCKER=true ;;
        --verbose) VERBOSE=true ;;
        --help|-h) CMD="help"; break ;;
        unit|e2e|bench) MODE="$1" ;;
        *) warn "未知参数: $1"; exit 1 ;;
    esac
    shift
done

# ─── 帮助 ───────────────────────────────────────
show_help() {
    echo "Usage: ./deploy.sh <command> [options]"
    echo ""
    echo "Commands:"
    echo "  build         cmake configure + build + install"
    echo "  test          全部测试 (unit + e2e)"
    echo "  test unit     C++ + Python 单元测试 (零外部依赖, ~45s)"
    echo "  test e2e      Docker 集成测试 (~3min)"
    echo "  test bench    wrk 性能测试"
    echo "  up            启动 Docker 开发环境"
    echo "  down          停止 Docker 环境并清理"
    echo "  restart       重启 Docker 栈"
    echo "  status        查看服务状态"
    echo "  clean         清理 build/ + Docker + tmp"
    echo ""
    echo "Options:"
    echo "  --force       强制全量构建"
    echo "  --skip-build  跳过 cmake 构建"
    echo "  --keep-docker E2E 后保留容器"
    echo "  --verbose     详细输出"
    echo ""
    echo "Speed tiers (fast → slow):"
    echo "  test unit      ~45s  零外部依赖"
    echo "  test e2e       ~3min 需 Docker"
    echo "  test           ~4min unit + e2e"
}

# ─── 依赖检查 ───────────────────────────────────
check_prereqs() {
    local missing=()
    for cmd in cmake make gcc g++; do
        command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
    done
    if [[ ${#missing[@]} -gt 0 ]]; then
        err "缺少依赖: ${missing[*]}"
        exit 1
    fi
    if [[ ! -f "${PROJECT_DIR}/code/3party/c-ares/CMakeLists.txt" ]]; then
        log "第三方子模块未初始化，正在 git submodule update --init --recursive ..."
        git -C "${PROJECT_DIR}" submodule update --init --recursive
    fi
}

# ─── OpenSSL 检测 ───────────────────────────────
detect_openssl() {
    if [[ -n "${OPENSSL_ROOT_DIR:-}" ]] && [[ -f "${OPENSSL_ROOT_DIR}/include/openssl/ssl.h" ]]; then
        CMAKE_OPENSSL_ARGS=("-DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}")
        return 0
    fi
    CMAKE_OPENSSL_ARGS=()
    return 0
}

# ─── Build ──────────────────────────────────────
cmd_build() {
    echo ""
    echo -e "${BOLD}=== [1/1] Build ===${NC}"
    cd "${PROJECT_DIR}"

    check_prereqs
    detect_openssl

    log "cmake configure (${BUILD_DIR}, ${CMAKE_BUILD_TYPE})..."
    cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
        -DTHUNDER_BUILD_TESTS=ON \
        "${CMAKE_OPENSSL_ARGS[@]}"

    log "thirdparty_deploy..."
    cmake --build "${BUILD_DIR}" --target thirdparty_deploy -j"${BUILD_JOBS}" 2>/dev/null || \
        warn "thirdparty_deploy 跳过（可能已部署）"

    log "cmake --build (-j${BUILD_JOBS})..."
    cmake --build "${BUILD_DIR}" -j"${BUILD_JOBS}" || {
        err "编译失败"
        exit 1
    }

    log "cmake --install..."
    cmake --install "${BUILD_DIR}" || {
        err "安装失败"
        exit 1
    }

    ok "Build 完成"
}

# ─── C++ Unit Tests ─────────────────────────────
run_cpp_unit() {
    echo ""
    echo -e "${BOLD}--- [C++ Unit Tests] gtest ---${NC}"
    local ctest_dir="${BUILD_DIR}/code/test"
    if [[ ! -d "${ctest_dir}" ]]; then
        warn "C++ 测试目录不存在 (${ctest_dir})，跳过"
        return 0
    fi

    cd "${ctest_dir}"
    # 汇总所有测试到一次 ctest 运行
    local ctest_out
    ctest_out=$(ctest -j"${BUILD_JOBS}" --output-on-failure 2>&1) || true

    # 统计
    local passed=$(echo "${ctest_out}" | grep -c "tests passed" || echo "0")
    local failed=$(echo "${ctest_out}" | grep -c "tests failed" || echo "0")
    echo "${ctest_out}" | tail -10

    if echo "${ctest_out}" | grep -q "100% tests passed"; then
        ok "C++ 单元测试: 全部通过"
    else
        warn "C++ 单元测试: 有失败项, 详见上方输出"
    fi
}

# ─── Python Unit Tests ──────────────────────────
run_py_unit() {
    echo ""
    echo -e "${BOLD}--- [Python Unit Tests] pytest ---${NC}"
    cd "${TESTS_DIR}"
    if command -v pytest >/dev/null 2>&1; then
        python3 -m pytest unit/ -v --tb=short 2>&1 || {
            warn "Python 单元测试有失败"
            return 1
        }
        ok "Python 单元测试: 全部通过"
    else
        warn "pytest 未安装, 跳过 Python 测试 (pip install pytest requests)"
    fi
}

# ─── E2E Tests ──────────────────────────────────
cmd_test_e2e() {
    echo ""
    echo -e "${BOLD}=== E2E Integration Tests ===${NC}"

    command -v docker >/dev/null 2>&1 || {
        err "未找到 docker, E2E 需要 Docker"
        exit 1
    }
    command -v pytest >/dev/null 2>&1 || {
        err "未找到 pytest, 请: pip install pytest requests"
        exit 1
    }

    # 1. Docker down (清场)
    log "docker compose down (清场)..."
    ( cd "${DOCKER_DIR}" && docker compose down --remove-orphans 2>/dev/null ) || true

    # 1b. 清 etcd bind-mount 残留 — 保证 E2E hermetic。
    #     etcd registry/slot 跨运行累积会触发"no slot available"等假故障 (issus #11)。
    #     只清 etcd: mariadb 清了要重新初始化(慢), redis 是缓存无需清。
    log "清理 etcd bind-mount (hermetic)..."
    rm -rf "${DOCKER_DIR}/data/etcd"/* 2>/dev/null || true

    # 2. Docker build
    log "docker compose build..."
    ( cd "${DOCKER_DIR}" && docker compose build ) || {
        err "Docker build 失败"
        exit 1
    }

    # 3. Docker up
    log "docker compose up -d..."
    ( cd "${DOCKER_DIR}" && docker compose up -d ) || {
        err "Docker up 失败"
        exit 1
    }

    # 4. Wait for services
    log "等待服务就绪 (最长 120s)..."
    local deadline=$(($(date +%s) + 120))
    local ready=false
    while [[ $(date +%s) -lt ${deadline} ]]; do
        if ss -tln 2>/dev/null | grep -q ':27000 ' && \
           ss -tln 2>/dev/null | grep -q ':27006 ' && \
           ss -tln 2>/dev/null | grep -q ':27008 '; then
            ready=true
            break
        fi
        sleep 2
    done

    if [[ "${ready}" != "true" ]]; then
        warn "部分端口未就绪, 继续尝试测试..."
    fi

    # 5. Run E2E tests
    echo ""
    echo -e "${BOLD}--- [E2E Tests] pytest ---${NC}"
    local e2e_result=0
    cd "${TESTS_DIR}"
    python3 -m pytest e2e/ -v -s --tb=short -m "integration or smoke" 2>&1 || e2e_result=$?

    # 6. Cleanup
    if [[ "${KEEP_DOCKER}" != "true" ]]; then
        log "docker compose down (清理)..."
        ( cd "${DOCKER_DIR}" && docker compose down --remove-orphans 2>/dev/null ) || true
        docker system prune -f 2>/dev/null || true
    else
        log "保留 Docker 容器 (--keep-docker)"
    fi

    if [[ ${e2e_result} -eq 0 ]]; then
        ok "E2E 测试: 全部通过"
    else
        warn "E2E 测试: 有失败项"
        return ${e2e_result}
    fi
}

# ─── Bench ──────────────────────────────────────
cmd_bench() {
    echo ""
    echo -e "${BOLD}--- [Benchmark] wrk 性能测试 ---${NC}"
    local bench_script="${TESTS_DIR}/benchmark/run_quick_bench.sh"
    if [[ -x "${bench_script}" ]]; then
        bash "${bench_script}"
    else
        warn "benchmark 脚本不存在或不可执行"
    fi
}

# ─── Up / Down / Restart / Status / Clean ───────
cmd_up() {
    echo ""
    log "docker compose up -d (开发环境)..."
    cd "${DOCKER_DIR}"
    docker compose up -d
    ok "Docker 环境已启动"
}

cmd_down() {
    echo ""
    log "docker compose down..."
    cd "${DOCKER_DIR}"
    docker compose down --remove-orphans 2>/dev/null || true
    docker system prune -f 2>/dev/null || true
    ok "Docker 环境已清理"
}

cmd_restart() {
    echo ""
    log "重启 Docker 栈..."
    cd "${DOCKER_DIR}"
    docker compose down --remove-orphans 2>/dev/null || true
    docker compose up -d
    ok "Docker 环境已重启"
}

cmd_status() {
    echo ""
    echo -e "${BOLD}=== Docker 容器状态 ===${NC}"
    cd "${DOCKER_DIR}"
    docker compose ps -a 2>/dev/null || true
    echo ""
    echo -e "${BOLD}=== 监听端口 ===${NC}"
    ss -tln 2>/dev/null | grep -E ':(27000|27006|27008|27010|27443|16068|6379|3306)\s' || echo "(无)"
}

cmd_clean() {
    echo ""
    log "清理 build/ ..."
    rm -rf "${BUILD_DIR}"
    ok "build/ 已清理"

    log "清理 Docker ..."
    ( cd "${DOCKER_DIR}" && docker compose down --remove-orphans 2>/dev/null ) || true
    docker system prune -f 2>/dev/null || true
    ok "Docker 已清理"

    log "清理临时文件 ..."
    rm -f /tmp/asio_uring_diag.log 2>/dev/null || true
    rm -rf /tmp/e2e-* /tmp/stress-* 2>/dev/null || true
    ok "临时文件已清理"
}

# ─── Main ───────────────────────────────────────
echo ""
echo -e "${BOLD}==============================================${NC}"
echo -e "${BOLD}  Thunder Deploy  $(date '+%Y-%m-%d %H:%M:%S')${NC}"
echo -e "${BOLD}==============================================${NC}"

case "${CMD}" in
    help|--help|-h)
        show_help
        ;;
    build)
        cmd_build
        ;;
    test)
        # 如果不需要 E2E, 先 build
        if [[ "${SKIP_BUILD}" != "true" ]]; then
            cmd_build
        fi

        case "${MODE}" in
            unit)
                run_cpp_unit
                run_py_unit
                ;;
            e2e)
                cmd_test_e2e
                ;;
            bench)
                cmd_bench
                ;;
            *)
                # 全部: unit + e2e
                run_cpp_unit
                run_py_unit
                cmd_test_e2e
                ;;
        esac
        ;;
    up)
        cmd_up
        ;;
    down)
        cmd_down
        ;;
    restart)
        cmd_restart
        ;;
    status)
        cmd_status
        ;;
    clean)
        cmd_clean
        ;;
    *)
        echo "未知命令: ${CMD}" >&2
        show_help
        exit 1
        ;;
esac

echo ""
echo -e "${BOLD}==============================================${NC}"
echo -e "${BOLD}  完成${NC}"
echo -e "${BOLD}==============================================${NC}"
