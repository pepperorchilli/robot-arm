"""冒烟测试：服务起来之后最基本的三件事。

冒烟测试不验证业务，它只回答一个问题——"这个服务还活着吗"。
CI 里它应该永远第一个跑，挂了就不用看后面的结果了。
"""

from __future__ import annotations


def test_openapi_document_available(api):
    r = api.get("/api-docs.json")

    assert r.status_code == 200
    spec = r.json()
    assert spec["openapi"].startswith("3.")
    assert "/set" in spec["paths"]
    assert "/api/telemetry" in spec["paths"]


def test_security_headers_present(api):
    r = api.get("/api/docs-should-not-matter")

    # 这些头不改变功能，只是关掉浏览器的危险默认行为。
    # 漏了不会让测试变红，但会在安全扫描里被标出来 —— 所以要有断言盯着。
    assert r.headers["X-Content-Type-Options"] == "nosniff"
    assert r.headers["X-Frame-Options"] == "DENY"
    assert r.headers["Referrer-Policy"] == "no-referrer"


def test_device_offline_before_any_connection(api):
    body = api.get("/api/device").json()

    assert body == {"online": False}


def test_homepage_is_served(api):
    r = api.get("/")

    assert r.status_code == 200
    assert "text/html" in r.headers["content-type"]
