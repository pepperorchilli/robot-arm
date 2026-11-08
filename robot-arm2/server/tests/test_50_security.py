"""安全测试：把"能正常用"之外的那一半补上。

这一组不看业务，只看**攻击者视角**：
  · 静态文件服务会不会被路径穿越读到 server.js；
  · 设备令牌、数据库口令会不会出现在任何响应里；
  · 伪造的会话 token 能不能混进去；
  · 注入串能不能穿透到 SQL；
  · 超大请求体有没有被挡住，以及挡住时的状态码对不对。

其中两条用例用 xfail(strict=True) 标记：它们描述的是**正确行为**，
但当前实现不满足。这样做的价值是——
  · 缺陷被记录成可执行的断言，而不是躺在某个人脑子里的 TODO；
  · 修好之后用例会变成 XPASS，strict 模式下 CI 立刻变红，
    逼着人来删掉标记，不会悄悄失效。
"""

from __future__ import annotations

import http.client
import json

import pytest

# 这个文件里的裸请求共用一个模拟 IP，和 conftest 分配的 10.42.* 不冲突
RAW_IP = "10.50.0.1"


def _raw_get(server, path: str) -> tuple[int, bytes]:
    """绕过 httpx 的 URL 归一化，把带有 %2e%2e 的原始路径原样发出去。

    httpx（和所有正常客户端）都会在发送前把 `/../` 折叠掉，
    所以想测路径穿越，只能自己拼 HTTP 报文。
    """
    conn = http.client.HTTPConnection("127.0.0.1", server.port, timeout=5)
    try:
        conn.putrequest("GET", path, skip_host=True, skip_accept_encoding=True)
        conn.putheader("Host", "127.0.0.1")
        conn.putheader("X-Forwarded-For", RAW_IP)
        conn.endheaders()
        resp = conn.getresponse()
        return resp.status, resp.read()
    finally:
        conn.close()


# ---------------------------------------------------------------- 静态文件

@pytest.mark.parametrize("path", [
    "/../server.js",
    "/..%2fserver.js",
    "/%2e%2e/server.js",
    "/..%2f..%2fpackage.json",
    "/..%2fconfig.js",
])
def test_path_traversal_cannot_read_source(server, path):
    """express.static 的根目录是 public/，任何跳出该目录的尝试都必须失败。"""
    status, body = _raw_get(server, path)

    assert status == 404
    assert b"ESP32_TOKEN" not in body
    assert b"require(" not in body


def test_server_side_files_are_not_served(api):
    """/server.js、/config.js、/schema.sql 这些不在 public/ 里，不该有 URL。"""
    for path in ("/server.js", "/config.js", "/schema.sql", "/.env", "/package.json"):
        r = api.get(path)
        assert r.status_code == 404, f"{path} 被意外暴露：{r.status_code}"
        assert "ESP32_TOKEN" not in r.text


def test_api_docs_do_not_leak_secrets(server, api):
    """OpenAPI 文档是公开的，里面不能出现设备令牌或数据库口令。"""
    spec = api.get("/api-docs.json")

    assert spec.status_code == 200
    assert server.device_token not in spec.text
    assert "arm_dev_password" not in spec.text
    assert "password_hash" not in spec.text


def test_device_endpoint_does_not_leak_token(api, device):
    body = api.get("/api/device")

    assert set(body.json()) == {"online", "since", "onlineSeconds"}
    assert "token" not in body.text.lower()


# ---------------------------------------------------------------- 会话伪造

@pytest.mark.parametrize("path", ["/set?servo=0&angle=90", "/api/telemetry"])
def test_forged_token_is_rejected(api, path):
    r = api.get(path, headers={"X-Auth-Token": "forged-token-0123456789abcdef"})

    assert r.status_code == 401


def test_forged_token_does_not_become_a_session(api):
    r = api.get("/api/me", headers={"X-Auth-Token": "forged-token-0123456789abcdef"})

    # /api/me 是查询接口，不返回 401，但必须明确告诉前端"没登录"
    assert r.status_code == 200
    assert r.json()["authed"] is False


def test_session_cookie_is_hardened(api, admin):
    """cookie 属性是防 XSS 窃取和 CSRF 的第一道墙。"""
    r = api.post("/api/login", json={
        "username": admin.account["username"], "password": "admin-pass-123",
    })
    cookie = r.headers["set-cookie"]

    assert "HttpOnly" in cookie          # JS 读不到，XSS 拿不走
    assert "SameSite=Lax" in cookie      # 跨站表单提交不会带上它
    assert "Path=/" in cookie


# ---------------------------------------------------------------- 注入

def test_sql_injection_in_login_is_not_executed(make_client, admin):
    """注入串走到参数化查询里，只会被当成一个不存在的用户名。"""
    client = make_client()

    r = client.post("/api/login", json={
        "username": "' OR '1'='1",
        "password": "' OR '1'='1",
    })
    assert r.status_code == 401

    # 表还在，原账号还能正常登录 —— 说明没有被注入破坏
    ok = client.post("/api/login", json={
        "username": admin.account["username"], "password": "admin-pass-123",
    })
    assert ok.status_code == 200


def test_sql_injection_in_message_is_stored_as_text(api, user):
    """留言内容原样入库。如果拼接 SQL，这条 DELETE 会把整张表删掉。"""
    payload = "'; DROP TABLE messages; --"

    r = api.post("/api/messages", json={"content": payload}, headers=user.headers)
    assert r.status_code == 200
    assert r.json()["content"] == payload

    listing = api.get("/api/messages", headers=user.headers)
    assert listing.status_code == 200
    assert [m["content"] for m in listing.json()] == [payload]


def test_xss_payload_is_stored_verbatim(api, user):
    """转义是渲染层的事，存储层不该改写用户输入（否则原文就丢了）。"""
    payload = "<script>alert(1)</script>"

    r = api.post("/api/messages", json={"content": payload}, headers=user.headers)

    assert r.json()["content"] == payload


# ---------------------------------------------------------------- 请求体

@pytest.mark.xfail(
    reason="已知缺陷：body-parser 的 413 被兜底错误处理改写成了 500",
    strict=True,
)
def test_oversized_json_body_is_rejected_with_413(api):
    """超过 express.json 的 100kb 上限，必须返回 413，而不是 500。

    当前实现：body-parser 抛出的是带 status 的错误，但兜底错误处理
    只认业务异常，其余一律 500。对客户端来说，"你的包太大"和
    "服务器炸了"是两件事，状态码混用会让重试策略做错。
    """
    big = json.dumps({
        "username": "oversize", "password": "pass-123456",
        "nickname": "x", "pad": "z" * 200_000,
    })

    r = api.post("/api/register", content=big,
                 headers={"Content-Type": "application/json"})

    assert r.status_code == 413


def test_malformed_json_does_not_crash_the_server(api):
    r = api.post("/api/login", content="{not json",
                 headers={"Content-Type": "application/json"})

    assert r.status_code in (400, 500)
    # 进程还活着，后续请求照常
    assert api.get("/api/device").status_code == 200
