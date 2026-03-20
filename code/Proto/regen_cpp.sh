#!/usr/bin/env bash
# 兼容入口：等价于 cmake --build … --target thunder_proto_gen（只生成 .pb 源，不编 libProto.so）。
# 用法（仓库根）：PROTO_BUILD_DIR=build bash code/Proto/regen_cpp.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
B="${PROTO_BUILD_DIR:-${ROOT}/build}"
exec cmake --build "${B}" --target thunder_proto_gen
