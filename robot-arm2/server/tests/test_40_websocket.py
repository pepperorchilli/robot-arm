"""WebSocket 连接管理：设备身份、顶号、断线清理。

服务器同一时刻只保留**一台**设备连接。这类"单实例连接"最容易出的问题不是
功能，而是状态：旧连接没清干净、新连接被误判成离线、设备走了遥测还挂着。
这一组专门钉这些行为。
"""

from __future__ import annotations

import contextlib
import time

from websocket import (
    WebSocketConnectionClosedException,
    create_connection,
)

from helpers.device import FakeDevice, wait_until


def _online(api) -> bool:
    return api.get("/api/device").json()["online"]


def test_connection_without_token_is_rejected(server):
    ws = create_connection(server.ws_url, timeout=5)
    try:
        ws.settimeout(2)
        try:
            data = ws.recv()
            assert data in ("", b"")     # 服务器直接关连接
        except WebSocketConnectionClosedException:
            pass
    finally:
        with contextlib.suppress(Exception):
            ws.close()


def test_connection_with_wrong_token_is_rejected(server):
    device = FakeDevice(server.ws_url, "definitely-not-the-token")
    closed = False
    try:
        device.connect()
        closed = device.is_closed()
    except Exception:
        # 握手阶段就被拒也算拒绝，两种表现都接受
        closed = True
    finally:
        device.close()

    assert closed, "token 错误却保持了连接"


def test_correct_token_registers_device(api, device):
    assert _online(api) is True


def test_new_connection_replaces_the_old_one(api, device, server):
    """设备重连时旧连接往往还没被 TCP 检测为断开，必须由新连接主动顶掉。"""
    second = FakeDevice(server.ws_url, server.device_token).connect()
    try:
        wait_until(lambda: device.is_closed(timeout=0.2), timeout=3,
                   message="旧连接没有被新连接顶掉")
        assert _online(api) is True, "顶号过程中设备不应被判为离线"
    finally:
        second.close()

    wait_until(lambda: _online(api) is False, timeout=3,
               message="新连接断开后设备仍被认为在线")


def test_disconnect_clears_telemetry(api, admin, server):
    """设备走了，最后一帧遥测必须失效 —— 否则仪表盘会一直显示断线前的读数。"""
    device = FakeDevice(server.ws_url, server.device_token).connect()
    try:
        device.send_telemetry(0, pos=1000)
        wait_until(
            lambda: api.get("/api/telemetry", headers=admin.headers).json()["available"],
            timeout=3,
            message="遥测没有变为可用",
        )
    finally:
        device.close()

    wait_until(lambda: _online(api) is False, timeout=3, message="设备仍在在线状态")
    time.sleep(0.1)

    body = api.get("/api/telemetry", headers=admin.headers).json()
    assert body["available"] is False
    assert body["online"] is False


def test_arrival_report_does_not_break_the_connection(api, admin, device):
    """固定格式的到位回报（0:90,1:90,...）是给前端看的，服务器只记录不解析。"""
    device.send_arrival([90, 90, 90, 90, 90, 90])
    time.sleep(0.1)

    # 连接仍在，后续指令还能正常下发
    assert _online(api) is True
    r = api.get("/set?servo=0&angle=10", headers=admin.headers)
    assert r.status_code == 200
    assert device.recv_command() == "0:10"
