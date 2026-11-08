"""测试夹具（fixtures）。

整体策略是**黑盒集成测试**：
  · 用子进程启动**真实的** server.js，不 import 它的内部函数；
  · 通过 HTTP 和 WebSocket 从外部驱动，和浏览器 / ESP32 走同一条路径；
  · 数据库用独立的 robot_arm_test，每个用例前清空。

为什么不让测试直接 require('./server.js')：那个文件在被 import 时就会
连数据库、监听端口、注册定时器，副作用无法撤销；而且黑盒测的是"部署后
真实跑起来的样子"，比单元测试更接近线上故障。

关于模拟客户端 IP：
  服务器对"同一 IP"有注册限流（5 次/小时）和登录失败封禁（5 次/10 分钟）。
  测试全从 127.0.0.1 发起时，跑到第五个用例就会被自己的防护机制拦住。
  所以每个用例分配一个独立的 X-Forwarded-For（服务器 trust proxy 打开，
  req.ip 取的就是它）—— 既隔离了限流计数，也顺带验证了反向代理下
  真实 IP 能被正确识别。

作用域说明：
  · server 是 module 级的 —— 每个测试文件一个进程，把限流计数、设备连接
    这类进程内状态隔离开，避免用例之间互相污染；
  · clean_db 是 function 级的 —— 每个用例都从空表开始。
"""

from __future__ import annotations

import itertools
import os
import pathlib
import socket
import subprocess
import time
from types import SimpleNamespace

import httpx
import pytest

from helpers import db
from helpers.device import FakeDevice, wait_until

SERVER_DIR = pathlib.Path(__file__).resolve().parents[1]
REPORT_DIR = pathlib.Path(__file__).resolve().parent / "reports"
REPORT_DIR.mkdir(exist_ok=True)

TEST_TOKEN = os.environ.get("TEST_ESP32_TOKEN", "e2e-test-token")
STARTUP_TIMEOUT = float(os.environ.get("TEST_STARTUP_TIMEOUT", "20"))

ADMIN_USER = {"username": "admin01", "password": "admin-pass-123", "nickname": "管理员"}
NORMAL_USER = {"username": "user01", "password": "user-pass-123", "nickname": "普通用户"}

_ip_counter = itertools.count(1)


def next_client_ip() -> str:
    """给每个客户端分配一个互不相同的内网地址，隔离按 IP 的限流计数。"""
    n = next(_ip_counter)
    return f"10.42.{n // 256}.{n % 256 + 1}"


# ---------------------------------------------------------------- 基础设施

def _free_port() -> int:
    """向系统要一个空闲端口，避免和本机 3000 上正在跑的服务撞车。"""
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    def __init__(self, base_url: str, port: int, proc: subprocess.Popen):
        self.base_url = base_url
        self.port = port
        self.proc = proc
        self.device_token = TEST_TOKEN

    @property
    def ws_url(self) -> str:
        """ESP32 连的地址。不带 token —— token 由 FakeDevice 拼上。"""
        return f"ws://127.0.0.1:{self.port}/"

    def get(self, path: str, **kwargs):
        """夹具内部用的匿名请求（健康检查等），固定一个 IP 单独计数。"""
        headers = {"X-Forwarded-For": "10.250.250.1", **kwargs.pop("headers", {})}
        return httpx.get(self.base_url + path, timeout=5, headers=headers, **kwargs)


@pytest.fixture(scope="session", autouse=True)
def prepared_database() -> None:
    """确保测试库有表。没有管理权限时跳过（假定已由脚本建好）。"""
    db.ensure_schema()


@pytest.fixture(scope="module")
def server():
    port = _free_port()
    base_url = f"http://127.0.0.1:{port}"
    log_path = REPORT_DIR / f"server-{port}.log"

    env = {
        **os.environ,
        "PORT": str(port),
        "BIND_HOST": "127.0.0.1",
        "DB_NAME": db.TEST_DB,
        "ESP32_TOKEN": TEST_TOKEN,
    }

    with open(log_path, "w", encoding="utf-8") as log:
        proc = subprocess.Popen(
            ["node", "server.js"],
            cwd=SERVER_DIR,
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
        )

    srv = Server(base_url, port, proc)
    try:
        _wait_ready(srv, log_path)
    except Exception:
        proc.kill()
        proc.wait(timeout=5)
        raise

    yield srv

    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()


def _wait_ready(srv: Server, log_path: pathlib.Path) -> None:
    deadline = time.time() + STARTUP_TIMEOUT
    while time.time() < deadline:
        if srv.proc.poll() is not None:
            tail = log_path.read_text(encoding="utf-8", errors="replace")[-2000:]
            raise RuntimeError(f"server.js 提前退出：\n{tail}")
        try:
            if httpx.get(srv.base_url + "/api-docs.json", timeout=0.5).status_code == 200:
                return
        except httpx.HTTPError:
            pass
        time.sleep(0.2)

    tail = log_path.read_text(encoding="utf-8", errors="replace")[-2000:]
    raise RuntimeError(f"server.js 在 {STARTUP_TIMEOUT:.0f}s 内没有就绪：\n{tail}")


@pytest.fixture(autouse=True)
def clean_db(server):
    """每个用例前清空业务表，保证起点一致。"""
    db.reset()


@pytest.fixture
def make_client(server):
    """创建一个带独立模拟 IP 的 HTTP 客户端。"""
    clients = []

    def _make() -> httpx.Client:
        client = httpx.Client(
            base_url=server.base_url,
            timeout=10,
            headers={"X-Forwarded-For": next_client_ip()},
        )
        clients.append(client)
        return client

    yield _make

    for client in clients:
        client.close()


@pytest.fixture
def api(make_client) -> httpx.Client:
    """匿名客户端：用来测公开接口和未登录行为。"""
    return make_client()


# ---------------------------------------------------------------- 账号

def _register(make_client, user: dict) -> SimpleNamespace:
    with make_client() as client:
        r = client.post("/api/register", json=user)
    assert r.status_code == 200, f"注册失败：{r.status_code} {r.text}"
    token = r.cookies.get("arm_token")
    assert token, "注册成功但没有下发会话 cookie"
    return SimpleNamespace(
        account=r.json()["account"],
        token=token,
        headers={"X-Auth-Token": token},
    )


@pytest.fixture
def admin(make_client, clean_db) -> SimpleNamespace:
    """第一个注册的账号会自动成为管理员。"""
    return _register(make_client, ADMIN_USER)


@pytest.fixture
def user(make_client, admin) -> SimpleNamespace:
    return _register(make_client, NORMAL_USER)


# ---------------------------------------------------------------- 设备

def _device_online(server: Server) -> bool:
    return server.get("/api/device").json().get("online") is True


@pytest.fixture
def device(server, clean_db) -> FakeDevice:
    d = FakeDevice(server.ws_url, TEST_TOKEN).connect()
    wait_until(lambda: _device_online(server), timeout=5, message="假设备没有登记上线")
    yield d
    d.close()
    wait_until(lambda: not _device_online(server), timeout=5,
               message="假设备断开后服务器仍认为在线")
