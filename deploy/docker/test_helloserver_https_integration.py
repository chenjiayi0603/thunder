#!/usr/bin/env python3
"""
Hello HTTPS integration test (Python requests).

Usage:
  python3 ./deploy/docker/test_helloserver_https_integration.py

Optional env vars:
  HELLO_HTTPS_HOST=127.0.0.1
  HELLO_HTTPS_PORT=27443
  HELLO_HTTPS_PATH=/hello/hello
  HELLO_HTTPS_CA=./deploy/HelloHttps/conf/certs/ca.crt
  HELLO_HTTPS_INSECURE=0|1
  HELLO_HTTPS_TIMEOUT=10
"""

from __future__ import annotations

import json
import os
import sys
from typing import Any, Dict, Iterable, Optional

import requests


def _env(name: str, default: str) -> str:
    value = os.getenv(name)
    return value if value is not None else default


HOST = _env("HELLO_HTTPS_HOST", "127.0.0.1")
PORT = _env("HELLO_HTTPS_PORT", "27443")
PATH = _env("HELLO_HTTPS_PATH", "/hello/hello")
CA_FILE = _env("HELLO_HTTPS_CA", "./deploy/HelloHttps/conf/certs/ca.crt")
INSECURE = _env("HELLO_HTTPS_INSECURE", "0") == "1"
TIMEOUT = float(_env("HELLO_HTTPS_TIMEOUT", "10"))
URL = f"https://{HOST}:{PORT}{PATH}"


def _verify_value() -> Any:
    if INSECURE:
        return False
    return CA_FILE


def _post_case(name: str, payload: Dict[str, Any], must_contain: Iterable[str]) -> None:
    print(f"=== {name}: POST {URL} ===")
    response = requests.post(URL, json=payload, verify=_verify_value(), timeout=TIMEOUT)
    print(f"status={response.status_code}")
    print(response.text)

    if response.status_code != 200:
        raise AssertionError(f"{name}: HTTP {response.status_code}, expect 200")

    body_text = response.text
    for needle in must_contain:
        if needle not in body_text:
            raise AssertionError(f"{name}: response missing '{needle}'")

    # Validate JSON shape when possible.
    parsed: Optional[Dict[str, Any]] = None
    try:
        parsed = response.json()
    except json.JSONDecodeError:
        parsed = None

    if parsed is not None:
        if "code" not in parsed:
            raise AssertionError(f"{name}: json missing key 'code'")
        if "msg" not in parsed:
            raise AssertionError(f"{name}: json missing key 'msg'")

    print(f"--- {name}: OK ---")


def main() -> int:
    try:
        _post_case("TLSHandshake+Echo", {"option": "Echo"}, ('"code"', '"msg"'))
        _post_case("TLSHandshake+TestHelloPoolCpu", {"option": "TestHelloPoolCpu"}, ("TestHelloPoolCpu", "786432"))
        _post_case("TLSHandshake+TestHelloPoolBlock", {"option": "TestHelloPoolBlock"}, ("TestHelloPoolBlock", '"slept_ms":80'))
    except requests.exceptions.SSLError as exc:
        print(f"TLS verify failed: {exc}", file=sys.stderr)
        print("Tip: check cert path or set HELLO_HTTPS_INSECURE=1 for debugging.", file=sys.stderr)
        return 2
    except Exception as exc:  # pylint: disable=broad-except
        print(f"Integration test failed: {exc}", file=sys.stderr)
        return 1

    print("=== Hello HTTPS Python integration test passed ===")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
