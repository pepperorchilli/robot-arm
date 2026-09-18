"""
手势控制机械臂 —— 主程序

链路：
    摄像头 → MediaPipe 手部关键点 → 手势判定 → HTTP → Node 服务器 → ESP32 → 舵机

两种模式（按 m 切换）：
    1) 指令模式：固定的手势触发固定的动作（张开/握拳/数字…）
    2) 跟随模式：手掌在画面里的位置，实时映射成底座和大臂的角度

运行：
    uv run gesture_control.py                 # 默认连 http://localhost:3000
    uv run gesture_control.py --url http://192.168.1.5:3000
    uv run gesture_control.py --dry-run       # 只识别，不发命令（不接机械臂也能调）
    uv run gesture_control.py --camera 1      # 换一个摄像头

快捷键：
    q 退出    m 切换模式    r 全部回中    d 开/关调试输出

依赖版本说明：
    MediaPipe 锁在 0.10.x + Python 3.12。
    1.0.x 在本机 macOS 上会在初始化时崩溃（DrishtiMetalHelper 报 service unavailable），
    强制 CPU delegate 也绕不过，所以用成熟的 0.10.x。
"""

import argparse
import time

import cv2
import mediapipe as mp

from arm_client import ArmClient
from gestures import classify, describe, fingers_up, palm_center

# ---------------------------------------------------------------
# 常量
# ---------------------------------------------------------------
# 手势被连续识别到多少帧才算「稳定」，防止手在过渡姿势时乱触发
STABLE_FRAMES = 5

# 指令模式：手势 → (说明, 动作)
#   动作接收一个 ArmClient，想发什么就发什么
#   索引对应 SO-ARM101 的 6 个关节：0底座 1肩 2肘 3腕俯仰 4腕旋转 5夹爪
GRIPPER = 5   # 夹爪的舵机编号

GESTURE_ACTIONS = {
    "张开":   ("夹爪张开", lambda c: c.set_servo(GRIPPER, 150)),
    "握拳":   ("夹爪闭合", lambda c: c.set_servo(GRIPPER, 30)),
    "数字1":  ("全部回中", lambda c: c.reset(90)),
    "剪刀手": ("预备姿态", lambda c: c.set_all([90, 120, 60, 90, 90, 150])),
    "数字3":  ("抓取姿态", lambda c: c.set_all([90, 135, 45, 90, 90, 150])),
    "数字4":  ("放下姿态", lambda c: c.set_all([90, 60, 120, 90, 90, 150])),
    "小指":   ("全部复位", lambda c: c.reset(90)),
}


# ---------------------------------------------------------------
# 画图辅助
# ---------------------------------------------------------------
def draw_text(frame, text, y, color=(255, 255, 255), scale=0.7):
    """带黑色描边的文字，画面亮也能看清"""
    cv2.putText(frame, text, (12, y), cv2.FONT_HERSHEY_SIMPLEX, scale,
                (0, 0, 0), 4, cv2.LINE_AA)
    cv2.putText(frame, text, (12, y), cv2.FONT_HERSHEY_SIMPLEX, scale,
                color, 2, cv2.LINE_AA)


# ---------------------------------------------------------------
# 模式处理
# ---------------------------------------------------------------
class CommandMode:
    """指令模式：稳定手势 → 触发一次动作"""

    name = "指令模式"

    def __init__(self, client):
        self.client = client
        self.current = None      # 当前连续识别到的手势
        self.streak = 0          # 连续帧数
        self.last_fired = None   # 上次触发的手势（同一手势不重复触发）

    def update(self, gesture):
        """返回 (状态文本, 是否触发了动作)"""
        if gesture is None:
            self.current, self.streak = None, 0
            return "未识别到手势", False

        if gesture == self.current:
            self.streak += 1
        else:
            self.current, self.streak = gesture, 1

        # 还没稳定，等
        if self.streak < STABLE_FRAMES:
            return f"{gesture} …稳定中({self.streak}/{STABLE_FRAMES})", False

        # 稳定了，但和上次触发的同一个手势 → 不重复发
        if gesture == self.last_fired:
            return f"{gesture}（已执行）", False

        action = GESTURE_ACTIONS.get(gesture)
        if action is None:
            return f"{gesture}（未绑定动作）", False

        label, fn = action
        fn(self.client)
        self.last_fired = gesture
        return f"{gesture} → {label}", True


class FollowMode:
    """跟随模式：手掌位置 → 底座/肩部角度"""

    name = "跟随模式"

    def __init__(self, client):
        self.client = client
        self.last_sent = None

    def update(self, landmarks):
        if landmarks is None:
            return "未识别到手", False

        cx, cy = palm_center(landmarks)
        # 画面坐标：x 向右 0→1，y 向下 0→1
        # 底座左右旋转：手往右 → 底座往右转
        base = int(round((1 - cx) * 180))
        # 肩部俯仰：手往上 → 肩部抬起（画面 y 越小越高）
        arm = int(round(cy * 180))
        base = max(0, min(180, base))
        arm = max(0, min(180, arm))

        # 变化太小就不发，省得刷屏（服务端每次都转发给 ESP32）
        if self.last_sent and abs(base - self.last_sent[0]) < 2 and abs(arm - self.last_sent[1]) < 2:
            return f"底座{base}° 肩部{arm}°（保持）", False

        self.client.set_servo(0, base)
        self.client.set_servo(1, arm)
        self.last_sent = (base, arm)
        return f"底座{base}° 大臂{arm}°", True


# ---------------------------------------------------------------
# 主程序
# ---------------------------------------------------------------
def build_hands():
    """创建 MediaPipe 手部检测器（0.10.x 的 solutions API）"""
    return mp.solutions.hands.Hands(
        static_image_mode=False,      # 视频流模式，会做帧间跟踪，更快更稳
        max_num_hands=1,
        model_complexity=1,
        min_detection_confidence=0.5,
        min_tracking_confidence=0.5,
    )


def main():
    parser = argparse.ArgumentParser(description="手势控制机械臂")
    parser.add_argument("--url", default="http://localhost:3000",
                        help="机械臂服务器地址（默认 http://localhost:3000）")
    parser.add_argument("--camera", type=int, default=0, help="摄像头编号（默认 0）")
    parser.add_argument("--dry-run", action="store_true",
                        help="只识别手势，不发控制命令")
    parser.add_argument("--no-window", action="store_true",
                        help="不弹预览窗口（只打印识别结果）")
    args = parser.parse_args()

    client = ArmClient(args.url)

    if args.dry_run:
        print("[dry-run] 只识别，不发送控制命令")
    elif not client.ping():
        print(f"⚠️  连不上服务器 {args.url}：{client.last_error}")
        print("    机械臂不会动。可以先开服务器，或用 --dry-run 只调识别。")
    else:
        print(f"✅ 已连上服务器 {args.url}")

    hands = build_hands()

    cap = cv2.VideoCapture(args.camera)
    if not cap.isOpened():
        raise SystemExit(
            f"打不开摄像头 {args.camera}。\n"
            "  1) 检查系统设置 → 隐私与安全性 → 摄像头，允许终端访问\n"
            "  2) 换一个编号试试：--camera 1"
        )

    mode = CommandMode(client)
    follow = FollowMode(client)
    use_command_mode = True
    debug = True

    fingers = None
    status = "等待手势…"

    print("\n快捷键：q 退出 | m 切换模式 | r 回中 | d 调试输出\n")

    try:
        while True:
            ok, frame = cap.read()
            if not ok:
                print("读不到摄像头画面，退出")
                break

            # 水平翻转成「照镜子」的效果，看着自然
            # （判定逻辑不用左右手信息，所以翻转不影响识别 —— 见 gestures.py）
            frame = cv2.flip(frame, 1)
            h, w = frame.shape[:2]

            results = hands.process(cv2.cvtColor(frame, cv2.COLOR_BGR2RGB))
            hand = results.multi_hand_landmarks[0] if results.multi_hand_landmarks else None
            landmarks = hand.landmark if hand else None
            fingers = fingers_up(landmarks) if landmarks else None

            # ---- 按模式处理 ----
            if use_command_mode:
                gesture = classify(fingers, with_thumb=True) if fingers else None
                status, fired = mode.update(gesture)
            else:
                status, fired = follow.update(landmarks)

            if fired and debug:
                print(f"[{mode.name if use_command_mode else follow.name}] {status}")

            # ---- 画面叠加信息 ----
            if not args.no_window:
                if hand:
                    mp.solutions.drawing_utils.draw_landmarks(
                        frame, hand, mp.solutions.hands.HAND_CONNECTIONS)

                draw_text(frame, f"模式: {'指令' if use_command_mode else '跟随'}  (m 切换)",
                          32, (0, 220, 255))
                draw_text(frame, status, 66, (255, 255, 255))
                if fingers:
                    draw_text(frame, f"手指: {describe(fingers)}", 100, (180, 255, 180), 0.6)
                draw_text(frame, f"服务器: {args.url}", h - 16, (160, 160, 160), 0.5)
                cv2.imshow("Gesture Control", frame)

            # ---- 键盘 ----
            key = cv2.waitKey(1) & 0xFF
            if key == ord("q"):
                break
            elif key == ord("m"):
                use_command_mode = not use_command_mode
                mode.last_fired = None
                print(f"→ 切换到{'指令' if use_command_mode else '跟随'}模式")
            elif key == ord("r"):
                client.reset(90)
                print("→ 全部回中")
            elif key == ord("d"):
                debug = not debug
                print(f"→ 调试输出{'开' if debug else '关'}")

    except KeyboardInterrupt:
        print("\n中断退出")
    finally:
        cap.release()
        if not args.no_window:
            cv2.destroyAllWindows()
        hands.close()


if __name__ == "__main__":
    main()
