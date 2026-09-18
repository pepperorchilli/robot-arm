"""
冒烟测试：确认「摄像头 + MediaPipe + 服务器」三件事都能通。

不接机械臂、不接服务器也能跑（会提示连不上，但不报错）：
    uv run smoke_test.py
"""

import sys

import cv2
import numpy as np

from arm_client import ArmClient
from gesture_control import build_hands
from gestures import classify, describe, fingers_up

FAIL = []


def check(label, ok, detail=""):
    mark = "✅" if ok else "❌"
    print(f"  {mark} {label}" + (f" — {detail}" if detail else ""))
    if not ok:
        FAIL.append(label)


print("=" * 50)
print(" 手势控制 · 冒烟测试")
print("=" * 50)

# ---------------- 1. MediaPipe ----------------
print("\n[1] MediaPipe 手部检测")
try:
    hands = build_hands()
    check("检测器创建", True, "0.10.x solutions API")

    blank = np.zeros((480, 640, 3), dtype=np.uint8)
    res = hands.process(cv2.cvtColor(blank, cv2.COLOR_BGR2RGB))
    n = 0 if res.multi_hand_landmarks is None else len(res.multi_hand_landmarks)
    check("空画面推理", True, f"检测到手数={n}（应为 0）")
    hands.close()
except Exception as e:
    check("MediaPipe", False, str(e))

# ---------------- 2. 手势判定 ----------------
print("\n[2] 手势判定逻辑")


class LM:
    def __init__(self, x, y):
        self.x, self.y = x, y


def fake_hand(extended):
    """造一只假手：extended 是 5 个布尔值"""
    lms = [LM(0.5, 0.5) for _ in range(21)]
    lms[0] = LM(0.5, 0.9)   # 手腕
    lms[9] = LM(0.5, 0.7)   # 中指掌指关节
    pairs = [(4, 3, 0.30), (8, 6, 0.40), (12, 10, 0.50), (16, 14, 0.60), (20, 18, 0.70)]
    for (tip, pip, x), up in zip(pairs, extended):
        lms[pip] = LM(x, 0.5)
        lms[tip] = LM(x, 0.2 if up else 0.72)
    return lms


cases = [
    ([False] * 5, "握拳"),
    ([True] * 5, "张开"),
    ([False, True, False, False, False], "数字1"),
    ([False, True, True, False, False], "剪刀手"),
]
for fingers_expected, want in cases:
    got_fingers = fingers_up(fake_hand(fingers_expected))
    got = classify(got_fingers, with_thumb=True)
    check(f"{want}", got == want, f"识别为 {got}（手指: {describe(got_fingers)}）")

# ---------------- 3. 摄像头 ----------------
print("\n[3] 摄像头")
cap = cv2.VideoCapture(0)
opened = cap.isOpened()
check("打开摄像头 0", opened)
if opened:
    ok, frame = cap.read()
    check("读取一帧", ok, f"尺寸 {frame.shape}" if ok else "")
    cap.release()

# ---------------- 4. 服务器 ----------------
print("\n[4] 机械臂服务器")
client = ArmClient("http://localhost:3000")
if client.ping():
    check("连接服务器", True, "http://localhost:3000")
else:
    print(f"  ⚠️  连不上 http://localhost:3000（{client.last_error[:50]}）")
    print("     这不影响手势识别本身。要控制机械臂时先启动：cd server && npm start")

# ---------------- 汇总 ----------------
print("\n" + "=" * 50)
if FAIL:
    print(f" 结果：{len(FAIL)} 项失败 → {', '.join(FAIL)}")
    sys.exit(1)
print(" 结果：全部通过 ✅")
print("=" * 50)
print("\n启动手势控制：uv run gesture_control.py")
