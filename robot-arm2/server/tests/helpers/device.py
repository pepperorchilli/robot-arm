"""假 ESP32 —— 测试用的设备替身。

为什么要有它：服务器的大部分行为依赖一台"连着的外部设备"，而这台设备
不能出现在 CI 里。测试替身（Test Double）就是把这些不可控依赖变成可控的，
这是测开最基本的一项能力。

和 server/simulator/fake-esp32.js 的分工：
  · 那个是**手工联调**用的，带物理模型、会自己动；
  · 这个是**测试用**的，完全被动 —— 只在你下指令时才说话，
    这样断言才不会随机失败。
"""

from __future__ import annotations

import contextlib
import time
from collections.abc import Callable

from websocket import (
    WebSocketConnectionClosedException,
    WebSocketTimeoutException,
    create_connection,
)

# 服务器关了连接时，recv() 可能返回空串，也可能抛异常 —— 两种都算"已关闭"。
# 注意：较新版本的 websocket-client 用 WebSocketConnectionClosedException，
# 旧文档里的 ConnectionClosed 已经不再从顶层导出。
CLOSED_EXCEPTIONS = (WebSocketConnectionClosedException, WebSocketTimeoutException)


class FakeDevice:
    """以 ESP32 的身份连上服务器，并记录它收到的所有指令。"""

    def __init__(self, url: str, token: str):
        self.url = url
        self.token = token
        self.ws = None

    # ---------- 连接 ----------

    def connect(self, timeout: float = 5.0) -> FakeDevice:
        sep = "&" if "?" in self.url else "?"
        self.ws = create_connection(f"{self.url}{sep}token={self.token}", timeout=timeout)
        return self

    def close(self) -> None:
        if self.ws is not None:
            with contextlib.suppress(Exception):
                self.ws.close()
            self.ws = None

    def __enter__(self) -> FakeDevice:
        return self.connect()

    def __exit__(self, *exc) -> None:
        self.close()

    # ---------- 上报（模拟固件） ----------

    def send_telemetry(
        self,
        joint: int,
        pos: int,
        load: int = 0,
        volt: int = 120,
        temp: int = 30,
        current: int = 10,
    ) -> None:
        """T:<关节>,<位置>,<负载>,<电压×10>,<温度>,<电流>"""
        self._send(f"T:{joint},{pos},{load},{volt},{temp},{current}")

    def send_event(self, event_type: str, joint: int | str = "", value: int | str = "") -> None:
        """E:<类型>,<关节>,<数值>，例如 E:collision,1,830"""
        self._send(f"E:{event_type},{joint},{value}")

    def send_arrival(self, angles: list[int]) -> None:
        """到位回报，老格式：0:90,1:90,..."""
        self._send(",".join(f"{i}:{a}" for i, a in enumerate(angles)))

    def _send(self, text: str) -> None:
        assert self.ws is not None, "先调用 connect()"
        self.ws.send(text)

    # ---------- 接收（服务器下发的指令） ----------

    def recv_command(self, timeout: float = 2.0) -> str | None:
        """收一条服务器下发的指令；超时返回 None。"""
        assert self.ws is not None, "先调用 connect()"
        self.ws.settimeout(timeout)
        try:
            data = self.ws.recv()
        except CLOSED_EXCEPTIONS:
            return None
        if isinstance(data, bytes):
            data = data.decode("utf-8")
        return data or None

    def drain(self, timeout: float = 0.3) -> list[str]:
        """把已到达的指令全部取走，返回列表。"""
        out = []
        while True:
            cmd = self.recv_command(timeout=timeout)
            if cmd is None:
                return out
            out.append(cmd)

    def is_closed(self, timeout: float = 2.0) -> bool:
        """服务器是否已经关掉了这条连接。"""
        assert self.ws is not None, "先调用 connect()"
        self.ws.settimeout(timeout)
        try:
            data = self.ws.recv()
        except WebSocketConnectionClosedException:
            return True
        except WebSocketTimeoutException:
            return False
        return data in (b"", "")   # 正常关闭时 ws 会返回空


def wait_until(
    predicate: Callable[[], bool],
    timeout: float = 5.0,
    interval: float = 0.05,
    message: str = "条件未在超时时间内满足",
) -> None:
    """轮询等待，避免用固定 sleep 造成随机失败（flaky test 的主要来源）。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return
        time.sleep(interval)
    raise AssertionError(f"{message}（等待 {timeout:.1f}s）")
