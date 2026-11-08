"""留言板：发帖、回复、删除、级联清理。

这一组对应测开 JD 里的"业务接口测试"，重点是三件事：
  · 字段的所有权 —— 昵称必须来自登录账号，不能让客户端自带；
  · 权限 —— 发帖人人可发，回复和删除只有管理员；
  · 数据一致性 —— 删留言时，它下面的回复必须一起消失（外键级联）。

最后还有一条长度边界的用例，用 xfail 记下当前实现的一个缺口。
"""

from __future__ import annotations

import pytest


def _post(api, headers, content):
    return api.post("/api/messages", json={"content": content}, headers=headers)


def _list(api, headers) -> list[dict]:
    r = api.get("/api/messages", headers=headers)
    assert r.status_code == 200, r.text
    return r.json()


# ---------------------------------------------------------------- 权限

def test_listing_requires_login(api):
    assert api.get("/api/messages").status_code == 401


def test_posting_requires_login(api):
    assert api.post("/api/messages", json={"content": "匿名留言"}).status_code == 401


def test_deleting_requires_login(api):
    assert api.delete("/api/messages/1").status_code == 401


def test_normal_user_cannot_delete(api, user):
    """越权测试：普通用户绕过前端直接调删除接口，必须在鉴权层被挡。"""
    msg = _post(api, user.headers, "用户自己的留言").json()

    r = api.delete(f"/api/messages/{msg['id']}", headers=user.headers)

    assert r.status_code == 403
    assert "管理员" in r.json()["error"]


# ---------------------------------------------------------------- 发帖

@pytest.mark.parametrize("content", ["", "   ", "\n\t "])
def test_empty_content_is_rejected(api, user, content):
    r = _post(api, user.headers, content)

    assert r.status_code == 400
    assert "不能为空" in r.json()["error"]


def test_nickname_comes_from_the_account_not_the_request(api, user):
    """客户端传 nickname 必须被忽略 —— 否则可以冒充管理员发公告。"""
    r = api.post("/api/messages", json={
        "content": "我是管理员",
        "nickname": "管理员",          # 想冒充
    }, headers=user.headers)

    assert r.status_code == 200
    assert r.json()["nickname"] == user.account["nickname"]
    assert r.json()["nickname"] != "管理员"


def test_content_is_trimmed(api, user):
    r = _post(api, user.headers, "  两边有空格  ")

    assert r.json()["content"] == "两边有空格"


def test_newest_message_is_listed_first(api, user):
    _post(api, user.headers, "第一条")
    _post(api, user.headers, "第二条")

    contents = [m["content"] for m in _list(api, user.headers)]

    assert contents == ["第二条", "第一条"]


def test_message_shape_matches_the_frontend_contract(api, user):
    body = _post(api, user.headers, "结构检查").json()

    assert set(body) == {"id", "nickname", "content", "time", "replies"}
    assert isinstance(body["id"], int)
    assert isinstance(body["time"], int)     # 毫秒时间戳，前端直接 new Date()
    assert body["time"] > 1_600_000_000_000
    assert body["replies"] == []


# ---------------------------------------------------------------- 回复

def test_admin_reply_nests_under_the_message(api, admin, user):
    msg = _post(api, user.headers, "请问第二代什么时候出？").json()

    r = api.post(f"/api/messages/{msg['id']}/reply",
                 json={"content": "已经出了"}, headers=admin.headers)

    assert r.status_code == 200
    body = r.json()
    assert body["id"] == msg["id"]
    assert [rep["content"] for rep in body["replies"]] == ["已经出了"]

    # 列表接口看到的也是一样的结构
    listed = _list(api, user.headers)
    assert [rep["content"] for rep in listed[0]["replies"]] == ["已经出了"]


def test_replies_are_ordered_oldest_first(api, admin, user):
    msg = _post(api, user.headers, "连续回复").json()

    for text in ("回复一", "回复二"):
        api.post(f"/api/messages/{msg['id']}/reply",
                 json={"content": text}, headers=admin.headers)

    listed = _list(api, user.headers)
    assert [rep["content"] for rep in listed[0]["replies"]] == ["回复一", "回复二"]


def test_reply_to_missing_message_is_404(api, admin):
    r = api.post("/api/messages/999999/reply",
                 json={"content": "对着空气回复"}, headers=admin.headers)

    assert r.status_code == 404
    assert "不存在" in r.json()["error"]


def test_reply_to_deleted_message_is_404(api, admin, user):
    msg = _post(api, user.headers, "马上要被删").json()
    assert api.delete(f"/api/messages/{msg['id']}",
                      headers=admin.headers).status_code == 200

    r = api.post(f"/api/messages/{msg['id']}/reply",
                 json={"content": "晚了"}, headers=admin.headers)

    assert r.status_code == 404


# ---------------------------------------------------------------- 删除

def test_admin_delete_removes_message_and_replies(api, admin, user):
    """级联删除必须生效，否则库里会留下指不到留言的孤儿回复。"""
    msg = _post(api, user.headers, "会被删掉").json()
    api.post(f"/api/messages/{msg['id']}/reply",
             json={"content": "也会被删掉"}, headers=admin.headers)

    r = api.delete(f"/api/messages/{msg['id']}", headers=admin.headers)

    assert r.status_code == 200
    assert r.json() == {"ok": True}
    assert _list(api, admin.headers) == []


def test_delete_only_affects_the_target_message(api, admin, user):
    keep = _post(api, user.headers, "保留").json()
    drop = _post(api, user.headers, "删除").json()

    api.delete(f"/api/messages/{drop['id']}", headers=admin.headers)

    ids = [m["id"] for m in _list(api, admin.headers)]
    assert ids == [keep["id"]]


def test_delete_missing_message_is_404(api, admin):
    r = api.delete("/api/messages/999999", headers=admin.headers)

    assert r.status_code == 404


# ---------------------------------------------------------------- 长度边界

def test_content_of_500_chars_is_accepted(api, user):
    """500 正好是数据库列的宽度，必须能存下 —— 不能少一个字节。"""
    content = "字" * 500

    r = _post(api, user.headers, content)

    assert r.status_code == 200
    assert r.json()["content"] == content


@pytest.mark.xfail(
    reason="已知缺陷：接口没有长度校验，超长内容直接撞数据库报错变成 500",
    strict=True,
)
def test_content_over_500_chars_is_rejected_with_400(api, user):
    """OpenAPI 里写了 maxLength: 500，但代码没有校验。

    现在超长内容会触发 MySQL 的 STRICT_TRANS_TABLES 报错，
    再被兜底处理成 500 —— 客户端拿不到"你写太长了"这种可操作的提示。
    """
    r = _post(api, user.headers, "字" * 501)

    assert r.status_code == 400
