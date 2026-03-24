#!/usr/bin/env bash
# 构建 Thunder → 构建/启动 Docker Compose → 打印多节点日志片段
#
# 用法（仓库任意路径）:
#   ./deploy/docker/dev_up_logs.sh
#   ./deploy/docker/dev_up_logs.sh down                  # 仅关闭 Compose 栈（等价 stop）
#   ./deploy/docker/dev_up_logs.sh status                # 仅查看 Compose 容器状态（等价 ps）
#   DOCKER_DOWN=1 ./deploy/docker/dev_up_logs.sh         # 同上
#   DOCKER_STATUS=1 ./deploy/docker/dev_up_logs.sh       # 同上 status
#   SKIP_BUILD=1 ./deploy/docker/dev_up_logs.sh          # 跳过 cmake，直接 docker
#   FOLLOW=1 ./deploy/docker/dev_up_logs.sh              # 持续跟随 docker 日志（Ctrl+C 结束）
#   SERVICES="center logic" LOG_TAIL=100 ./deploy/docker/dev_up_logs.sh
#
# 环境变量:
#   SKIP_BUILD=1          不执行 cmake 构建与 install
#   SKIP_THIRD_PARTY=1    与 SKIP_BUILD 无关；为 1 时构建阶段不跑 thirdparty_deploy（仅当已部署过 3lib）
#   BUILD_JOBS=1           cmake --build -j（默认 1，避免并行卡死）
#   CMAKE_BUILD_TYPE      默认 RelWithDebInfo
#   BUILD_DIR             默认 仓库根下 build
#   LOG_TAIL              docker compose logs --tail 行数（默认 80）
#   DEPLOY_LOG_TAIL       宿主机 deploy/*/log/*.log 每个文件尾部行数（默认 40）
#   FOLLOW=1              docker compose logs -f（默认只打印快照后退出）
#   SERVICES              空格分隔服务名，默认 center logic hello interface
#   DOCKER_DOWN=1         只执行 docker compose down，不做构建与 up
#   DOCKER_STATUS=1       只查看 docker compose 状态（ps -a、images、top）
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DOCKER_DIR="${SCRIPT_DIR}"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"
BUILD_JOBS="${BUILD_JOBS:-1}"
SKIP_BUILD="${SKIP_BUILD:-0}"
SKIP_THIRD_PARTY="${SKIP_THIRD_PARTY:-0}"
LOG_TAIL="${LOG_TAIL:-80}"
DEPLOY_LOG_TAIL="${DEPLOY_LOG_TAIL:-40}"
FOLLOW="${FOLLOW:-0}"
SERVICES="${SERVICES:-center logic hello interface}"
DOCKER_DOWN="${DOCKER_DOWN:-0}"
DOCKER_STATUS="${DOCKER_STATUS:-0}"

# 子命令: down | stop | status | ps
case "${1:-}" in
  down | stop) DOCKER_DOWN=1 ;;
  status | ps) DOCKER_STATUS=1 ;;
esac

if [[ "${DOCKER_DOWN}" == "1" ]]; then
  cd "${DOCKER_DIR}"
  echo "==> docker compose down（${DOCKER_DIR}）"
  docker compose down --remove-orphans
  echo "==> 剩余容器（若有）"
  docker compose ps -a || true
  exit 0
fi

if [[ "${DOCKER_STATUS}" == "1" ]]; then
  cd "${DOCKER_DIR}"
  echo "==> docker compose ps -a（${DOCKER_DIR}）"
  docker compose ps -a
  echo ""
  echo "==> docker compose images"
  docker compose images 2>/dev/null || true
  echo ""
  echo "==> docker compose top（各服务进程）"
  docker compose top 2>/dev/null || true
  exit 0
fi

cd "${REPO_ROOT}"

if [[ "${SKIP_BUILD}" != "1" ]]; then
  echo "==> [1/3] CMake 配置: ${BUILD_DIR} (${CMAKE_BUILD_TYPE})"
  cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  if [[ "${SKIP_THIRD_PARTY}" != "1" ]]; then
    echo "==> [1/3] 第三方部署 thirdparty_deploy (-j${BUILD_JOBS})"
    cmake --build "${BUILD_DIR}" --target thirdparty_deploy -j"${BUILD_JOBS}"
  else
    echo "==> [1/3] 跳过 thirdparty_deploy (SKIP_THIRD_PARTY=1)"
  fi
  echo "==> [1/3] 全量构建并安装到 deploy/ (-j${BUILD_JOBS})"
  cmake --build "${BUILD_DIR}" -j"${BUILD_JOBS}"
  cmake --install "${BUILD_DIR}"
  echo "==> [1/3] 完成"
else
  echo "==> [1/3] 跳过构建 (SKIP_BUILD=1)"
fi

cd "${DOCKER_DIR}"
echo "==> [2/3] docker compose build（并行度由 Docker 控制；若卡可另开终端 DOCKER_BUILDKIT=0 docker compose build）"
docker compose build

echo "==> [2/3] docker compose up -d"
docker compose up -d

echo "==> [3/3] 容器状态"
docker compose ps -a

_log_deploy_files() {
  local deploy_root="${REPO_ROOT}/deploy"
  echo ""
  echo "---------- 宿主机 deploy/*/log 近期文件（每文件尾部 ${DEPLOY_LOG_TAIL} 行）----------"
  local found=0
  local n=0
  while IFS= read -r -d '' f && [[ "${n}" -lt 20 ]]; do
    found=1
    n=$((n + 1))
    echo ""
    echo "===== ${f#"${REPO_ROOT}/"} ====="
    tail -n "${DEPLOY_LOG_TAIL}" "${f}" 2>/dev/null || true
  done < <(find "${deploy_root}" -type f -path '*/log/*.log' -print0 2>/dev/null)
  if [[ "${found}" -eq 0 ]]; then
    echo "(未找到 deploy/*/log/*.log，可能尚未写入或路径不同)"
  fi
}

if [[ "${FOLLOW}" == "1" ]]; then
  echo "==> [3/3] 跟随 Docker 日志（${SERVICES}）Ctrl+C 结束；退出后仍会打印 deploy 日志快照"
  # shellcheck disable=SC2086
  docker compose logs -f --tail "${LOG_TAIL}" ${SERVICES} || true
  _log_deploy_files
else
  echo "==> [3/3] Docker 日志快照（--tail ${LOG_TAIL}）"
  # shellcheck disable=SC2086
  docker compose logs --tail "${LOG_TAIL}" ${SERVICES} 2>/dev/null || docker compose logs --tail "${LOG_TAIL}"
  _log_deploy_files
  echo ""
  echo "提示: FOLLOW=1 $0 可持续跟踪容器日志；状态: $0 status；停止栈: $0 down"
fi
