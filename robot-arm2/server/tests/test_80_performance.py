"""性能基线：遥控链路的端到端时延。

这里测的不是"服务器能扛多少 QPS"，而是一件更具体的事：
**滑块拖到某个角度，机械臂多久开始动。**

链路上有三次转发：浏览器 → HTTP → 服务器 → WebSocket → ESP32 → 舵机。
其中"从 HTTP 请求发出到设备收到命令"这一段完全在服务器控制范围内，
所以它是可测、可回归的；舵机自己的运动时间不在此列。

阈值故意定得很宽（p95 < 200ms）：这个用例的目的是**发现退化**，
不是卡死某个数字。换一台 CI 机器，绝对值会变，但同一台机器上
昨天的 3ms 变成今天的 80ms，就说明有东西退化了。

结果同时写进 reports/perf.json，方便在 CI 里做趋势对比。
"""

from __future__ import annotations

import json
import math
import pathlib
import time

import pytest

SAMPLES = 200
REPORT = pathlib.Path(__file__).parent / "reports" / "perf.json"


def _percentile(values: list[float], p: float) -> float:
    """最近秩法（nearest-rank）：p95 就是第 ceil(0.95n) 小的那个值。"""
    ordered = sorted(values)
    rank = max(1, math.ceil(p / 100 * len(ordered)))
    return ordered[rank - 1]


def _summarize(name: str, latencies: list[float]) -> dict:
    stats = {
        "name": name,
        "samples": len(latencies),
        "min_ms": round(min(latencies), 2),
        "p50_ms": round(_percentile(latencies, 50), 2),
        "p95_ms": round(_percentile(latencies, 95), 2),
        "p99_ms": round(_percentile(latencies, 99), 2),
        "max_ms": round(max(latencies), 2),
    }
    print(f"\n{name}: " + ", ".join(f"{k}={v}" for k, v in stats.items() if k != "name"))

    REPORT.parent.mkdir(exist_ok=True)
    history = json.loads(REPORT.read_text()) if REPORT.exists() else []
    history = [*history, stats][-50:]        # 只留最近 50 次，避免文件无限增长
    REPORT.write_text(json.dumps(history, indent=2, ensure_ascii=False), encoding="utf-8")
    return stats


@pytest.mark.perf
def test_set_command_round_trip(api, admin, device):
    """HTTP 请求发出 → 设备收到 `关节:角度` 的完整时延。

    这条路径上任何一处退化（事件循环被阻塞、日志同步写盘、
    多余的数据库查询）都会直接反映在这里。
    """
    latencies = []

    for i in range(SAMPLES):
        started = time.perf_counter()
        r = api.get(f"/set?servo=0&angle={i % 180}", headers=admin.headers)
        assert r.status_code == 200
        assert device.recv_command(timeout=2) == f"0:{i % 180}"
        latencies.append((time.perf_counter() - started) * 1000)

    stats = _summarize("set_round_trip", latencies)

    assert stats["p95_ms"] < 200, f"p95 退化到 {stats['p95_ms']}ms"
    assert stats["p99_ms"] < 500, f"p99 退化到 {stats['p99_ms']}ms"


@pytest.mark.perf
def test_telemetry_read_latency(make_client, admin, device):
    """仪表盘每 500ms 轮询一次遥测，单次读取必须远快于轮询间隔。"""
    device.send_telemetry(0, pos=1000)
    client = make_client()

    latencies = []
    for _ in range(SAMPLES):
        started = time.perf_counter()
        assert client.get("/api/telemetry", headers=admin.headers).status_code == 200
        latencies.append((time.perf_counter() - started) * 1000)

    stats = _summarize("telemetry_read", latencies)

    assert stats["p95_ms"] < 50, f"p95 退化到 {stats['p95_ms']}ms，已接近轮询间隔"
