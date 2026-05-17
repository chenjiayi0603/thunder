#!/usr/bin/env bash
# =============================================================================
# Thunder 一键测试入口 — 覆盖单元测试、端到端测试、性能基准、构建
# =============================================================================
#
# 测试分层:
#   unit   - Python 单元测试，零外部依赖，秒级完成
#            test_conhash.py        一致性哈希算法
#            test_iobackend_behavior.py  IoBackend 行为契约
#            test_json_parse.py      JSON 解析
#            test_token_verify.py    Token 验证
#            test_websocket_key.py   WebSocket 密钥
#   e2e    - 端到端集成测试，需 Docker 栈（hello/http/ws/https/center）
#            test_http_hello.py     HTTP Hello 服务
#            test_https_hello.py    HTTPS Hello 服务
#            test_ws_hello.py       WebSocket Hello 服务
#            test_center_admin.py   Center 管理接口
#            test_interface_chain.py Interface→Logic 链路
#            test_multicenter_raft.py Raft 多中心
#            test_stress.py         压力测试
#            test_wrk_smoke.py      wrk 冒烟
#   bench  - 性能基准（需 wrk），按后端对比 RPS/延迟
#   build  - 全量 CMake 构建 + 安装（-j1 强制，避免子模块竞态）
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
#   MODE=external ./tests/run_all.sh e2e      # e2e external 模式（远程栈）
#   KEEP_DOCKER=1 ./tests/run_all.sh e2e      # 测试后保留 Docker 容器
#
# 环境变量:
#   MODE          - e2e 模式: local(默认, 需 Docker) | external(远程栈)
#   KEEP_DOCKER   - e2e 后是否保留容器: 0(默认, 销毁) | 1(保留)
#
# 前置依赖:
#   unit:  python3, pytest
#   e2e:   python3, pytest, requests, docker
#   bench: wrk
#   build: cmake, make, gcc, g++
# =============================================================================
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

# 自动检测 OpenSSL 安装路径（cmake FindOpenSSL 对非标准路径的支持参差不齐）
detect_openssl() {
    # 优先用用户传入的 OPENSSL_ROOT_DIR
    if [[ -n "${OPENSSL_ROOT_DIR:-}" ]] && [[ -f "${OPENSSL_ROOT_DIR}/include/openssl/ssl.h" ]]; then
        echo "OpenSSL: 使用 OPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}"
        CMAKE_OPENSSL_ARGS=(
            "-DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}"
            "-DOPENSSL_CRYPTO_LIBRARY=${OPENSSL_ROOT_DIR}/lib/libcrypto.so"
            "-DOPENSSL_SSL_LIBRARY=${OPENSSL_ROOT_DIR}/lib/libssl.so"
        )
        return 0
    fi

    # 常见非标准路径
    local search_paths=(
        "$HOME/local/openssl-dev/usr"
        "/opt/openssl"
        "/usr/local/opt/openssl"
        "/usr/local"
    )

    for prefix in "${search_paths[@]}"; do
        if [[ -f "${prefix}/include/openssl/ssl.h" ]]; then
            # 确定 lib 子目录（可能是 lib/ 或 lib/x86_64-linux-gnu/）
            local libdir="${prefix}/lib"
            [[ -d "${prefix}/lib/x86_64-linux-gnu" ]] && libdir="${prefix}/lib/x86_64-linux-gnu"
            local crypto_lib="${libdir}/libcrypto.so"
            local ssl_lib="${libdir}/libssl.so"
            # so 找不到则尝试 .a
            [[ -f "${crypto_lib}" ]] || crypto_lib="${libdir}/libcrypto.a"
            [[ -f "${ssl_lib}" ]] || ssl_lib="${libdir}/libssl.a"

            if [[ -f "${crypto_lib}" ]] && [[ -f "${ssl_lib}" ]]; then
                echo "OpenSSL: 自动检测 ${prefix} (${libdir})"
                CMAKE_OPENSSL_ARGS=(
                    "-DOPENSSL_ROOT_DIR=${prefix}"
                    "-DOPENSSL_CRYPTO_LIBRARY=${crypto_lib}"
                    "-DOPENSSL_SSL_LIBRARY=${ssl_lib}"
                )
                return 0
            fi
        fi
    done

    # 系统默认路径
    if [[ -f "/usr/include/openssl/ssl.h" ]]; then
        echo "OpenSSL: 使用系统默认路径"
        CMAKE_OPENSSL_ARGS=()
        return 0
    fi

    echo "警告: 未找到 OpenSSL，cmake 可能失败。请设置 OPENSSL_ROOT_DIR 环境变量。" >&2
    CMAKE_OPENSSL_ARGS=()
    return 1
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
    detect_openssl

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo "${CMAKE_OPENSSL_ARGS[@]}"

    # thirdparty_deploy: 初次构建或第三方库变更时需要；已部署时跳过避免 absl RPATH 重写报错
    if cmake --build build --target thirdparty_deploy -j1 2>&1; then
        echo "thirdparty_deploy 完成"
    else
        echo "thirdparty_deploy 失败（可能已部署），继续主构建..."
    fi

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
