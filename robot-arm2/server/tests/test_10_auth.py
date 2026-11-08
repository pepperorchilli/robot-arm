"""账号与会话：注册、登录、鉴权、权限边界。

这一组对应测开 JD 里的"接口测试"与"安全测试"。
重点不是"能注册就算过"，而是**边界**：
  · 用户名格式的边界（2 位 / 3 位 / 含非法字符）
  · 密码长度的边界（5 位 / 6 位）
  · 重复注册
  · 未登录、越权、登出后会话失效
"""

from __future__ import annotations

import pytest


def test_first_registered_account_becomes_admin(make_client):
    client = make_client()

    r = client.post("/api/register", json={
        "username": "first01", "password": "pass-123456", "nickname": "第一个",
    })

    assert r.status_code == 200
    assert r.json()["account"]["role"] == "admin"
    # 会话是 httpOnly cookie —— 前端 JS 不应该能读到它
    assert "HttpOnly" in r.headers["set-cookie"]


def test_second_registered_account_is_normal_user(user):
    assert user.account["role"] == "user"


def test_username_too_short_is_rejected(api):
    r = api.post("/api/register", json={
        "username": "ab", "password": "pass-123456", "nickname": "短",
    })

    assert r.status_code == 400
    assert "用户名" in r.json()["error"]


def test_username_with_illegal_chars_is_rejected(api):
    r = api.post("/api/register", json={
        "username": "bad name!", "password": "pass-123456", "nickname": "非法",
    })

    assert r.status_code == 400


def test_password_shorter_than_six_is_rejected(api):
    r = api.post("/api/register", json={
        "username": "shortpw", "password": "12345", "nickname": "短密码",
    })

    assert r.status_code == 400
    assert "密码" in r.json()["error"]


def test_empty_nickname_is_rejected(api):
    r = api.post("/api/register", json={
        "username": "nonick", "password": "pass-123456", "nickname": "   ",
    })

    assert r.status_code == 400


def test_duplicate_username_is_rejected(api, admin):
    """重复注册必须失败 —— 数据库的唯一索引是最后一道防线。"""
    r = api.post("/api/register", json={
        "username": admin.account["username"],
        "password": "pass-123456",
        "nickname": "撞名",
    })

    assert r.status_code == 409


def test_login_with_correct_password(make_client, admin):
    client = make_client()

    r = client.post("/api/login", json={
        "username": admin.account["username"], "password": "admin-pass-123",
    })

    assert r.status_code == 200
    assert r.json()["account"]["username"] == admin.account["username"]
    assert "arm_token=" in r.headers["set-cookie"]


def test_login_with_wrong_password_is_401(make_client, admin):
    client = make_client()

    r = client.post("/api/login", json={
        "username": admin.account["username"], "password": "wrong-password",
    })

    assert r.status_code == 401
    # 不能泄露"用户名存在但密码错"这种信息
    assert "用户名或密码" in r.json()["error"]


def test_login_with_unknown_user_is_401(make_client):
    client = make_client()

    r = client.post("/api/login", json={
        "username": "no-such-user", "password": "pass-123456",
    })

    assert r.status_code == 401


@pytest.mark.xfail(
    strict=True,
    reason="已知缺陷：password 缺失时 crypto.scrypt 抛类型错误，被兜底成 500",
)
def test_login_without_password_is_400_not_500(make_client):
    """字段缺失是客户端错误（4xx），不是服务器故障（500）。

    现在 password 为 undefined / null 时会一路走到 crypto.scrypt，
    抛 TypeError 被兜底处理成 500。前端漏传字段就会看到"服务器内部错误"，
    排障时完全指错方向。
    """
    client = make_client()

    r = client.post("/api/login", json={"username": "ghost"})

    assert r.status_code == 400


def test_me_reports_not_authed_without_token(api):
    body = api.get("/api/me").json()

    assert body["authed"] is False


def test_control_requires_login(api):
    """/set 是执行器接口，未登录必须挡住 —— 这是最危险的一条路径。"""
    r = api.get("/set?servo=0&angle=90")

    assert r.status_code == 401


def test_telemetry_requires_login(api):
    r = api.get("/api/telemetry")

    assert r.status_code == 401


def test_normal_user_cannot_reply_as_admin(api, user):
    """越权测试：普通用户走管理员接口，必须在鉴权层就被挡住。"""
    r = api.post(
        "/api/messages/1/reply",
        json={"content": "越权回复"},
        headers=user.headers,
    )

    assert r.status_code == 403
    assert "管理员" in r.json()["error"]


def test_logout_invalidates_session(api, admin):
    assert api.get("/api/me", headers=admin.headers).json()["authed"] is True

    assert api.post("/api/logout", headers=admin.headers).status_code == 200

    # 登出后同一个 token 不能再被接受
    assert api.get("/api/me", headers=admin.headers).json()["authed"] is False
