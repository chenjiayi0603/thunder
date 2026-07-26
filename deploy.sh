#!/usr/bin/env bash
###############################################################################
# Thunder — 一键部署与测试 Skill
#
# === 速度层级（从快到慢）===
#   ./deploy.sh test unit      最快   C++ gtest + Python unit   ~45s (零外部依赖)
#   ./deploy.sh test           默认   unit + Docker E2E          ~3min
#   ./deploy.sh test e2e       E2E    Docker 集成测试             ~3min
#   ./deploy.sh test smoke     冒烟   核心链路 + etcd 注册中心     ~5s (需集群)
#   ./deploy.sh test regression 回归   全量自动化回归 (提交前必跑) ~5min
#   ./deploy.sh test k8s       K8s    K8s 全量回归 (含 fd 迁移/优雅重启) ~10min
#   ./deploy.sh test perf      性能   wrk 压测                   ~1min
#
# === 一键全量 ===
#   ./tests/run_all.sh          Docker Compose 环境: 构建 → 部署 → 全部测试
#                               (unit + gtest + lua + smoke + graceful restart,
#                                推荐提交前使用, 需 Docker, 不需要 K8s 集群)
#
# === 常用命令 ===
#   ./deploy.sh build           cmake configure + build + install
#   ./deploy.sh test unit       C++ 单元测试 + Python 单元测试
#   ./deploy.sh test e2e        Docker 集成测试
#   ./deploy.sh test smoke      冒烟测试 (核心链路 + etcd 注册中心)
#   ./deploy.sh test regression 全量回归 (提交前必跑)
#   ./deploy.sh test perf       wrk 性能测试
#   ./deploy.sh test            全部测试 (unit + e2e)
#   ./deploy.sh up              启动 Docker 开发环境
#   ./deploy.sh down            停止并清理 Docker 环境
#   ./deploy.sh restart         重启 Docker 栈
#   ./deploy.sh status          查看服务状态
#   ./deploy.sh admin nodes     查看 etcd 在线节点 (等效 admin_nodes.py)
#   ./deploy.sh admin status    查看 etcd 集群健康 (等效 admin_status.sh)
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
log()   { echo -e "${BLUE}▸${NC} $*"; }
ok()    { echo -e "${GREEN}✔${NC} $*"; }
warn()  { echo -e "${YELLOW}⚠${NC} $*"; }
error() { echo -e "${RED}✘${NC} $*"; }
err()  { echo -e "${RED}✘${NC} $*"; }

# ─── 状态文件 & 增量部署支持 (#166) ──────────────────
STATE_DIR="${PROJECT_DIR}/.deploy-state"
mkdir -p "$STATE_DIR"
STATE_K8S_OK="${STATE_DIR}/test-k8s-last-ok"     # 记录上次全量通过的 commit
STATE_DIGESTS="${STATE_DIR}/image-digests"       # 各服务 image digest

# 核心测试端口
TEST_PORTS=(27006 27008 27010 27012 27443 21883 30090 31883)

# ── 环境自愈: 清除非 K8s 进程占用的测试端口 ──
_self_heal() {
    local killed=0
    set +e  # 自愈过程不允许失败中断
    for port in "${TEST_PORTS[@]}"; do
        local pid
        pid=$(timeout 3 ss -tlnp 2>/dev/null | grep ":${port} " | grep -oP 'pid=\K[0-9]+' | head -1 || echo "")
        [[ -z "$pid" ]] && continue
        # 区分 K8s Pod vs 原生进程
        if cat /proc/$pid/cgroup 2>/dev/null | grep -q "kubepods" 2>/dev/null; then
            continue
        fi
        warn "端口 $port 被非 K8s 进程 PID=$pid 占用, 清理中..."
        kill -9 $pid 2>/dev/null || true
        killed=$((killed + 1))
    done
    # 杀残留孤儿进程
    for name in Hello_robot Interface_robot MqttBroker_robot; do
        for p in $(pgrep -f "$name" 2>/dev/null || echo ""); do
            [[ -z "$p" ]] && continue
            cat /proc/$p/cgroup 2>/dev/null | grep -q "kubepods" 2>/dev/null && continue
            kill -9 $p 2>/dev/null || true
            killed=$((killed + 1))
        done
    done
    [[ $killed -gt 0 ]] && { sleep 1; ok "环境自愈: 已清理 $killed 个冲突"; }
    set -e
    return 0
}

# ── 资源余量检查 ──
_check_resources() {
    local mem_avail disk_avail
    mem_avail=$(free -m | awk '/^Mem:/{print $7}')
    disk_avail=$(df -BG / | awk 'NR==2{print $4}' | tr -d 'G')
    echo "  内存: ${mem_avail}MB  磁盘: ${disk_avail}GB  CPU: $(nproc)核"
    [[ "$mem_avail" -lt 2048 ]] && warn "  ← 内存不足 2GB, make -j 可能 OOM"
    [[ "${disk_avail:-0}" -lt 5 ]] && warn "  ← 磁盘不足 5GB, docker build 可能失败"
}

# ── 增量构建检测: 返回需要构建的服务列表 ──
# 输出: "all" = 全量, "none" = 跳过, "hello interface logic mqtt admin" = 部分
_changed_services() {
    local last_ok state_file="$STATE_K8S_OK"
    if [[ ! -f "$state_file" ]] || $FORCE; then
        echo "all"
        return
    fi
    local last_commit; last_commit=$(cat "$state_file")
    local changed; changed=$(cd "$PROJECT_DIR" && git diff --name-only "$last_commit" HEAD 2>/dev/null || echo "")
    if [[ -z "$changed" ]]; then
        echo "none"
        return
    fi
    # 框架层变更 → 全量
    if echo "$changed" | grep -q "^code/Net/"; then
        echo "all"; return
    fi
    # 基础库变更 → 全量
    if echo "$changed" | grep -q "^code/Util/\|^code/3party/\|^CMakeLists.txt"; then
        echo "all"; return
    fi
    local svcs=""
    echo "$changed" | grep -q "^code/HelloHttp/\|^deploy/HelloHttp/\|^deploy/HelloHttps/\|^deploy/HelloWs/\|^deploy/HelloWss/" && svcs="$svcs hello"
    echo "$changed" | grep -q "^code/Interface/\|^deploy/Interface/" && svcs="$svcs interface"
    echo "$changed" | grep -q "^code/Logic/\|^deploy/Logic/" && svcs="$svcs logic"
    echo "$changed" | grep -q "^code/HelloMqttBroker/\|^deploy/MqttBroker/" && svcs="$svcs mqtt"
    echo "$changed" | grep -q "^deploy/admin-web/" && svcs="$svcs admin"
    # 如果只改了测试/文档/k8s yaml → 跳过构建
    local code_only
    code_only=$(echo "$changed" | grep -v "^tests/\|^k8s/\|^issus-list.md\|^deploy.sh\|^.deploy-state/" || echo "")
    if [[ -z "$code_only" ]] && [[ -z "$svcs" ]]; then
        echo "none"; return
    fi
    echo "${svcs:-all}"
}

# ── 智能镜像部署: 只导入有变化的镜像 ──
_smart_ctr_import() {
    local tag="$1" img ctr_ok=true
    mkdir -p /tmp/thunder-images
    for img in thunder-interface thunder-hello thunder-hello-https thunder-hello-ws \
               thunder-hello-wss thunder-logic thunder-mqtt thunder-admin-web; do
        local new_digest old_digest
        new_digest=$(docker image inspect "$img:${tag}" --format '{{.ID}}' 2>/dev/null || echo "")
        [[ -z "$new_digest" ]] && continue
        old_digest=$(grep "^$img " "$STATE_DIGESTS" 2>/dev/null | awk '{print $2}' || echo "")
        if [[ "$new_digest" == "$old_digest" ]]; then
            log "  $img:${tag} digest 未变化, 跳过"
            continue
        fi
        log "  $img:${tag} digest 已变化, 导入..."
        docker save "$img:${tag}" -o "/tmp/thunder-images/${img}.tar" 2>/dev/null || { warn "  $img save 失败"; ctr_ok=false; }
    done
    $ctr_ok || { rm -rf /tmp/thunder-images; return 1; }
    # 一次性导入全部
    docker run --rm --privileged --pid=host --network=host \
        -v /tmp/thunder-images:/tmp/thunder-images \
        alpine:latest nsenter -t 1 -m -u -n -i -p -- sh -c '
            for f in /tmp/thunder-images/*.tar; do
                ctr -n k8s.io image import "$f" || exit 1
            done
        ' || { rm -rf /tmp/thunder-images; return 1; }
    # 更新 digest 记录
    for img in thunder-interface thunder-hello thunder-hello-https thunder-hello-ws \
               thunder-hello-wss thunder-logic thunder-mqtt thunder-admin-web; do
        local d
        d=$(docker image inspect "$img:${tag}" --format '{{.ID}}' 2>/dev/null || echo "")
        [[ -n "$d" ]] && { grep -v "^$img " "$STATE_DIGESTS" 2>/dev/null > "${STATE_DIGESTS}.tmp" || true; echo "$img $d" >> "${STATE_DIGESTS}.tmp"; mv "${STATE_DIGESTS}.tmp" "$STATE_DIGESTS"; }
    done
    rm -rf /tmp/thunder-images
    return 0
}

# ─── 参数解析 ───────────────────────────────────
CMD="${1:-help}"; shift || true
_ADMIN_SUB="${1:-}"  # admin nodes/status/config 透传
_ADMIN_ARGS=("$@")   # save all remaining args before the option-parse loop shifts them
_EXTRA_ARGS=("$@")
FORCE=false; SKIP_BUILD=false; KEEP_DOCKER=false; VERBOSE=false; MODE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --force) FORCE=true ;;
        --skip-build) SKIP_BUILD=true ;;
        --keep-docker) KEEP_DOCKER=true ;;
        --verbose) VERBOSE=true ;;
        --help|-h) CMD="help"; break ;;
        unit|e2e|bench|smoke|regression|perf|all|k8s|compose) MODE="$1" ;;
        Hello_*|Logic_*|Interface_*|HelloWs_*|HelloHttps_*) MODE="$1" ;;
        	*) ;;  # pass-through for admin/extra args
    esac
    shift
done

# ─── 帮助 ───────────────────────────────────────
show_help() {
    echo "Usage: ./deploy.sh <command> [options]"
    echo ""
    echo "Commands:"
    echo "  build         cmake configure + build + install"
    echo "  build-so      构建 SO 镜像 (可指定类型: hello/logic/interface/all)"
    echo "  test          全部测试 (unit + e2e)"
    echo "  test unit     C++ + Python 单元测试 (零外部依赖, ~45s)"
    echo "  test e2e      Docker 集成测试 (~3min)"
    echo "  test smoke    冒烟测试 (核心链路 + etcd 注册中心, 需集群)"
    echo "  test regression 全量回归 (提交前必跑, ~5min)"
    echo "  test compose   Docker Compose 集成测试 (~3min)"
    echo "  test k8s       K8s 全量回归 (需集群, ~10min)"
    echo "  test perf/bench wrk 性能测试"
    echo "  up            启动 Docker 开发环境"
    echo "  down          停止 Docker 环境并清理"
    echo "  restart       重启 Docker 栈"
    echo "  status        查看服务状态"
    echo "  admin nodes   查看在线节点"
    echo "  admin routes  查看路由表"
    echo "  admin status  etcd 集群健康"
    echo "  admin config  查改配置"
    echo "  logs          查看所有节点最近日志 (需集群)"
    echo "  clean         清理 build/ + Docker + tmp"
    echo "  release       一键: cmake → image → 部署 → 回归"
    echo "  release k8s   K8s 版本 (需集群)"
    echo ""
    echo "Options:"
    echo "  --force       强制全量构建"
    echo "  --skip-build  跳过 cmake 构建"
    echo "  --keep-docker E2E 后保留容器"
    echo "  --verbose     详细输出"
    echo ""
    echo "Speed tiers (fast → slow):"
    echo "  test unit      ~45s  零外部依赖"
    echo "  test smoke     ~5s   需 Docker 集群"
    echo "  test e2e       ~3min 需 Docker"
    echo "  test regression ~5min 需 Docker (提交前必跑)"
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
        -DTHUNDER_BUILD_HELLO_PLUGINS=ON \
        -DTHUNDER_BUILD_NODE_PLUGINS=ON \
        ${GTEST_SRC_DIR:+-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="${GTEST_SRC_DIR}"} \
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

    # ─── 构建后校验: io_backend 与 cmake 编译选项一致性 ───
    _validate_io_backend

    ok "Build 完成"
}

_validate_io_backend() {
    local cmake_cache="${BUILD_DIR}/CMakeCache.txt"
    local asio_enabled=false
    if [[ -f "$cmake_cache" ]] && grep -q "ENABLE_ASIO_URING:BOOL=ON" "$cmake_cache" 2>/dev/null; then
        asio_enabled=true
    fi
    local mismatch=()
    while IFS=: read -r file line content; do
        if $asio_enabled; then
            # cmake 开了 asio_uring → 配置必须用 asio_uring, 不能用 ev
            if echo "$content" | grep -q '"io_backend": "ev"'; then
                mismatch+=("$file:$line 期望 asio_uring 实际 ev")
            fi
        else
            # cmake 没开 asio_uring → 配置不能用 asio_uring
            if echo "$content" | grep -q '"io_backend": "asio_uring"'; then
                mismatch+=("$file:$line 期望 ev 实际 asio_uring (二进制不支持)")
            fi
        fi
    done < <(grep -rn '"io_backend"' "${PROJECT_DIR}/deploy/" --include="*.json" 2>/dev/null)
    if [[ ${#mismatch[@]} -gt 0 ]]; then
        err "io_backend 配置与 cmake 编译选项不一致！"
        for m in "${mismatch[@]}"; do
            err "  $m"
        done
        err "修复: deploy.sh build 会自动修正, 或手动改 deploy/*/conf/*.json"
        # Auto-fix: align configs with cmake
        if $asio_enabled; then
            find "${PROJECT_DIR}/deploy/" -name "*.json" -exec sed -i 's/"io_backend": "ev"/"io_backend": "asio_uring"/' {} \;
        else
            find "${PROJECT_DIR}/deploy/" -name "*.json" -exec sed -i 's/"io_backend": "asio_uring"/"io_backend": "ev"/' {} \;
        fi
        ok "io_backend 已自动修正"
    fi
}

# ─── SO 镜像构建 ─────────────────────────────────
SO_IMAGE_DIR="${PROJECT_DIR}/so-images"

# 已知 SO 模块列表（so-images/ 不入 git，由此列表驱动）
# 格式: "ModuleDirName"  →  自动从 deploy/{ServicePrefix}/plugins/ 查找对应 .so
ALL_SO_MODULES=(
    HelloHttp_ModuleHello
    HelloHttp_ModuleRaw
    HelloHttps_ModuleHello
    HelloWs_CmdHello
    HelloWs_ModuleShake
    Interface_ModuleInterface
    Logic_CmdGetToken
)

# 从 deploy/ 找到指定模块的源 .so 路径（全名优先，回退到短名）
find_so_source() {
    local mod="$1"                             # e.g. Interface_ModuleInterface
    local svc="${mod%%_*}"                     # e.g. Interface
    local plugin_dir="${PROJECT_DIR}/deploy/${svc}/plugins"

    # 优先: 全名匹配 (如 HelloHttp_ModuleHello.so)
    if [[ -f "${plugin_dir}/${mod}.so" ]]; then
        echo "${plugin_dir}/${mod}.so"
        return
    fi
    # 回退: 短名匹配 (如 ModuleInterface.so, CmdGetToken.so)
    local short="${mod#*_}"
    if [[ -f "${plugin_dir}/${short}.so" ]]; then
        echo "${plugin_dir}/${short}.so"
        return
    fi
    echo ""
}

cmd_build_so() {
    local target="${MODE:-all}"

    if [[ "$target" == "all" ]]; then
        for mod in "${ALL_SO_MODULES[@]}"; do
            build_one_so_module "$mod"
        done
    else
        # 检查是否在已知列表里
        local found=0
        for mod in "${ALL_SO_MODULES[@]}"; do
            [[ "$mod" == "$target" ]] && { found=1; break; }
        done
        if [[ "$found" -eq 1 ]]; then
            build_one_so_module "$target"
        else
            warn "SO 模块不存在: ${target}  (已知模块: ${ALL_SO_MODULES[*]})"
        fi
    fi
    ok "SO 镜像构建完成"
}

build_one_so_module() {
    local mod="$1"
    local dir="${SO_IMAGE_DIR}/${mod}"
    local tag="so-${mod,,}:latest"
    local hash_file="${dir}/.so_hash"

    # 从 deploy/ 找源 .so，自动复制到 so-images/ 目录
    local src
    src=$(find_so_source "$mod")
    if [[ -z "$src" ]]; then
        warn "${mod}: 在 deploy/ 中未找到对应 .so，跳过（先跑 deploy.sh build）"
        return
    fi

    mkdir -p "$dir"
    cp -f "$src" "${dir}/${mod}.so"

    # 增量: SO 无变化则跳过
    local new_hash
    new_hash=$(sha256sum "${dir}/${mod}.so" | awk '{print $1}')
    local old_hash=""
    [[ -f "$hash_file" ]] && old_hash=$(cat "$hash_file")
    if [[ "$new_hash" == "$old_hash" ]] && docker image inspect "$tag" &>/dev/null; then
        echo "  ${tag} (无变化, 跳过)"
        return
    fi

    log "构建 SO 镜像: ${tag}  (来源: ${src})"

    cat > "${dir}/Dockerfile" << 'DOCKERFILE'
FROM alpine:3.20
COPY *.so /app/so/
CMD ["/bin/true"]
DOCKERFILE

    docker build -t "$tag" "$dir" || {
        err "${mod} 镜像构建失败"
        return
    }

    echo "$new_hash" > "$hash_file"
    local size
    size=$(docker image inspect "$tag" --format '{{.Size}}' 2>/dev/null)
    size=$(( size / 1024 / 1024 ))
    ok "  ${tag}  ${size}MB"
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

    # 1c. 端口冲突预检 — 检查 Thunder 关键内部端口是否被 k8s/k3s 进程占用
    #     使用 ss 而非 lsof，因为 lsof 无法看到 root 进程的 fd（非 root 用户）
    log "端口冲突预检 (27006/27007/27010/27011/27443/27444)..."
    local conflict_ports=()
    for port in 27006 27007 27010 27011 27443 27444; do
        if ss -tlnH 2>/dev/null | grep -qE ":${port}[[:space:]]"; then
            conflict_ports+=("${port}")
        fi
    done
    if [[ ${#conflict_ports[@]} -gt 0 ]]; then
        err "端口冲突：${conflict_ports[*]}"
        err "以上端口被占用（可能是 k3s/k8s Thunder pod），Docker Compose E2E 无法正确启动 Worker"
        err "请先 scale down 对应 k8s deployment，例如："
        err "  kubectl scale deployment thunder-hello --replicas=0 -n thunder"
        err "  kubectl scale deployment thunder-hello-ws --replicas=0 -n thunder"
        err "  kubectl scale deployment thunder-hello-https --replicas=0 -n thunder"
        err "待 E2E 完成后再 scale back to 1"
        exit 1
    fi

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
    python3 -m pytest e2e/ -v -s --tb=short -m "integration or smoke" --mode=external 2>&1 || e2e_result=$?

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

# ─── Smoke ───────────────────────────────────────
cmd_test_smoke() {
    local smoke_script="${PROJECT_DIR}/tests/test_smoke.sh"
    if [[ ! -r "${smoke_script}" ]]; then
        err "冒烟脚本不存在: ${smoke_script}"
        return 1
    fi
    echo "=== 冒烟测试 (核心链路 + etcd 注册中心) ==="
    if bash "${smoke_script}" "$@"; then
        ok "冒烟测试: 全部通过"
    else
        warn "冒烟测试: 有失败项"
        return 1
    fi
}

# ─── Regression ──────────────────────────────────
cmd_test_regression() {
    local reg_script="${PROJECT_DIR}/tests/regression.sh"
    if [[ ! -r "${reg_script}" ]]; then
        err "回归脚本不存在: ${reg_script}"
        return 1
    fi
    echo "=== 全量回归 (提交前必跑) ==="
    if bash "${reg_script}" "$@"; then
        ok "全量回归: 全部通过"
    else
        warn "全量回归: 有失败项"
        return 1
    fi
}

# ─── K8s 回归测试 (5 阶段: 预检→构建→部署→测试→清理) ───
# 用法: ./deploy.sh test k8s
#
# 阶段 0: PRE-CHECK  — 检查端口/僵尸Pod/资源余量，清理老实例
# 阶段 1: BUILD     — C++ cmake + 3 docker 镜像 (Interface/Hello/admin-web)
# 阶段 2: DEPLOY    — 清 etcd → 导入 containerd → rollout restart → 等 Ready
# 阶段 3: TEST      — regression-test.sh 36 项全量
# 阶段 4: CLEAN     — 删测试 artifacts/NFS残留/etcd测试键/审计记录
#
# === fd 迁移 / 优雅重启回归注意事项 ===
# 本测试覆盖 Worker 优雅重启 (GracefulRestartWorker) 的 fd 迁移路径:
#   1. Manager 通过共享内存原子标志 (pDrainMigrate) 通知旧 Worker 排空时迁移 fd
#      (替代旧 CMD_WORKER_DRAIN 消息, 见 WorkerAttr.hpp / SpawnSingleWorker)
#   2. 旧 Worker 收到 SIGTERM → EnterDrainMode() → 不接受新请求, 等待 20s
#   3. 排空完成或超时后 TransferAllFds() 一次性迁移所有客户端 fd 给新 Worker
#      (不再做部分迁移, 避免 StepNode/StepCo20 等跨节点 Step 上下文被破坏)
#   4. Manager RecvFdFromWorker() 接收 fd → 转发给新 Worker → FdTransfer() 接收
# 验证点: rollout restart 后旧 Worker 的 keep-alive 连接不断开,
#         客户端无感知, 无重连风暴。
# ─── Docker Compose 一键测试 (#166 P4) ──────────────────────
# ./deploy.sh test compose        全流程: build → up → test → stop
# ./deploy.sh test compose --quick 跳过 cmake build (已有二进制)
cmd_test_compose() {
    local compose_dir="${PROJECT_DIR}/docker"
    local compose_file="${compose_dir}/docker-compose.yml"
    local quick=false
    [[ " $* " =~ " --quick " ]] && quick=true

    echo ""
    echo -e "${BOLD}============================================${NC}"
    echo -e "${BOLD}  Docker Compose 集成测试${NC}"
    echo -e "${BOLD}============================================${NC}"

    # PRE-CHECK
    echo ""
    log "自愈检查..."
    _self_heal
    _check_resources

    # BUILD (可选)
    if ! $quick && [[ "${SKIP_BUILD:-false}" != "true" ]]; then
        log "cmake build + install..."
        cd "${PROJECT_DIR}"
        detect_openssl
        cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
            -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
            -DTHUNDER_BUILD_HELLO_PLUGINS=ON \
            "${CMAKE_OPENSSL_ARGS[@]}" >/dev/null || {
            err "cmake configure 失败"; return 1
        }
        cmake --build "${BUILD_DIR}" -j"${BUILD_JOBS}" || { err "C++ 编译失败"; return 1; }
        cmake --install "${BUILD_DIR}" || { err "cmake install 失败"; return 1; }
        _validate_io_backend
        ok "  C++ 构建完成"
    else
        ok "  跳过构建 (--quick)"
    fi

    # UP
    log "docker compose up -d..."
    cd "${compose_dir}"
    docker compose -f "${compose_file}" down --remove-orphans 2>/dev/null || true
    docker compose -f "${compose_file}" up -d 2>&1 || { err "compose up 失败"; return 1; }
    ok "  服务已启动"

    # 等端口
    log "等待端口就绪..."
    sleep 5
    for port in 27006 21883; do
        for i in $(seq 1 30); do
            timeout 1 bash -c "echo > /dev/tcp/127.0.0.1/$port" 2>/dev/null && break
            sleep 1
        done
    done
    ok "  端口就绪"

    # TEST
    log "运行 E2E 测试..."
    cd "${TESTS_DIR}/e2e"
    python3 -m pytest . -v -s --tb=short -m "integration or smoke" \
        --mode=local 2>&1 | tail -20
    local test_ret=${PIPESTATUS[0]}

    # STOP (保留数据, 不 down --volumes)
    log "docker compose stop..."
    cd "${compose_dir}"
    docker compose -f "${compose_file}" stop 2>/dev/null || true
    ok "  服务已停止 (数据保留)"

    if [[ $test_ret -eq 0 ]]; then
        echo -e "  ${GREEN}✔ Docker Compose 集成测试 PASS${NC}"
    else
        echo -e "  ${RED}✘ Docker Compose 集成测试 FAIL${NC}"
    fi
    return $test_ret
}

# ─── K8s 全量回归测试 ────────────────────────────────────────
# ./deploy.sh test k8s
cmd_test_k8s() {
    local ns="${K8S_NAMESPACE:-thunder}"
    local host_ip="${K8S_HOST_IP:-192.168.3.61}"
    local tag="test-$(date +%Y%m%d-%H%M%S)"
    local pass=0 fail=0

    echo ""
    echo -e "${BOLD}============================================${NC}"
    echo -e "${BOLD}  K8s 回归测试 — 5 阶段全流程${NC}"
    echo -e "${BOLD}  ns=${ns}  tag=${tag}${NC}"
    echo -e "${BOLD}============================================${NC}"

    # ============================================================
    # 阶段 0: PRE-CHECK — 环境自愈 + 增量检测 (#166)
    # ============================================================
    echo ""
    echo -e "${BOLD}--- [0/4] PRE-CHECK (环境自愈) ---${NC}"

    # 0.1 自愈: 清除非 K8s 进程占用的端口
    log "0.1 环境自愈 (端口+进程)..."
    _self_heal
    log "0.1a 环境自愈完成"

    # 0.2 僵尸 Pod 清理 (只清理业务服务, 保护 infra)
    set +e
    local zombie_count=0
    zombie_count=$(kubectl get pods -n "$ns" --no-headers --field-selector=status.phase!=Running 2>/dev/null \
        | grep -v -E "etcd|mysql|redis|minio|nfs|node-tuner" 2>/dev/null | wc -l 2>/dev/null || echo "0")
    zombie_count=$(echo "$zombie_count" | tr -d '[:space:]')
    [[ -z "$zombie_count" ]] && zombie_count=0
    if [[ "$zombie_count" -gt 0 ]] 2>/dev/null; then
        kubectl get pods -n "$ns" --no-headers --field-selector=status.phase!=Running 2>/dev/null \
            | grep -v -E "etcd|mysql|redis|minio|nfs|node-tuner" 2>/dev/null \
            | awk '{print $1}' | xargs -r kubectl delete pod -n "$ns" 2>/dev/null || true
    fi
    set -e

    # 0.3 资源余量
    log "0.3 宿主机资源..."
    _check_resources

    # 0.4 增量检测: 哪些服务需要重新构建
    local CHANGED_SVCS; CHANGED_SVCS=$(_changed_services || echo "all")
    log "0.4 增量检测: 需构建服务 = ${CHANGED_SVCS}"

    # 0.4 清理残留容器
    log "0.4 清理残留容器..."
    local dead_containers=$(docker ps -aq --filter "status=exited" 2>/dev/null | wc -l)
    if [[ "$dead_containers" -gt 10 ]]; then
        docker container prune -f 2>/dev/null || true
        ok "  已清理 ($dead_containers 个残留)"
    else
        ok "  无需清理 ($dead_containers 个)"
    fi

    # 0.5 清理 etcd 测试残留 (只删已知脏 key/entry, 保留 registry/canary 等运营数据)
    log "0.5 清理 etcd 测试残留..."
    local etcd_pod=$(kubectl get pods -n "$ns" -l app=thunder-etcd --field-selector=status.phase=Running -o jsonpath='{.items[0].metadata.name}' 2>/dev/null || echo "")
    if [[ -n "$etcd_pod" ]]; then
        # 删除 admin-web 历史遗留的变体 key (HELLOHTTP 无下划线)
        if kubectl exec -n "$ns" "$etcd_pod" -- etcdctl get /thunder/config/module/ --prefix --keys-only 2>/dev/null | grep -q "HELLOHTTP$"; then
            kubectl exec -n "$ns" "$etcd_pod" -- etcdctl del /thunder/config/module/HELLOHTTP 2>/dev/null || true
            ok "  已删除 stale key: HELLOHTTP"
        fi
        # 清理 module config 中的 _regression_* 条目 (只删条目, 不删整个 key)
        for key in HELLO_HTTP HELLO_HTTPS HELLO_WSS; do
            local cfg=$(kubectl exec -n "$ns" "$etcd_pod" -- etcdctl get "/thunder/config/module/$key" --print-value-only 2>/dev/null || echo "")
            if echo "$cfg" | grep -q "_regression"; then
                kubectl exec -n "$ns" "$etcd_pod" -- etcdctl del "/thunder/config/module/$key" 2>/dev/null || true
                ok "  已清理 ${key} 中的 _regression 残留 (重建后从本地 conf 恢复)"
            fi
        done
        # 删除 LOGIC module key (历次 SO 下发测试残留的 version bump + so_url:
        # Pod 启动会按 so_url 从 minio 拉陈旧/损坏 .so 覆盖本地插件 → Worker 崩溃循环。
        # 每次回归必须从本地 conf 版本 1 起步, SO 下发测试运行期间会自行重建该 key)
        kubectl exec -n "$ns" "$etcd_pod" -- etcdctl del /thunder/config/module/LOGIC 2>/dev/null || true
        ok "  etcd 残留检查完成"
    else
        warn "  etcd 不可达, 跳过"
    fi

    echo -e "${GREEN}✔${NC} PRE-CHECK 完成"

    # ============================================================
    # 阶段 1: BUILD — 增量构建 (#166)
    # ============================================================
    echo ""
    echo -e "${BOLD}--- [1/4] BUILD (需构建: ${CHANGED_SVCS}) ---${NC}"

    if [[ "$CHANGED_SVCS" == "none" ]]; then
        ok "  无代码变更, 跳过 BUILD"
    else
        # 1.1 C++ cmake build + install
        log "1.1 C++ cmake build + install..."
        cd "${PROJECT_DIR}"
        detect_openssl
        cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
            -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
            -DTHUNDER_BUILD_TESTS=ON \
            -DTHUNDER_BUILD_HELLO_PLUGINS=ON \
            -DTHUNDER_BUILD_NODE_PLUGINS=ON \
            ${GTEST_SRC_DIR:+-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="${GTEST_SRC_DIR}"} \
            "${CMAKE_OPENSSL_ARGS[@]}" >/dev/null || {
            err "cmake configure 失败"
            return 1
        }
        cmake --build "${BUILD_DIR}" -j"${BUILD_JOBS}" || {
            err "C++ 编译失败"
            return 1
        }
        cmake --install "${BUILD_DIR}" || {
            err "cmake install 失败"
            return 1
        }
        _validate_io_backend
        ok "  C++ 构建完成"

        # 1.2 admin-web
        if [[ "$CHANGED_SVCS" == "all" ]] || echo "$CHANGED_SVCS" | grep -q "admin"; then
            log "1.2 编译 admin-web (Go)..."
            cd "${PROJECT_DIR}/deploy/admin-web"
            CGO_ENABLED=1 go build -o admin-web . 2>&1 || { err "admin-web Go 编译失败"; return 1; }
            ok "  admin-web 编译完成"
            cd "${PROJECT_DIR}"
        fi

        # 1.3 Docker 镜像 (按需构建)
        log "1.3 构建 Docker 镜像..."
        local images_ok=true
        _build_docker_if_needed() {
            local svc="$1" dockerfile="$2" img_name="$3" ctx="${4:-.}"
            if [[ "$CHANGED_SVCS" == "all" ]] || echo " $CHANGED_SVCS " | grep -q " $svc "; then
                docker build -f "$dockerfile" -t "${img_name}:${tag}" "$ctx" 2>&1 | tail -3 || {
                    err "${img_name} 镜像构建失败"; images_ok=false
                }
            else
                log "  ${img_name}:${tag} 跳过 (未变更)"
            fi
        }
        _build_docker_if_needed "interface"  "deploy/Interface/Dockerfile"    "thunder-interface"
        _build_docker_if_needed "hello"      "deploy/HelloHttp/Dockerfile"    "thunder-hello"
        _build_docker_if_needed "hello"      "deploy/HelloHttps/Dockerfile"   "thunder-hello-https"
        _build_docker_if_needed "hello"      "deploy/HelloWs/Dockerfile"      "thunder-hello-ws"
        _build_docker_if_needed "hello"      "deploy/HelloWss/Dockerfile"     "thunder-hello-wss"
        _build_docker_if_needed "logic"      "deploy/Logic/Dockerfile"        "thunder-logic"
        _build_docker_if_needed "mqtt"       "deploy/MqttBroker/Dockerfile"   "thunder-mqtt"
        _build_docker_if_needed "admin"      "deploy/admin-web/Dockerfile"    "thunder-admin-web" "deploy/admin-web/"

        if [[ "$images_ok" != "true" ]]; then
            err "部分镜像构建失败"
            return 1
        fi
        ok "  Docker 镜像构建完成"
    fi

    echo -e "${GREEN}✔${NC} BUILD 完成"

    # ============================================================
    # 阶段 2: DEPLOY — 智能镜像导入 + 滚动更新 (#166)
    # ============================================================
    echo ""
    echo -e "${BOLD}--- [2/4] DEPLOY (智能导入) ---${NC}"

    # 2.1 智能镜像导入 (只导入 digest 变化的镜像)
    if [[ "$CHANGED_SVCS" == "none" ]]; then
        ok "  无镜像变更, 跳过 containerd 导入"
    else
        log "2.1 智能镜像导入..."
        _smart_ctr_import "$tag" || {
            err "镜像导入 containerd 失败"
            return 1
        }
        ok "  镜像导入完成"
    fi

    # 2.2b 确保 7 个 Deployment 存在 (缺失则从 k8s/ apply, 不再静默跳过)
    log "2.2b 检查 Deployment 是否存在..."
    declare -A DEP_YAML=(
        [thunder-hello]="hello-deployment.yaml"
        [thunder-hello-https]="hello-https-deployment.yaml"
        [thunder-hello-ws]="hello-ws-deployment.yaml"
        [thunder-hello-wss]="hello-wss-deployment.yaml"
        [thunder-interface]="interface-deployment.yaml"
        [thunder-logic]="logic-deployment.yaml"
        [thunder-logic-v2]="logic-v2-deployment.yaml"
        [thunder-mqtt-broker]="mqtt-broker-deployment.yaml"
        [thunder-admin-web]="admin-web-deployment.yaml"
    )
    for dep in "${!DEP_YAML[@]}"; do
        if ! kubectl get deploy "$dep" -n "$ns" >/dev/null 2>&1; then
            log "  $dep 不存在, apply k8s/${DEP_YAML[$dep]} ..."
            kubectl apply -f "k8s/${DEP_YAML[$dep]}" || {
                err "  $dep apply 失败"
                return 1
            }
            ok "  $dep 已创建"
        else
            # 历史 deployment 可能残留 imagePullPolicy: Always (旧 yaml 创建, 从不重新 apply)
            # Always + 时间戳 tag → kubelet 无视本地镜像直连 docker.io, 必挂 ImagePullBackOff
            kubectl patch deployment "$dep" -n "$ns" --type=strategic \
                -p '{"spec":{"template":{"spec":{"containers":[{"name":"app","imagePullPolicy":"IfNotPresent"}]}}}}' \
                >/dev/null 2>&1 || true
        fi
    done
    ok "  Deployment 检查完成"

    # 2.3 滚动更新所有 Deployment
    log "2.3 滚动更新所有 Gateway + admin-web..."
    # 确保 replicas=1 (CLEAN 阶段可能已 scale 到 0)
    for dep in thunder-hello thunder-hello-https thunder-hello-ws thunder-hello-wss \
               thunder-interface thunder-logic thunder-logic-v2 thunder-admin-web; do
        kubectl scale deploy -n "$ns" "$dep" --replicas=1 2>/dev/null || true
    done
    declare -A DEP_IMAGE=(
        [thunder-interface]="thunder-interface:${tag}"
        [thunder-hello]="thunder-hello:${tag}"
        [thunder-hello-https]="thunder-hello-https:${tag}"
        [thunder-hello-ws]="thunder-hello-ws:${tag}"
        [thunder-hello-wss]="thunder-hello-wss:${tag}"
        [thunder-logic]="thunder-logic:${tag}"
        [thunder-logic-v2]="thunder-logic:${tag}"
        [thunder-admin-web]="thunder-admin-web:${tag}"
    )
    for dep in "${!DEP_IMAGE[@]}"; do
        local img="${DEP_IMAGE[$dep]}"
        kubectl -n "$ns" set image "deployment/${dep}" "*=${img}" 2>/dev/null || {
            err "  $dep set image 失败"
            return 1
        }
        ok "  $dep → $img"
    done

	    # 2.3b 节点性能优化 DaemonSet (#154)
	    log "2.3b 部署 node-tuner DaemonSet..."
	    kubectl apply -f k8s/node-tuner-daemonset.yaml 2>/dev/null
	    ok "  node-tuner deployed"

    # 2.4 等待全部 Pod Ready
    log "2.4 等待 Pod Ready (最长 180s)..."
    kubectl wait --for=condition=Ready pods --all -n "$ns" --timeout=180s 2>/dev/null || {
        warn "  部分 Pod 未在 180s 内 Ready"
        kubectl get pods -n "$ns" 2>/dev/null | grep -v -E "Completed|Error|Evicted" | head -20
    }

    # 2.4a 磁盘压力大时 kubelet imageGC 会删掉刚导入但尚未被使用的镜像 (本机磁盘 >85% 时每 5min 一轮)
    #       检测到 ImagePull 失败则重导入并强制重建 Pending Pod, 避免 ErrImagePull 假死
    if kubectl get pods -n "$ns" --no-headers 2>/dev/null | grep -qE 'ImagePullBackOff|ErrImagePull'; then
        warn "  检测到 ImagePull 失败 (kubelet imageGC 可能删了未使用镜像), 重新导入..."
        _smart_ctr_import "$tag" || {
            err "镜像重导入 containerd 失败"
            return 1
        }
        kubectl delete pods -n "$ns" --field-selector=status.phase=Pending 2>/dev/null || true
        kubectl wait --for=condition=Ready pods --all -n "$ns" --timeout=180s 2>/dev/null || {
            err "  重导入后 Pod 仍未 Ready"
            kubectl get pods -n "$ns" 2>/dev/null | grep -v -E "Completed|Error|Evicted" | head -20
            return 1
        }
        ok "  重导入后 Pod 全部 Ready"
    fi

    # 2.4b 校验各 Deployment 镜像版本 == 本次 tag (防止跑旧镜像)
    log "2.4b 校验 Deployment 镜像版本 (tag=${tag})..."
    for dep in "${!DEP_IMAGE[@]}"; do
        local actual=$(kubectl get deployment "$dep" -n "$ns" -o jsonpath='{.spec.template.spec.containers[*].image}' 2>/dev/null || echo "")
        if [[ "$actual" != *":${tag}"* ]]; then
            err "  $dep 镜像版本不符: 期望 :${tag}, 实际: ${actual:-<none>}"
            return 1
        fi
        ok "  $dep → $actual"
    done

    # 2.5 验证 etcd 注册 (等待节点重新注册, 含 LOGIC)
    log "2.5 等待 etcd 重新注册 (含 LOGIC)..."
    local max_wait=60 waited=0
    while [[ $waited -lt $max_wait ]]; do
        local reg_keys=$(kubectl exec -n "$ns" "$etcd_pod" -- etcdctl get /thunder/registry/ --prefix --keys-only 2>/dev/null || echo "")
        local reg_count=$(echo "$reg_keys" | grep -c "/" || echo "0")
        if [[ "$reg_count" -ge 5 ]] && echo "$reg_keys" | grep -q "LOGIC"; then
            ok "  已注册 $reg_count 个节点, 含 LOGIC (${waited}s)"
            break
        fi
        sleep 5
        waited=$((waited + 5))
    done
    if [[ $waited -ge $max_wait ]]; then
        warn "  注册超时 (当前 $reg_count 个, LOGIC 未就绪), 继续测试..."
        warn "  thunder-logic 最近日志:"
        kubectl logs -n "$ns" deploy/thunder-logic --tail=30 2>/dev/null | sed 's/^/    /' || true
    fi

    echo -e "${GREEN}✔${NC} DEPLOY 完成"

    # 等待服务真正就绪 (pod Ready ≠ SO 热加载完成, 需 15-30s)
    log "2.6 等待 SO 热加载完成 (最长 45s)..."
    local deadline=$(($(date +%s) + 45))
    local svc_ready=false
    while [[ $(date +%s) -lt ${deadline} ]]; do
        if curl -s -m 2 "http://127.0.0.1:27006/hello/hello" -X POST \
             -H "Content-Type: application/json" -d '{"option":"Echo","size":2}' 2>/dev/null | grep -q '"code":0' && \
           curl -s -m 2 "http://127.0.0.1:27008/Interface/gentoken" 2>/dev/null | grep -q .; then
            svc_ready=true
            break
        fi
        sleep 1
    done
    if $svc_ready; then
        ok "  SO 热加载完成 ($((45 - $((${deadline} - $(date +%s)))))s)"
    else
        warn "  SO 热加载超时, 继续测试..."
    fi

    # ============================================================
    # 阶段 3: TEST — 回归测试
    # ============================================================
    echo ""
    echo -e "${BOLD}--- [3/4] TEST (全量回归) ---${NC}"

    local test_output test_ret=0
    # set -e 下命令替换赋值失败会静默终止脚本 (CLEAN 不执行), 必须用 || 捕获
    test_output=$(bash "${PROJECT_DIR}/k8s/regression-test.sh" 2>&1) || test_ret=$?
    echo "$test_output"
    # 解析通过/失败/跳过/总计 (先剥 ANSI 颜色码, 取含"总计"的最终汇总行)
    local summary_line=$(echo "$test_output" | sed 's/\x1b\[[0-9;]*m//g' | grep '总计:' | tail -1)
    local t_pass=$(echo "$summary_line" | grep -oP '通过:\s*\K[0-9]+' || echo "?")
    local t_fail=$(echo "$summary_line" | grep -oP '失败:\s*\K[0-9]+' || echo "0")
    local t_skip=$(echo "$summary_line" | grep -oP '跳过:\s*\K[0-9]+' || echo "0")
    local t_total=$(echo "$summary_line" | grep -oP '总计:\s*\K[0-9]+' || echo "?")
    if [[ $test_ret -eq 0 ]]; then
        ok "回归测试: ${t_pass}/${t_total} PASS (${t_fail} FAIL, ${t_skip} SKIP)"
    else
        fail=$((fail + 1))
        err "回归测试: ${t_fail}/${t_total} FAIL (${t_pass} PASS, ${t_skip} SKIP)"
    fi

    # ============================================================
    # 阶段 4: CLEAN — 清理测试残留
    # ============================================================
    echo ""
    echo -e "${BOLD}--- [4/4] CLEAN (清理测试残留) ---${NC}"

    # 4.1 删测试 artifacts
    log "4.1 清理制品库测试文件..."
    local admin_pod=$(kubectl get pods -n "$ns" -l app=thunder-admin-web --field-selector=status.phase=Running -o jsonpath='{.items[0].metadata.name}' 2>/dev/null || echo "")
    if [[ -n "$admin_pod" ]]; then
        kubectl exec -n "$ns" "$admin_pod" -- sh -c 'rm -f /app/data/artifacts/*/_regression_*' 2>/dev/null || true
        ok "  制品库已清理"
    fi

    # 4.2 删 NFS 测试文件
    log "4.2 清理 NFS 测试文件..."
    if [[ -n "$admin_pod" ]]; then
        kubectl exec -n "$ns" "$admin_pod" -- sh -c 'rm -f /data/thunder/plugins/*/_regression_*' 2>/dev/null || true
        ok "  NFS 已清理"
    fi

    # 4.3 清理 etcd 测试残留 (只删 _regression_ 项, 保留 registry/canary 等运营数据)
    log "4.3 清理 etcd 测试残留..."
    local etcd_pod_clean=$(kubectl get pods -n "$ns" -l app=thunder-etcd --field-selector=status.phase=Running -o jsonpath='{.items[0].metadata.name}' 2>/dev/null || echo "")
    if [[ -n "$etcd_pod_clean" ]]; then
        # 只删 test tag 镜像的模块配置 (PRE-CHECK 已清过, 这里兜底)
        for key in $(kubectl exec -n "$ns" "$etcd_pod_clean" -- etcdctl get /thunder/config/module/ --prefix --keys-only 2>/dev/null | grep -v "^$" | grep -v "/$"); do
            if kubectl exec -n "$ns" "$etcd_pod_clean" -- etcdctl get "$key" --print-value-only 2>/dev/null | grep -q "_regression"; then
                kubectl exec -n "$ns" "$etcd_pod_clean" -- etcdctl del "$key" 2>/dev/null || true
                ok "  已删除 stale key: $key"
            fi
        done
        # LOGIC module key 一并清除 (SO 下发测试的 version bump/so_url 残留会致下轮 Pod 拉坏 .so 崩溃)
        kubectl exec -n "$ns" "$etcd_pod_clean" -- etcdctl del /thunder/config/module/LOGIC 2>/dev/null || true
        ok "  etcd 清理完成"
    fi

    # 4.4 清 SQLite 审计残留
    log "4.4 清理 SQLite 审计残留..."
    if [[ -n "$admin_pod" ]]; then
        kubectl exec -n "$ns" "$admin_pod" -- sh -c 'sqlite3 /app/data/admin.db "DELETE FROM audit_log WHERE target LIKE '\''%_regression_%'\'';"' 2>/dev/null || true
        ok "  审计记录已清理"
    fi

    # 4.5 清理本地 docker 测试镜像 (所有 thunder-*:test-*, 非仅本次 tag)
    log "4.5 清理本地测试镜像..."
    docker images --format '{{.Repository}}:{{.Tag}}' 2>/dev/null | grep -E '^thunder-.*:test-' | \
        xargs -r docker rmi 2>/dev/null || true
    ok "  本地镜像已清理"

    # 4.5b 清理 containerd 中旧 test 镜像 (保留本次 tag, 防止磁盘累积; 失败只 warn)
    log "4.5b 清理 containerd 旧 test 镜像..."
    docker run --rm --privileged --pid=host --network=host \
        alpine:latest nsenter -t 1 -m -u -n -i -p -- sh -c "
            ctr -n k8s.io images ls -q | grep -E 'thunder-.*:test-' | grep -v ':${tag}' | \
                xargs -r ctr -n k8s.io images rm
        " 2>/dev/null || warn "  containerd 旧镜像清理失败 (忽略)"
    ok "  containerd 旧镜像已清理"

    # 4.6 关闭业务服务 (归还端口, 不影响下次测试)
    log "4.6 关闭业务服务 (释放端口)..."
    for dep in thunder-hello thunder-hello-https thunder-hello-ws thunder-hello-wss \
               thunder-interface thunder-logic thunder-logic-v2 thunder-admin-web \
               thunder-mqtt-broker; do
        kubectl scale deploy -n "$ns" "$dep" --replicas=0 2>/dev/null || true
    done
    sleep 3
    # 杀残留孤儿进程
    sudo killall -9 Hello_robot Interface_robot MqttBroker_robot 2>/dev/null || true
    ok "  服务已关闭, 端口已释放"

    echo -e "${GREEN}✔${NC} CLEAN 完成"

    # ============================================================
    # 汇总
    # ============================================================
    echo ""
    echo -e "${BOLD}============================================${NC}"
    if [[ $fail -eq 0 ]]; then
        echo -e "  ${GREEN}✔ K8s 回归测试: ${t_pass:-?}/${t_total:-?} PASS (${t_fail:-0} FAIL, ${t_skip:-0} SKIP)${NC}"
        # 保存状态: 记录本次通过的 commit + 更新镜像 digest
        git rev-parse HEAD > "$STATE_K8S_OK" 2>/dev/null || true
        ok "  增量状态已保存 ($STATE_K8S_OK)"
    else
        echo -e "  ${RED}✘ K8s 回归测试: ${t_fail:-?} FAIL (${t_pass:-?}/${t_total:-?})${NC}"
    fi
    echo -e "${BOLD}============================================${NC}"
    return $fail
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

cmd_image() {
    local tag="${IMAGE_TAG:-latest}"
    local registry="${IMAGE_REGISTRY:-}"
    local services=("${@}")

    if [[ ${#services[@]} -eq 0 ]]; then
        services=(logic hello http https ws wss interface admin-web)
    fi

    for svc in "${services[@]}"; do
        local dir ctx tag_name
        case "$svc" in
            logic)     dir="deploy/Logic";       ctx="."; tag_name="thunder-logic" ;;
            hello)     dir="deploy/HelloHttp";   ctx="."; tag_name="thunder-hello-http" ;;
            https)     dir="deploy/HelloHttps";  ctx="."; tag_name="thunder-hello-https" ;;
            ws)        dir="deploy/HelloWs";     ctx="."; tag_name="thunder-hello-ws" ;;
            wss)       dir="deploy/HelloWss";    ctx="."; tag_name="thunder-hello-wss" ;;
            interface) dir="deploy/Interface";   ctx="."; tag_name="thunder-interface" ;;
            admin-web) dir="deploy/admin-web";   ctx="deploy/admin-web"; tag_name="thunder-admin-web" ;;
            *)
                warn "未知服务: $svc, 跳过"
                continue
                ;;
        esac

        local full_tag="${tag_name}:${tag}"
        if [[ -n "$registry" ]]; then
            full_tag="${registry}/${tag_name}:${tag}"
        fi

        log "构建镜像: ${full_tag}"
        docker build -t "$full_tag" -f "${dir}/Dockerfile" "$ctx" || {
            error "镜像构建失败: $full_tag"
            return 1
        }
        ok "${full_tag}"
    done
}

cmd_push() {
    local registry="${IMAGE_REGISTRY:-}"
    if [[ -z "$registry" ]]; then
        error "请设置 IMAGE_REGISTRY 环境变量"
        return 1
    fi
    local tag="${IMAGE_TAG:-latest}"
    local services=("${@}")

    if [[ ${#services[@]} -eq 0 ]]; then
        services=(logic hello http https ws wss interface admin-web)
    fi

    for svc in "${services[@]}"; do
        local tag_name
        case "$svc" in
            logic)     tag_name="thunder-logic" ;;
            hello)     tag_name="thunder-hello-http" ;;
            https)     tag_name="thunder-hello-https" ;;
            ws)        tag_name="thunder-hello-ws" ;;
            wss)       tag_name="thunder-hello-wss" ;;
            interface) tag_name="thunder-interface" ;;
            admin-web) tag_name="thunder-admin-web" ;;
            *) warn "未知服务: $svc, 跳过"; continue ;;
        esac

        local full_tag="${registry}/${tag_name}:${tag}"
        log "推送: ${full_tag}"
        docker push "$full_tag" || {
            error "推送失败: $full_tag"
            return 1
        }
        ok "${full_tag}"
    done
}

cmd_deploy() {
    local ns="${K8S_NAMESPACE:-thunder}"
    local tag="${IMAGE_TAG:-latest}"

    echo ""
    log "K8s 一键部署 (ns=${ns})"

    # 1. 清理僵尸 + 非 Running Pod
    sudo killall -9 Hello_robot Interface_robot 2>/dev/null || true
    kubectl delete pod -n "$ns" --field-selector=status.phase!=Running 2>/dev/null || true
    sleep 2
    # 等端口释放
    for port in 27006 27007 27008 27009 27010 27011 27012 27443 27444; do
        while ss -tlnH 2>/dev/null | grep -q ":$port "; do sleep 1; done
    done

    # 2. 构建镜像
    cmd_image || return 1

    # 3. 导入 containerd
    log "导入镜像到 containerd ..."
    for img in thunder-logic  thunder-hello-http thunder-hello-https \
               thunder-hello-ws thunder-hello-wss thunder-interface; do
        docker save "$img:$tag" 2>/dev/null | sudo -S ctr -n k8s.io image import - 2>/dev/null
    done

    # 4. 部署基础设施
    kubectl apply -f k8s/etcd-pv.yaml 2>/dev/null
    kubectl apply -f k8s/etcd-statefulset.yaml 2>/dev/null
    kubectl apply -f k8s/mysql.yaml 2>/dev/null
    kubectl apply -f k8s/redis.yaml 2>/dev/null
    sleep 10

    # 5. 部署业务服务
    for yaml in k8s/logic-deployment.yaml k8s/logic-v2-deployment.yaml \
                k8s/hello-deployment.yaml k8s/hello-https-deployment.yaml \
                k8s/hello-ws-deployment.yaml k8s/hello-wss-deployment.yaml \
                k8s/interface-deployment.yaml \
                k8s/mqtt-broker-deployment.yaml; do
        kubectl apply -f "$yaml" 2>/dev/null
    done

	# 5b. 节点性能优化 (#154) — 在所有业务 Pod 之后部署
	kubectl apply -f k8s/node-tuner-daemonset.yaml 2>/dev/null
    ok "deploy 完成，等待 Pod ..."
    kubectl wait --for=condition=Ready pods --all -n "$ns" --timeout=180s 2>/dev/null || true
    kubectl get pods -n "$ns" 2>/dev/null | grep -v -E "Completed|Error|Evicted" | head -20
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

# ─── Release ────────────────────────────────────
# 一键: cmake → image → 部署 → 回归
#   ./deploy.sh release        Docker Compose
#   ./deploy.sh release k8s    K8s (需集群)
cmd_release() {
    local target="${1:-compose}"
    local tag="${IMAGE_TAG:-latest}"

    echo ""
    echo -e "${BOLD}=== Thunder Release: ${target} ===${NC}"

    # 1. cmake build
    cmd_build

    # 2. docker image build (所有服务)
    log "构建 Docker 镜像..."
    cmd_image logic interface hello https ws wss || {
        err "镜像构建失败"
        return 1
    }

    if [[ "$target" == "k8s" ]]; then
        # 3a. K8s deploy
        cmd_deploy
        # 4a. K8s 回归
        log "K8s 回归测试..."
        bash "${PROJECT_DIR}/k8s/regression-test.sh"
    else
        # 3b. Docker Compose up
        log "启动 Docker Compose..."
        cd "${DOCKER_DIR}"
        docker compose down --remove-orphans 2>/dev/null || true
        docker compose up -d || { err "compose up 失败"; return 1; }

        # 4b. 等待就绪
        log "等待服务就绪 (最长 120s)..."
        local deadline=$(($(date +%s) + 120))
        while [[ $(date +%s) -lt ${deadline} ]]; do
            ss -tln 2>/dev/null | grep -q ':27006 ' && ss -tln 2>/dev/null | grep -q ':27008 ' && break
            sleep 3
        done

        # 5b. 回归测试
        log "回归测试..."
        run_cpp_unit
        run_py_unit
        cmd_test_regression
    fi

    ok "Release 完成 (${target})"
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
            smoke)
                cmd_test_smoke
                ;;
            regression)
                cmd_test_regression
                ;;
            perf|bench)
                cmd_bench
                ;;
            k8s)
                cmd_test_k8s
                ;;
            compose)
                cmd_test_compose
                ;;
            *)
                # 全部: unit + e2e
                run_cpp_unit
                run_py_unit
                cmd_test_e2e
                ;;
        esac
        ;;
    build-so)
        cmd_build_so
        ;;
    image)
        cmd_image "${_EXTRA_ARGS[@]}"
        ;;
    push)
        cmd_push "${_EXTRA_ARGS[@]}"
        ;;
    deploy)
        cmd_deploy
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
    admin)
        python3 "${PROJECT_DIR}/deploy/scripts/admin.py" "${_ADMIN_ARGS[@]}"
        ;;
    clean)
        cmd_clean
        ;;
    release)
        cmd_release "${MODE:-compose}"
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
