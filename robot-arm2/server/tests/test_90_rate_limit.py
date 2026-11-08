"""限流与防爆破：三套计数、三个阈值、三种响应。

服务器有三道互相独立的闸：
  1. 登录失败   同一 IP 10 分钟失败 5 次 → 封 15 分钟
  2. 注册       同一 IP 1 小时最多 5 个账号
  3. 全局请求   同一 IP 1 分钟最多 300 次（/set 豁免）

这三套计数都存在**进程内存**里，用例之间会互相污染 —— 所以每一组都
从一个全新的模拟 IP 开始，正好也验证了"计数是按 IP 分别记的"这件事。

限流是最容易写错、也最容易被忽视的一块：阈值调低会把正常用户拦在门外
（真实踩过，见 server.js 注释），调高等于没设。所以这里测的不是"有没有",
而是**第几次开始拦、状态码和 Retry-After 对不对、豁免路径真的豁免**。
"""

from __future__ import annotations

import httpx


def _client(server, ip: str) -> httpx.Client:
    return httpx.Client(base_url=server.base_url, timeout=10,
                        headers={"X-Forwarded-For": ip})


# ---------------------------------------------------------------- 全局

def test_global_limit_blocks_the_301st_request(server):
    """阈值 300/分钟：前 300 次通过，第 301 次返回 429 并告知多久后重试。"""
    with _client(server, "10.60.0.1") as client:
        statuses = [client.get("/api/device").status_code for _ in range(300)]
        blocked = client.get("/api/device")

    assert set(statuses) == {200}, "前 300 次里出现了非 200，阈值不对"
    assert blocked.status_code == 429
    assert blocked.headers["Retry-After"] == "60"
    assert "频繁" in blocked.json()["error"]


def test_rate_limit_is_counted_per_ip(server):
    """A 被打满不能影响 B —— 否则一个扫描器就能把整站拖死。"""
    with _client(server, "10.60.1.1") as noisy:
        for _ in range(301):
            noisy.get("/api/device")
        assert noisy.get("/api/device").status_code == 429

    with _client(server, "10.60.1.2") as quiet:
        assert quiet.get("/api/device").status_code == 200


def test_set_is_exempt_from_the_global_limit(server):
    """/set 是"用起来就高频"的接口（拖滑块每 60ms 一次），必须豁免。

    未登录访问仍然是 401 —— 豁免的是限流，不是鉴权。
    """
    with _client(server, "10.60.2.1") as client:
        statuses = [client.get("/set?servo=0&angle=90").status_code
                    for _ in range(320)]

    assert set(statuses) == {401}, "豁免路径出现了 429 或其它状态码"


# ---------------------------------------------------------------- 登录爆破

def test_login_is_blocked_after_five_failures(server):
    """5 次密码错误之后，即使密码正确也要被挡住 —— 否则爆破只需换密码。"""
    ip = "10.60.3.1"
    with _client(server, ip) as client:
        client.post("/api/register", json={
            "username": "bruteforce", "password": "correct-pass", "nickname": "靶子",
        })

        failures = [
            client.post("/api/login", json={
                "username": "bruteforce", "password": f"wrong-{i}",
            }).status_code
            for i in range(5)
        ]
        blocked = client.post("/api/login", json={
            "username": "bruteforce", "password": "correct-pass",
        })

    assert failures == [401] * 5
    assert blocked.status_code == 429
    assert int(blocked.headers["Retry-After"]) > 0
    assert "分钟" in blocked.json()["error"]          # 给用户可操作的提示


def test_successful_login_clears_the_failure_counter(server):
    """成功登录要把失败计数清零，否则偶尔手滑几次就会被莫名封号。"""
    ip = "10.60.3.2"
    with _client(server, ip) as client:
        client.post("/api/register", json={
            "username": "slippery", "password": "correct-pass", "nickname": "手滑",
        })
        for _ in range(4):
            assert client.post("/api/login", json={
                "username": "slippery", "password": "wrong",
            }).status_code == 401

        assert client.post("/api/login", json={
            "username": "slippery", "password": "correct-pass",
        }).status_code == 200

        # 计数已清零，又能再失败 4 次而不被封
        for _ in range(4):
            assert client.post("/api/login", json={
                "username": "slippery", "password": "wrong",
            }).status_code == 401


def test_unknown_user_also_counts_toward_the_block(server):
    """不存在的用户名也必须计数，否则攻击者可以用假用户名绕过封禁。"""
    with _client(server, "10.60.3.3") as client:
        for _ in range(5):
            assert client.post("/api/login", json={
                "username": f"ghost-{_}X", "password": "whatever",
            }).status_code == 401

        r = client.post("/api/login", json={
            "username": "ghost-final", "password": "whatever",
        })

    assert r.status_code == 429


# ---------------------------------------------------------------- 注册灌水

def test_register_limit_is_five_accounts_per_hour(server):
    """同一 IP 第 6 个账号必须被拦 —— 这是防脚本灌库的最后一道闸。"""
    with _client(server, "10.60.4.1") as client:
        created = [
            client.post("/api/register", json={
                "username": f"bulk{i:02d}", "password": "pass-123456",
                "nickname": f"批量{i}",
            }).status_code
            for i in range(5)
        ]
        blocked = client.post("/api/register", json={
            "username": "bulk99", "password": "pass-123456", "nickname": "第六个",
        })

    assert created == [200] * 5
    assert blocked.status_code == 429
    assert "5" in blocked.json()["error"]


def test_failed_registration_does_not_consume_the_quota(server):
    """参数不合法不该计入配额 —— 否则用户填错三次就把自己锁死了。"""
    with _client(server, "10.60.4.2") as client:
        for i in range(8):
            assert client.post("/api/register", json={
                "username": "x", "password": "pass-123456", "nickname": f"错{i}",
            }).status_code == 400

        ok = client.post("/api/register", json={
            "username": "finally", "password": "pass-123456", "nickname": "终于",
        })

    assert ok.status_code == 200
