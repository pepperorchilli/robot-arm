"""
机械臂通信客户端

手势模块在系统里的位置：
    摄像头 → [本模块] → HTTP → Node 服务器 → WebSocket → ESP32 → 舵机

也就是说，手势识别只是「另一个客户端」，和网页遥控是平级的。
现有服务器一行都不用改，直接把命令发到同一个 /set 接口。
"""

import requests

# 服务端默认地址与端口（见 server/config.js 的 PORT）
DEFAULT_BASE_URL = "http://localhost:3000"

# 舵机编号 -> 名称，和网页遥控页保持一致
SERVO_NAMES = ("底座", "大臂", "小臂", "手腕", "夹爪")
NUM_SERVOS = len(SERVO_NAMES)

ANGLE_MIN, ANGLE_MAX = 0, 180


class ArmClient:
    """封装对机械臂服务器的调用"""

    def __init__(self, base_url=DEFAULT_BASE_URL, timeout=2.0):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.last_error = None

    # ---------------- 底层 ----------------
    def _get(self, path, params):
        try:
            resp = requests.get(
                f"{self.base_url}{path}", params=params, timeout=self.timeout
            )
            self.last_error = None
            if resp.status_code != 200:
                self.last_error = f"HTTP {resp.status_code}: {resp.text}"
                return False
            return True
        except requests.RequestException as e:
            # 服务器没开 / 网络不通：记录下来，但不要让程序崩掉
            self.last_error = str(e)
            return False

    # ---------------- 对外 ----------------
    def set_servo(self, index, angle):
        """设置单个舵机角度。index 从 0 开始（0=底座 … 4=夹爪）"""
        index = max(0, min(NUM_SERVOS - 1, int(index)))
        angle = max(ANGLE_MIN, min(ANGLE_MAX, int(angle)))
        return self._get("/set", {"servo": index, "angle": angle})

    def set_all(self, angles):
        """一次设置多个舵机（逐个发，服务端接口就是单个的）"""
        ok = True
        for i in range(min(len(angles), NUM_SERVOS)):
            if not self.set_servo(i, angles[i]):
                ok = False
        return ok

    def reset(self, angle=90):
        """全部回中"""
        return self.set_all([angle] * NUM_SERVOS)

    def ping(self):
        """探活：能连上服务器就返回 True"""
        return self._get("/", {})
