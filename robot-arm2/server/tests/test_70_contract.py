"""契约测试：OpenAPI 文档 vs 真实实现。

文档不是注释，是对外承诺 —— 前端、Postman 集合、未来的第三方调用方
都按它写代码。文档和实现一旦分叉，调用方就会踩坑，而且没有任何告警。
这一组做三个方向的核对：

  1. 代码里每个 API 路由都在文档里（漏了 = 调用方看不见它）；
  2. 文档里的路径在代码里真的存在（写错路径 = 调用方 404）；
  3. 文档声明的状态码、数据结构与真实响应一致。

第 2 条顺带验证了一件事：匿名访问每个文档里声明 401 的接口，
返回的必须是 401 而不是 404 —— 404 说明路由根本不存在，文档在骗人。
"""

from __future__ import annotations

import pathlib
import re

import httpx
import pytest

SERVER_DIR = pathlib.Path(__file__).resolve().parents[1]

# 从源码里把路由表抠出来（只读文本，不 require，不影响被测进程）
_ROUTE_RE = re.compile(r"app\.(get|post|put|delete)\(\s*'([^']+)'")


def _app_routes() -> list[tuple[str, str]]:
    source = (SERVER_DIR / "server.js").read_text(encoding="utf-8")
    routes = set()
    for method, path in _ROUTE_RE.findall(source):
        if path.startswith("/api-docs"):
            continue                      # 文档服务本身不写进文档
        if path.startswith("/api") or path == "/set":
            routes.add((method.upper(), path))
    return sorted(routes)


APP_ROUTES = _app_routes()


def _spec_path(path: str) -> str:
    """Express 的 :id → OpenAPI 的 {id}"""
    return re.sub(r":(\w+)", r"{\1}", path)


@pytest.fixture(scope="module")
def spec(server) -> dict:
    r = httpx.get(server.base_url + "/api-docs.json", timeout=5,
                  headers={"X-Forwarded-For": "10.51.0.1"})
    assert r.status_code == 200
    return r.json()


# ---------------------------------------------------------------- 双向覆盖

@pytest.mark.parametrize("method,path", [
    pytest.param(
        method, path,
        marks=pytest.mark.xfail(
            strict=True,
            reason="已知缺陷：/api/auth 是 /api/me 的别名，但没写进 OpenAPI 文档",
        ) if path == "/api/auth" else (),
    )
    for method, path in APP_ROUTES
])
def test_route_is_documented(spec, method, path):
    """代码里有的接口，文档里必须有 —— 否则前后端联调时它是隐形的。"""
    ops = spec["paths"].get(_spec_path(path))

    assert ops is not None, f"{method} {path} 没有出现在 OpenAPI 文档里"
    assert method.lower() in ops, f"{path} 在文档里没有 {method} 方法"


def test_documented_paths_exist_in_code(spec):
    """反向：文档里有的路径，代码里必须有 —— 拼错一个字母就是全部 404。"""
    known = {_spec_path(path) for _, path in APP_ROUTES}
    missing = sorted(set(spec["paths"]) - known)

    assert missing == [], f"文档里有、代码里没有的路径：{missing}"


# ---------------------------------------------------------------- 状态码

def _sample_body(op: dict, spec: dict) -> dict | None:
    """按文档声明的 requestBody 造一个字段齐全但值无意义的请求体。

    测"未登录返回 401"时，请求体必须是合法的 —— 否则测的就变成了
    "空请求体会不会崩"，那是另一件事（见 test_10_auth.py 里的空字段用例）。
    """
    content = op.get("requestBody", {}).get("content", {}).get("application/json")
    if not content:
        return None

    schema = content.get("schema", {})
    ref = schema.get("$ref")
    if ref:
        schema = spec["components"]["schemas"][ref.rsplit("/", 1)[-1]]
    return {name: "placeholder" for name in schema.get("required", [])}


def test_documented_401_actually_happens(server, spec):
    """文档承诺"未登录返回 401"的接口，逐个匿名打一遍。

    这条同时是路由存在性检查：路径写错会得到 404，而不是 401。
    """
    request_headers = {"X-Forwarded-For": "10.51.0.2"}
    checked, failures = 0, []

    for path, ops in spec["paths"].items():
        for method, op in ops.items():
            if "401" not in op.get("responses", {}):
                continue
            url = path.replace("{id}", "1")
            r = httpx.request(method.upper(), server.base_url + url,
                              headers=request_headers, timeout=5,
                              json=_sample_body(op, spec))
            checked += 1
            if r.status_code != 401:
                failures.append(f"{method.upper()} {path} 文档写 401，实际 {r.status_code}")

    assert checked >= 5, "带 401 声明的接口少于 5 个，文档可能退化了"
    assert failures == [], "\n".join(failures)


# ---------------------------------------------------------------- 数据结构

def test_message_matches_the_documented_schema(api, user, spec):
    schema = spec["components"]["schemas"]["Message"]

    body = api.post("/api/messages", json={"content": "契约"},
                    headers=user.headers).json()

    assert set(body) == set(schema["properties"])


def test_reply_matches_the_documented_schema(api, admin, user, spec):
    schema = spec["components"]["schemas"]["Reply"]
    msg = api.post("/api/messages", json={"content": "契约"},
                   headers=user.headers).json()

    body = api.post(f"/api/messages/{msg['id']}/reply",
                    json={"content": "回复"}, headers=admin.headers).json()

    assert set(body["replies"][0]) == set(schema["properties"])


def test_error_bodies_follow_the_error_schema(api, admin):
    """所有错误响应都是 {error: string}，前端才能用一个分支统一展示。"""
    responses = [
        api.post("/api/register", json={
            "username": "ab", "password": "pass-123456", "nickname": "短"}),
        api.post("/api/login", json={"username": "nobody", "password": "pass-123456"}),
        api.post("/api/messages/999999/reply", json={"content": "x"},
                 headers=admin.headers),
    ]

    for r in responses:
        assert r.status_code >= 400, r.text
        body = r.json()
        assert set(body) == {"error"}, f"{r.request.url} 的错误体结构不对：{body}"
        assert isinstance(body["error"], str) and body["error"]


# ---------------------------------------------------------------- 参数约束

@pytest.mark.xfail(
    strict=True,
    reason="已知缺陷：文档声明 servo 0-5、angle 0-180，实现完全不校验就越过边界下发",
)
def test_set_enforces_the_documented_range(api, admin, device):
    """/set 的文档写了取值范围，代码里却一个判断都没有。

    超范围的值会被原样拼成命令发给固件（"9:999"），
    由固件去决定是忽略还是乱动 —— 这不是接口该有的行为。
    """
    r = api.get("/set?servo=9&angle=999", headers=admin.headers)

    assert r.status_code == 400
