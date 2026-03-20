#!/usr/bin/env bash
# 将 staging 与 protobuf 构建树中的产物复制到 code/3party/lib、include 与 deploy/3lib（覆盖同名文件）
# 用法（由 CMake thirdparty_deploy 调用）:
#   deploy_thirdparty.sh <THUNDER_ROOT> <STAGE_DIR> <THIRD_PARTY_DIR> <DEPLOY_3LIB_DIR> <PROTOBUF_BUILD_DIR>
set -euo pipefail

THUNDER_ROOT="${1:?}"
STAGE="${2:?}"
THIRD="${3:?}"
DEPLOY3="${4:?}"
PB_BUILD="${5:?}"

mkdir -p "${THIRD}/lib" "${THIRD}/include" "${DEPLOY3}"

if [[ -d "${STAGE}/lib" ]]; then
  # hiredis-vip 的 install 会 ln -sf 形成 .so -> .so.1 -> .so.1.0 链；若目标目录里残留旧 symlink，
  # 多次 deploy 合并时可能成环，导致 cp: Too many levels of symbolic links。先清掉再拷。
  for d in "${THIRD}/lib" "${DEPLOY3}"; do
    [[ -d "$d" ]] || continue
    shopt -s nullglob
    for f in "${d}/libhiredis_vip.so" "${d}/libhiredis_vip.so."*; do
      [[ -e "$f" ]] && rm -f -- "$f"
    done
    shopt -u nullglob
  done
  # GNU cp：覆盖前先删目标，避免 symlink 成环时 stat 失败
  cp -a --remove-destination "${STAGE}/lib/." "${THIRD}/lib/"
  cp -a --remove-destination "${STAGE}/lib/." "${DEPLOY3}/"
fi

if [[ -d "${STAGE}/include" ]]; then
  cp -a "${STAGE}/include/." "${THIRD}/include/"
fi

# protobuf 仍建在源码树 protobuf/build（Thunder 链接 absl 等依赖该目录）；额外把 .so / protoc 同步到 lib 与 3lib
if [[ -d "${PB_BUILD}" ]]; then
  shopt -s nullglob
  for f in "${PB_BUILD}"/libprotobuf*.so "${PB_BUILD}"/libprotobuf*.so.* \
           "${PB_BUILD}"/libprotoc*.so "${PB_BUILD}"/libprotoc*.so.*; do
    [[ -e "$f" ]] || continue
    cp -a "$f" "${THIRD}/lib/"
    cp -a "$f" "${DEPLOY3}/"
  done
  if [[ -f "${PB_BUILD}/protoc" ]]; then
    cp -a "${PB_BUILD}/protoc" "${THIRD}/lib/" || true
    cp -a "${PB_BUILD}/protoc" "${DEPLOY3}/" || true
  fi
  utf8="${PB_BUILD}/third_party/utf8_range"
  if [[ -d "$utf8" ]]; then
    for f in "$utf8"/libutf8*.so "$utf8"/libutf8*.so.* "$utf8"/libutf8_validity*; do
      [[ -e "$f" ]] || continue
      cp -a "$f" "${THIRD}/lib/"
      cp -a "$f" "${DEPLOY3}/"
    done
  fi
  shopt -u nullglob
fi

echo "Third-party deploy: ${THIRD}/lib + ${THIRD}/include + ${DEPLOY3}"
