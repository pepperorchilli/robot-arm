"""
机械臂通信客户端

手势模块在系统里的位置：
    摄像头 → [本模块] → HTTP → Node 服务器 → WebSocket → ESP32 → 舵机

也就是说，手势识别只是「另一个客户端」，和网页遥控是平级的。

鉴权：控制接口需要登录，所以本模块会先用密码换一个 token。
      密码来源优先级：构造参数 > 环境变量 ARM_PASSWORD。
      本地开发时可以直接 export ARM_PASSWORD=xxx，不用改代码。
"""

import os

import requests

# 服务端默认地址与端口（见 server/config.js 的 PORT）
DEFAULT_BASE_URL = "http://localhost:3000"

# 舵机编号 -> 名称，和网页遥控页保持一致（SO-ARM101 的 6 个关节）
SERVO_NAMES = ("底座", "肩部", "肘部", "腕俯仰", "腕旋转", "夹爪")
NUM_SERVOS = len(SERVO_NAMES)

ANGLE_MIN, ANGLE_MAX = 0, 180


class ArmClient:
    """封装对机械臂服务器的调用"""

    def __init__(self, base_url=DEFAULT_BASE_URL, timeout=2.0, password=None):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.password = password or os.environ.get("ARM_PASSWORD")
        self.token = None          # 登录后拿到的凭据
        self.last_error = None

    # ---------------- 鉴权 ----------------

    def login(self):
        """用密码换 token。没配密码就跳过（服务器可能没开鉴权）"""
        if self.token:
            return True
        if not self.password:
            self.last_error = "未配置密码（构造参数或环境变量 ARM_PASSWORD）"
            return False

        try:
            resp = requests.post(
                f"{self.base_url}/api/login",
                json={"password": self.password},
                timeout=self.timeout,
            )
        except requests.RequestException as e:
            self.last_error = str(e)
            return False

        if resp.status_code != 200:
            self.last_error = "登录失败：密码错误" if resp.status_code == 401 else f"HTTP {resp.status_code}"
            return False

        # token 在 Set-Cookie 里
        self.token = resp.cookies.get("arm_token")
        if not self.token:
            self.last_error = "登录成功但没拿到 token"
            return False

        self.last_error = None
        return True

    # ---------------- 底层 ----------------

    def _get(self, path, params):
        headers = {"X-Auth-Token": self.token} if self.token else {}

        try:
            resp = requests.get(
                f"{self.base_url}{path}", params=params,
                headers=headers, timeout=self.timeout,
            )
        except requests.RequestException as e:
            # 服务器没开 / 网络不通：记录下来，但不要让程序崩掉
            self.last_error = str(e)
            return False

        # 401：token 过期或没登录 → 重登一次再试
        if resp.status_code == 401 and self.password:
            self.token = None
            if self.login():
                return self._get(path, params)
            return False

        if resp.status_code != 200:
            self.last_error = f"HTTP {resp.status_code}: {resp.text}"
            return False

        self.last_error = None
        return True

    # ---------------- 对外 ----------------

    def set_servo(self, index, angle):
        """设置单个舵机角度。index 从 0 开始（0=底座 … 5=夹爪）"""
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
        """探活：能连上服务器就返回 True（这个接口不需要登录）"""
        try:
            resp = requests.get(f"{self.base_url}/api/auth", timeout=self.timeout)
            self.last_error = None
            return resp.status_code == 200
        except requests.RequestException as e:
            self.last_error = str(e)
            return False
