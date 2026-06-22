from __future__ import annotations

import os
import re
import shutil
from pathlib import Path

import requests
import pytest

from helpers.runtime import repo_root, require_ok, run_cmd

# 从 conftest 已加载的端口环境变量里读；WRK_TARGET 可覆盖整个 URL（k8s 场景）
_HELLO_HTTP_PORT = os.environ.get("THUNDER_HELLO_HTTP_PORT", "27006")
_DEFAULT_TARGET = f"http://127.0.0.1:{_HELLO_HTTP_PORT}/hello/hello"


def _extract_metric(text: str, pattern: str) -> str:
    m = re.search(pattern, text, re.MULTILINE)
    return m.group(1).strip() if m else "N/A"


@pytest.mark.perf
def test_wrk_smoke(proxyless_env: dict[str, str]) -> None:
    # 测试目的：给出可读的性能冒烟结果（而不仅仅是"命令成功"）。
    results_dir = repo_root() / "tests" / "benchmark" / "results"
    results_dir.mkdir(parents=True, exist_ok=True)
    report = results_dir / "wrk_test_result.md"
    wrk_threads = os.getenv("WRK_THREADS", "4")
    wrk_connections = os.getenv("WRK_CONNECTIONS", "100")
    wrk_duration = os.getenv("WRK_DURATION", "60s")
    target = os.getenv("WRK_TARGET", _DEFAULT_TARGET)
    if shutil.which("wrk") is None:
        # 无 wrk 时退化为轻量请求回归，确保该用例在默认命令中可执行。
        s = requests.Session()
        s.trust_env = False
        for _ in range(3):
            r = s.post(target, json={"option": "Echo"}, timeout=10)
            assert r.status_code == 200, r.text
            assert '"code"' in r.text
        report.write_text(
            "# wrk test result\n\n"
            "- mode: fallback-http\n"
            "- reason: `wrk` not installed\n"
            "- check: 3x POST `/hello/hello` success\n",
            encoding="utf-8",
        )
        return
    script = repo_root() / "tests" / "benchmark" / "wrk_helloserver.lua"
    cp = run_cmd(
        [
            "wrk",
            f"-t{wrk_threads}",
            f"-c{wrk_connections}",
            f"-d{wrk_duration}",
            "-s",
            str(script),
            "--latency",
            target,
        ],
        cwd=repo_root(),
        env=proxyless_env,
    )
    require_ok(cp, "wrk smoke")
    out = cp.stdout or ""
    requests_sec = _extract_metric(out, r"Requests/sec:\s*([0-9.]+)")
    latency = _extract_metric(out, r"Latency\s+([0-9.]+\w+)")
    transfer_sec = _extract_metric(out, r"Transfer/sec:\s*([0-9.]+\w+)")
    summary = (
        "# wrk test result\n\n"
        "- mode: wrk\n"
        "- target: `" + target + "`\n"
        "- args: `-t" + wrk_threads + " -c" + wrk_connections + " -d" + wrk_duration + "`\n"
        "- requests_per_sec: " + requests_sec + "\n"
        "- latency_avg: " + latency + "\n"
        "- transfer_per_sec: " + transfer_sec + "\n\n"
        "## raw output\n\n"
        "```text\n" + out + "\n```\n"
    )
    report.write_text(summary, encoding="utf-8")
    print(
        f"[wrk-smoke] requests/sec={requests_sec}, latency={latency}, transfer/sec={transfer_sec}\n"
        f"[wrk-smoke] report={report}"
    )

