#!/usr/bin/env bash
# 使用与工程一致的 protoc 生成 C++ 代码，并应用 GCC + protobuf 7 生成器的已知补丁。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
PROTOC="${ROOT}/../3party/protobuf/build/protoc"
cd "${ROOT}"
"${PROTOC}" -I. --cpp_out=src common.proto coor.proto enum.proto test_proto3.proto user.proto user_basic.proto

python3 "${ROOT}/patch_common_pb_h.py"

for m in TEXT_CONTENT PICTURE_CONTENT VOICE_CONTENT msg_content errorinfo user_info single_msg_push session_info quality_control_option taboo_option; do
  sed -i "s/PROTOBUF_FIELD_OFFSET(${m},/PROTOBUF_FIELD_OFFSET(::common::${m},/g" src/common.pb.cc
  sed -i "s/offsetof(${m},/offsetof(::common::${m},/g" src/common.pb.cc
done

echo "Proto C++ regenerated and patched under ${ROOT}/src"
