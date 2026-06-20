#!/usr/bin/env bash
# Build etcd-cpp-apiv3 (v0.15.4) and install headers + .so into code/3party/.
#
# 前提：
#   1. git submodule update --init --recursive   (检出 etcd-cpp-apiv3/)
#   2. code/3party/ 下的 protobuf 已编译完成      (lib/ 下有 libprotobuf.so)
#
# 依赖：gRPC 运行库必须与本项目的 protobuf ABI 兼容。
# 系统 libgrpc 1.51 链接了系统 protobuf 3.21，与本项目 protobuf 5.x 不兼容，
# 因此需要另行编译 gRPC。本脚本提供两种模式：
#
#   --mode=system-grpc   直接用系统 gRPC（仅限测试环境，可能 ABI 不匹配）
#   --mode=build-grpc    从源码编译 gRPC（兼容本项目 protobuf，耗时 ~30min）
#
# 默认：--mode=build-grpc
#
# 用法：
#   cd /path/to/thunder
#   bash code/3party/build_etcd.sh [--mode=build-grpc|system-grpc] [--jobs=N]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THUNDER_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
STAGE_DIR="$SCRIPT_DIR"              # install prefix for etcd headers + .so
PROTOBUF_STAGE="$STAGE_DIR"          # 已有 include/google/ lib/libprotobuf.so
ETCD_SRC="$SCRIPT_DIR/etcd-cpp-apiv3"

GRPC_VERSION="v1.66.5"              # 与 protobuf 5.x / absl-20250512 兼容
GRPC_STAGE="/tmp/thunder-grpc-stage"
JOBS=$(nproc)
BUILD_MODE="build-grpc"

# ── 参数解析 ─────────────────────────────────────────────────
for arg in "$@"; do
    case "$arg" in
        --mode=*) BUILD_MODE="${arg#--mode=}" ;;
        --jobs=*) JOBS="${arg#--jobs=}" ;;
        -j*)      JOBS="${arg#-j}" ;;
        --help|-h)
            grep "^#" "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "[build_etcd] 未知参数: $arg"; exit 1 ;;
    esac
done

echo "[build_etcd] mode=$BUILD_MODE jobs=$JOBS"
echo "[build_etcd] etcd src : $ETCD_SRC"
echo "[build_etcd] install  : $STAGE_DIR/include + $STAGE_DIR/lib"

# ── 检查子模块 ────────────────────────────────────────────────
if [[ ! -f "$ETCD_SRC/CMakeLists.txt" ]]; then
    echo "[build_etcd] ERROR: $ETCD_SRC/CMakeLists.txt 不存在"
    echo "             请先执行: git submodule update --init --recursive"
    exit 1
fi

# ── Step 1: 编译 gRPC（如需要）────────────────────────────────
if [[ "$BUILD_MODE" == "build-grpc" ]]; then
    if [[ -f "$GRPC_STAGE/lib/libgrpc++.a" || -f "$GRPC_STAGE/lib/libgrpc++.so" ]]; then
        echo "[build_etcd] gRPC 已在 $GRPC_STAGE，跳过编译"
    else
        echo "[build_etcd] 克隆 gRPC $GRPC_VERSION ..."
        GRPC_SRC="/tmp/thunder-grpc-src"
        rm -rf "$GRPC_SRC"
        git clone --depth 1 --branch "$GRPC_VERSION" --recurse-submodules \
            https://github.com/grpc/grpc.git "$GRPC_SRC"

        echo "[build_etcd] 编译 gRPC（耗时约 20-40 分钟）..."
        cmake -S "$GRPC_SRC" -B "$GRPC_SRC/build" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$GRPC_STAGE" \
            -DgRPC_PROTOBUF_PROVIDER=package \
            -DProtobuf_ROOT="$PROTOBUF_STAGE" \
            -DABSL_ROOT="$PROTOBUF_STAGE" \
            -DgRPC_ABSL_PROVIDER=package \
            -DgRPC_BUILD_TESTS=OFF \
            -DgRPC_BUILD_GRPC_CSHARP_PLUGIN=OFF \
            -DgRPC_BUILD_GRPC_NODE_PLUGIN=OFF \
            -DgRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN=OFF \
            -DgRPC_BUILD_GRPC_PHP_PLUGIN=OFF \
            -DgRPC_BUILD_GRPC_PYTHON_PLUGIN=OFF \
            -DgRPC_BUILD_GRPC_RUBY_PLUGIN=OFF \
            -DgRPC_BUILD_GRPC_CPP_PLUGIN=ON \
            -DBUILD_SHARED_LIBS=OFF
        cmake --build "$GRPC_SRC/build" --parallel "$JOBS"
        cmake --install "$GRPC_SRC/build"
        echo "[build_etcd] gRPC 安装到 $GRPC_STAGE"
    fi
    GRPC_CMAKE_PREFIX="$GRPC_STAGE"
else
    # system-grpc: 用系统 gRPC（仅测试用，ABI 可能不匹配本项目 protobuf）
    echo "[build_etcd] WARNING: 使用系统 gRPC，可能与本项目 protobuf ABI 不兼容"
    GRPC_CMAKE_PREFIX="/usr"
fi

# ── Step 2: 编译 etcd-cpp-apiv3 ──────────────────────────────
ETCD_BUILD="/tmp/thunder-etcd-build"
rm -rf "$ETCD_BUILD"

echo "[build_etcd] 编译 etcd-cpp-apiv3 v0.15.4 ..."
cmake -S "$ETCD_SRC" -B "$ETCD_BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$STAGE_DIR" \
    -DBUILD_ETCD_CORE_ONLY=ON \
    -DBUILD_ETCD_TESTS=OFF \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_PREFIX_PATH="$PROTOBUF_STAGE;$GRPC_CMAKE_PREFIX" \
    -DProtobuf_ROOT="$PROTOBUF_STAGE" \
    -DETCD_W_STRICT=OFF

cmake --build "$ETCD_BUILD" --parallel "$JOBS"
cmake --install "$ETCD_BUILD"

echo ""
echo "[build_etcd] ✅ 完成"
echo "  头文件: $STAGE_DIR/include/etcd/"
so_file=$(find "$STAGE_DIR/lib" -name "libetcd-cpp-api-core.so*" | head -1)
echo "  库文件: ${so_file:-$STAGE_DIR/lib/libetcd-cpp-api-core.so (未找到，请检查安装输出)}"
echo ""
echo "下一步: ./deploy.sh build"
