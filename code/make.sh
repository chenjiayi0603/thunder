#!/usr/bin/env bash
# =============================================================================
# Thunder 构建入口（已改为 CMake，不再调用 Util/Net 下 makefile）
#
# 在 code/ 目录执行，例如：
#   ./make.sh all              # gen_proto + cmake 全量编译（默认 build/）
#   ./make.sh Util|Proto|Net|plugin
#   ./make.sh clean            # cmake --build ... --target clean
#   ./make.sh install          # cmake --install（默认安装到 ../deploy）
#   ./make.sh first            # 全量编译 + cmake --install + deploy/install.sh first + restart_nodes
#
# 等价于在仓库根目录：
#   cmake -S . -B build && cmake --build build -j$(nproc) && cmake --install build
#
# 环境变量：
#   THUNDER_BUILD_DIR   构建目录，默认 <仓库根>/build
#   CMAKE_BUILD_TYPE    默认 RelWithDebInfo
#   THUNDER_CMAKE_ARGS  追加传给 cmake 配置，如 -DTHUNDER_BUILD_CENTER=OFF
# =============================================================================

: <<'DOC'
历史说明（原 make.sh + makefile 流程已废弃）：
  原 ./make.sh Util / Net 曾调用 Util/makefile、Net/src/makefile。
  现统一由根目录 CMakeLists.txt 生成 Ninja/Makefile 并构建。
DOC

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THUNDER_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${THUNDER_BUILD_DIR:-${THUNDER_ROOT}/build}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

function Usage() {
  cat <<EOF
用法（在 code/ 目录下）: ./make.sh <命令>

  all       运行 Proto/codegen + CMake 全量编译（Util、Proto、Net、Hello、Center 等）
  Util      仅构建 target Util（libUtil.so）
  Proto     运行 gen_proto.sh + 构建 target Proto
  Net       仅构建 target Net
  plugin    仅构建 Hello 插件 target ModuleHello
  clean     cmake --build ... --target clean
  install   cmake --install（CMAKE_INSTALL_PREFIX 默认 ../deploy）
  first     全量编译 + cmake --install + deploy/install.sh first + restart_nodes.sh all
  firstrun  仅 deploy/install.sh first + restart_nodes（不编译）
  run       deploy/restart_nodes.sh all

其他：
  多节点插件（Center、Logic、Interface 等）仍用 ./plugins.sh（见 plugins.conf）；协议见 ./make.sh Proto

构建目录: ${BUILD_DIR}
并行任务: ${JOBS}（可用环境变量 JOBS 覆盖）
EOF
}

function make_dir() {
  find "${SCRIPT_DIR}" -maxdepth 6 -type f -name "*.sh" -exec chmod +x {} + 2>/dev/null || true
  mkdir -p "${THUNDER_ROOT}/deploy/lib"
  if [[ ! -e "${SCRIPT_DIR}/3party/lib" ]] && [[ -d "${THUNDER_ROOT}/deploy/3lib" ]]; then
    ln -sfn "${THUNDER_ROOT}/deploy/3lib" "${SCRIPT_DIR}/3party/lib"
  fi
}

function thunder_cmake_configure() {
  cmake -S "${THUNDER_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}" \
    ${THUNDER_CMAKE_ARGS:-}
}

function thunder_cmake_build() {
  local -a extra=()
  [[ -n "${1:-}" ]] && extra=(--target "$1")
  thunder_cmake_configure
  cmake --build "${BUILD_DIR}" "${extra[@]}" --parallel "${JOBS}"
}

function thunder_cmake_install() {
  cmake --install "${BUILD_DIR}"
}

function run_gen_proto() {
  if [[ -x "${SCRIPT_DIR}/Proto/src/gen_proto.sh" ]]; then
    (cd "${SCRIPT_DIR}/Proto/src" && ./gen_proto.sh)
  elif [[ -f "${SCRIPT_DIR}/Proto/src/gen_proto.sh" ]]; then
    (cd "${SCRIPT_DIR}/Proto/src" && bash ./gen_proto.sh)
  fi
}

function do_first() {
  run_gen_proto
  thunder_cmake_build
  thunder_cmake_install
  (cd "${THUNDER_ROOT}/deploy" && ./install.sh first)
  (cd "${THUNDER_ROOT}/deploy" && ./restart_nodes.sh all)
}

function do_firstrun() {
  (cd "${THUNDER_ROOT}/deploy" && ./install.sh first)
  (cd "${THUNDER_ROOT}/deploy" && ./restart_nodes.sh all)
}

function do_run() {
  (cd "${THUNDER_ROOT}/deploy" && ./restart_nodes.sh all)
}

# -----------------------------------------------------------------------------

if [[ $# -lt 1 ]]; then
  Usage
  exit 1
fi

make_dir

case "$1" in
  -h|--help|help)
    Usage
    ;;
  Util|Net|Center|Hello|ModuleHello)
    thunder_cmake_build "$1"
    ;;
  Proto)
    run_gen_proto
    thunder_cmake_build Proto
    ;;
  plugin)
    thunder_cmake_build ModuleHello
    ;;
  all)
    run_gen_proto
    thunder_cmake_build
    ;;
  clean)
    if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
      cmake --build "${BUILD_DIR}" --target clean --parallel "${JOBS}"
    else
      echo "提示: 尚未配置 ${BUILD_DIR}，跳过 clean"
    fi
    ;;
  install)
    thunder_cmake_install
    ;;
  first)
    do_first
    ;;
  firstrun)
    do_firstrun
    ;;
  run)
    do_run
    ;;
  *)
    Usage
    exit 1
    ;;
esac
