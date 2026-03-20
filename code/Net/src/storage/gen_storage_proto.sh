#!/usr/bin/env bash
set -euo pipefail
ASYNC_SERVER_PATH=$(dirname "$0")
cd "${ASYNC_SERVER_PATH}"
ASYNC_SERVER_PATH=$(pwd)
CODE_ROOT="$(cd "${ASYNC_SERVER_PATH}/../../.." && pwd)"
PROTOC="${CODE_ROOT}/3party/protobuf/build/protoc"
PROTOC_LIB="${CODE_ROOT}/3party/protobuf/build"
UTF8_LIB="${CODE_ROOT}/3party/protobuf/build/third_party/utf8_range"
export LD_LIBRARY_PATH="${PROTOC_LIB}:${UTF8_LIB}:${LD_LIBRARY_PATH:-}"
chmod +x "${PROTOC}"
"${PROTOC}" --version
"${PROTOC}" -I=. --cpp_out=. ./dataproxy.proto
cp -f ./*.pb.h ../../include/storage/
