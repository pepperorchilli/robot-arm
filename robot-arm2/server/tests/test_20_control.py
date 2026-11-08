"""遥控链路：浏览器 → 服务器 → 设备。

这是整个系统唯一的"写"路径，也是最需要测试的一条：
  · 设备不在线时，服务器必须明确返回 503，而不是假装成功；
  · 设备在线时，命令要**原样**到达设备 —— 中间任何转换都是 bug；
  · 边界值（夹爪档位）要被截断，不能让非法值透传到固件。
"""

from __future__ import annotations


def test_set_without_device_returns_503(api, admin):
    """没有设备时下发指令，必须返回 503 —— 前端靠这个状态码判断离线。"""
    r = api.get("/set?servo=0&angle=90", headers=admin.headers)

    assert r.status_code == 503
    assert "未连接" in r.text


def test_device_online_after_connect(api, device):
    body = api.get("/api/device").json()

    assert body["online"] is True
    assert body["onlineSeconds"] >= 0


def test_set_forwards_command_verbatim(api, admin, device):
    """命令格式是 `关节:角度`，一个字符都不能改。"""
    r = api.get("/set?servo=3&angle=45", headers=admin.headers)

    assert r.status_code == 200
    assert device.recv_command() == "3:45"


def test_set_returns_the_angle_it_sent(api, admin, device):
    r = api.get("/set?servo=1&angle=120", headers=admin.headers)

    assert r.text == "120"


def test_grip_step_is_clamped_to_twenty(api, admin, device):
    """夹持档位是 [-20, 20]，超出范围要截断，不能把 999 透传给固件。"""
    r = api.get("/api/grip?step=999", headers=admin.headers)

    assert r.status_code == 200
    assert r.text == "20"
    assert device.recv_command() == "Z:20"


def test_grip_negative_step_is_clamped(api, admin, device):
    r = api.get("/api/grip?step=-999", headers=admin.headers)

    assert r.text == "-20"
    assert device.recv_command() == "Z:-20"


def test_grip_without_device_returns_503(api, admin):
    r = api.get("/api/grip?step=1", headers=admin.headers)

    assert r.status_code == 503


def test_commands_preserve_order(api, admin, device):
    """连续下发多条指令，设备应按顺序收到 —— 乱序会让机械臂走错轨迹。"""
    for angle in (10, 20, 30):
        api.get(f"/set?servo=0&angle={angle}", headers=admin.headers)

    assert device.drain() == ["0:10", "0:20", "0:30"]
