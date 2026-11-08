"""遥测解析：设备上报的原始寄存器值 → 浏览器能用的物理量。

固件是"哑"的，所有单位换算都在服务器做（见 handleTelemetry）。
这意味着**换算逻辑一旦错了，网页上看到的一切都是错的**，而且不会报错。
所以这一组的重点是：给定一个原始值，断言换算后的每一个字段。
"""

from __future__ import annotations

import time

from helpers.device import wait_until


def _telemetry(api, admin) -> dict:
    r = api.get("/api/telemetry", headers=admin.headers)
    assert r.status_code == 200, r.text
    return r.json()


def _wait_available(api, admin, timeout: float = 3.0) -> dict:
    body = {}

    def ready() -> bool:
        nonlocal body
        body = _telemetry(api, admin)
        return body.get("available") is True

    wait_until(ready, timeout=timeout, message="遥测一直没有变为可用")
    return body


def test_telemetry_unavailable_before_any_report(api, admin, device):
    """设备刚连上、还没上报时，必须是 available:false，不能让前端拿到空对象。"""
    body = _telemetry(api, admin)

    assert body["available"] is False
    assert body["online"] is True


def test_telemetry_is_parsed_into_physical_units(api, admin, device):
    # 关节 0，raw 位置 1000，负载 500，电压 120(×0.1V)，温度 35，电流 10(×6.5mA)
    device.send_telemetry(0, pos=1000, load=500, volt=120, temp=35, current=10)
    body = _wait_available(api, admin)

    joint = body["joints"][0]
    assert joint["pos"] == 1000
    assert joint["deg"] == 87.9              # 1000 × 360 / 4096
    assert joint["angle"] == 12              # 映射到机械行程后的滑块角度
    assert joint["load"] == {"magnitude": 500, "percent": 50.0, "clockwise": False}
    assert joint["volt"] == 12.0             # 寄存器单位 0.1V
    assert joint["temp"] == 35
    assert joint["cur"] == 65                # 寄存器单位 6.5mA


def test_load_direction_bit_is_decoded(api, admin, device):
    """负载寄存器 bit10 是方向位，不是最高位 —— 这个坑值得一条用例盯着。"""
    device.send_telemetry(1, pos=2000, load=500 | 0x400)
    body = _wait_available(api, admin)

    load = body["joints"][1]["load"]
    assert load["magnitude"] == 500          # 方向位不能混进大小
    assert load["clockwise"] is True


def test_joint_index_out_of_range_is_ignored(api, admin, device):
    device.send_telemetry(6, pos=1000)       # 只有 0~5 六个关节
    time.sleep(0.2)

    body = _telemetry(api, admin)
    assert body["available"] is False


def test_malformed_telemetry_is_ignored(api, admin, device):
    device._send("T:1,2,3")                  # 字段不够，必须整条丢弃
    time.sleep(0.2)

    body = _telemetry(api, admin)
    assert body["available"] is False


def test_non_numeric_position_yields_null_angle(api, admin, device):
    """记录当前行为：位置不是数字时不会整条丢弃，而是换算字段变成 null。

    这是已知的健壮性缺口（服务器只校验了字段个数，没校验数值）。
    用例先把这个行为钉住，等修了再改断言 —— 这叫"特征测试"，
    用来防止重构时悄悄改变已有行为。
    """
    device._send("T:0,abc,0,120,30,10")
    time.sleep(0.2)

    body = _telemetry(api, admin)
    assert body["available"] is True
    assert body["joints"][0]["angle"] is None


def test_events_are_recorded_and_returned(api, admin, device):
    device.send_event("collision", joint=1, value=830)
    # 事件本身不产生遥测，得有一条正常的 T: 帧才能从 /api/telemetry 读到
    device.send_telemetry(0, pos=1000)
    body = _wait_available(api, admin)

    events = body["events"]
    assert any(e["type"] == "collision" and e["joint"] == 1 and e["value"] == 830
               for e in events)


def test_torque_event_flips_torque_state(api, admin, device):
    device.send_telemetry(0, pos=1000)
    _wait_available(api, admin)
    assert _telemetry(api, admin)["torqueOn"] is True

    device.send_event("torque", joint="", value=0)
    time.sleep(0.1)

    assert _telemetry(api, admin)["torqueOn"] is False


def test_telemetry_goes_stale_after_three_seconds(api, admin, device):
    """超过 3 秒没有新遥测，数据必须失效 —— 否则仪表盘会一直显示断电前的读数。"""
    device.send_telemetry(0, pos=1000)
    _wait_available(api, admin)

    time.sleep(3.1)

    body = _telemetry(api, admin)
    assert body["available"] is False
    assert body["online"] is True            # 设备还在，只是数据过期了
