#!/usr/bin/env bash
# 启动 Hello 节点（可执行文件 deploy/bin/Hello，工作目录 deploy/Hello）
# 用法: ./start_helloserver.sh
#       CONF=conf/Hello.json ./start_helloserver.sh

set -euo pipefail

DEPLOY_ROOT="$(cd "$(dirname "$0")" && pwd)"
CODE_ROOT="$(cd "${DEPLOY_ROOT}/../code" && pwd)"
CONF="${CONF:-conf/Hello.json}"

export LD_LIBRARY_PATH="${DEPLOY_ROOT}/lib:${CODE_ROOT}/3party/lib:${CODE_ROOT}/3party/lib/mariadb:${CODE_ROOT}/3party/protobuf/build${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

cd "${DEPLOY_ROOT}/Hello"
exec "${DEPLOY_ROOT}/bin/Hello" "${CONF}"
