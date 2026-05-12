#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HELLO_HOME="$(cd "${SCRIPT_DIR}/.." && pwd)"
CERT_DIR="${HELLO_HOME}/conf/certs"

mkdir -p "${CERT_DIR}"

CA_KEY="${CERT_DIR}/ca.key"
CA_CRT="${CERT_DIR}/ca.crt"
SERVER_KEY="${CERT_DIR}/server.key"
SERVER_CSR="${CERT_DIR}/server.csr"
SERVER_CRT="${CERT_DIR}/server.crt"

if [[ -f "${CA_CRT}" && -f "${SERVER_CRT}" && -f "${SERVER_KEY}" ]]; then
  echo "证书已存在，跳过生成: ${CERT_DIR}"
  exit 0
fi

if ! command -v openssl >/dev/null 2>&1; then
  echo "错误: 未找到 openssl，请先安装。" >&2
  exit 1
fi

echo "==> 生成 CA 证书"
openssl genrsa -out "${CA_KEY}" 2048 >/dev/null 2>&1
openssl req -x509 -new -nodes -key "${CA_KEY}" -sha256 -days 3650 \
  -subj "/C=CN/ST=ZJ/L=HZ/O=Thunder/CN=Thunder-Dev-CA" \
  -addext "basicConstraints=critical,CA:TRUE" \
  -addext "keyUsage=critical,keyCertSign,cRLSign" \
  -out "${CA_CRT}" >/dev/null 2>&1

echo "==> 生成服务端证书"
openssl genrsa -out "${SERVER_KEY}" 2048 >/dev/null 2>&1
openssl req -new -key "${SERVER_KEY}" \
  -subj "/C=CN/ST=ZJ/L=HZ/O=Thunder/CN=127.0.0.1" \
  -out "${SERVER_CSR}" >/dev/null 2>&1

openssl x509 -req -in "${SERVER_CSR}" -CA "${CA_CRT}" -CAkey "${CA_KEY}" \
  -CAcreateserial -out "${SERVER_CRT}" -days 3650 -sha256 \
  -extfile <(printf "subjectAltName=IP:127.0.0.1,DNS:localhost\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n") >/dev/null 2>&1

rm -f "${SERVER_CSR}" "${CERT_DIR}/ca.srl"
chmod 600 "${CA_KEY}" "${SERVER_KEY}"

echo "==> 证书生成完成: ${CERT_DIR}"

