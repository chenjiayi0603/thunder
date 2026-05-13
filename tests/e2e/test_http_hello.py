from __future__ import annotations

import pytest
import requests


BASE = "http://127.0.0.1:27006/hello/hello"


@pytest.mark.integration
@pytest.mark.smoke
@pytest.mark.parametrize(
    ("payload", "needles"),
    [
        ({"option": "Echo"}, ['"code"', '"msg"']),
        ({"option": "TestHelloPoolCpu"}, ["TestHelloPoolCpu", "786432"]),
        ({"option": "TestHelloPoolBlock"}, ["TestHelloPoolBlock", '"slept_ms":80']),
        ({"option": "NoSuchOption"}, ['"code"']),
    ],
)
def test_http_hello_options(payload: dict[str, str], needles: list[str], http_session: requests.Session) -> None:
    resp = http_session.post(BASE, json=payload, timeout=20)
    assert resp.status_code == 200, resp.text
    for n in needles:
        assert n in resp.text, resp.text

