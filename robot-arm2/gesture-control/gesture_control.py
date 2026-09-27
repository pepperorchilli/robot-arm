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

控制接口要登录，用户名/密码这样给（详见 arm_client.py 开头的说明）：
    ARM_USERNAME=qiudai ARM_PASSWORD=xxx uv run gesture_control.py
    uv run gesture_control.py --username qiudai --password xxx

快捷键：
    q 退出    m 切换模式    r 全部回中    d 开/关调试输出

依赖版本说明：
    MediaPipe 锁在 0.10.x + Python 3.12。
    1.0.x 在本机 macOS 上会在初始化时崩溃（DrishtiMetalHelper 报 service unavailable），
    强制 CPU delegate 也绕不过，所以用成熟的 0.10.x。
"""

import argparse
import os
import sys
import time

import cv2
import mediapipe as mp
import numpy as np
from PIL import Image, ImageDraw, ImageFont

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
# 两种模式的显示样式：横幅文字、底色(RGB)、一句话说明。
# 指令模式用绿、跟随模式用蓝，做成整条横幅 —— 瞄一眼就知道现在是哪个。
MODE_STYLE = {
    True:  ("指令模式", (46, 160, 67), "手势触发固定姿态"),
    False: ("跟随模式", (38, 108, 214), "手掌位置实时映射"),
}

# 中文字体：OpenCV 自带的 Hershey 字体只有 ASCII，cv2.putText 画「模式: 指令」
# 出来是一串问号（每个 UTF-8 字节一个「?」）—— 整个叠加层以前都是乱码。
# 所以中文一律用 PIL 渲染，再贴回画面上。
_FONT_CANDIDATES = (
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/STHeiti Light.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
)
_font_path = next((p for p in _FONT_CANDIDATES if os.path.exists(p)), None)

_fonts = {}
_text_cache = {}          # (文字, 字号, 颜色, x) → 渲染好的图块


def _font(size):
    if size not in _fonts:
        if _font_path is None:
            raise SystemExit("找不到中文字体，叠加层会是乱码。装一个思源黑体或改 _FONT_CANDIDATES。")
        _fonts[size] = ImageFont.truetype(_font_path, size)
    return _fonts[size]


def draw_text(frame, text, y, color=(255, 255, 255), scale=0.7, x=12):
    """
    画一行带黑色描边的文字，y 是文字的**底边**。

    color 按 RGB 给（PIL 的惯例），内部转成 BGR 再贴到画面上。
    渲染结果按 (文字, 字号, 颜色, x) 缓存 —— 这几行字每帧长得一样，
    没必要每帧重新排版一遍。
    """
    if not text:
        return

    size = max(12, int(round(30 * scale)))
    key = (text, size, color, x)
    patch = _text_cache.get(key)
    if patch is None:
        font = _font(size)
        l, t, r, b = font.getbbox(text)
        pad = 3                                  # 描边会往外扩，留点余量
        img = Image.new("RGBA", ((r - l) + pad * 2, (b - t) + pad * 2), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        ox, oy = pad - l, pad - t
        # 黑色描边：往 8 个方向各偏移一次。摄像头画面明暗不定，没描边会看不清。
        for dx, dy in ((-2, 0), (2, 0), (0, -2), (0, 2), (-1, -1), (1, -1), (-1, 1), (1, 1)):
            d.text((ox + dx, oy + dy), text, font=font, fill=(0, 0, 0, 255))
        d.text((ox, oy), text, font=font, fill=color + (255,))

        arr = np.array(img)
        patch = (arr[:, :, 2::-1], arr[:, :, 3:4].astype(np.float32) / 255.0)
        if len(_text_cache) > 300:               # 状态文字每帧都可能变，别无限涨
            _text_cache.clear()
        _text_cache[key] = patch

    rgb, alpha = patch
    h, w = rgb.shape[:2]
    fh, fw = frame.shape[:2]

    y0, x0 = int(y) - h, int(x)                  # y 是底边，往上推一个字高

    # 出画的部分裁掉。不能直接切 frame —— 形状对不上会报错。
    sx0, sy0 = max(0, -x0), max(0, -y0)
    sx1 = w - max(0, (x0 + w) - fw)
    sy1 = h - max(0, (y0 + h) - fh)
    if sx0 >= sx1 or sy0 >= sy1:
        return
    x0, y0 = max(0, x0), max(0, y0)
    rgb, alpha = rgb[sy0:sy1, sx0:sx1], alpha[sy0:sy1, sx0:sx1]

    roi = frame[y0:y0 + rgb.shape[0], x0:x0 + rgb.shape[1]]
    roi[:] = (roi * (1 - alpha) + rgb * alpha).astype(np.uint8)


def draw_mode_banner(frame, use_command_mode, width):
    """顶上的整条模式横幅。半米外也看得见现在是哪个模式。"""
    label, color, hint = MODE_STYLE[use_command_mode]
    cv2.rectangle(frame, (0, 0), (width, 54), color[::-1], -1)   # cv2 要 BGR
    draw_text(frame, label, 41, (255, 255, 255), 1.1)
    draw_text(frame, f"{hint}   ·   按 m 切换", 40, (232, 232, 232), 0.5, x=170)


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
    parser.add_argument("--username", default=None,
                        help="登录用户名（也可用环境变量 ARM_USERNAME）")
    parser.add_argument("--password", default=None,
                        help="控制密码（也可用环境变量 ARM_PASSWORD）")
    parser.add_argument("--no-window", action="store_true",
                        help="不弹预览窗口（只打印识别结果）")
    args = parser.parse_args()

    # 输出改成行缓冲。
    # 默认 Python 的 stdout 在重定向到文件/管道时是块缓冲（攒够 8KB 才落盘），
    # 于是「登录失败」这种关键一行会一直卡在缓冲区里看不见，
    # 看起来就像程序什么都没说 —— 排查时被这个坑过一次。
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:      # Python < 3.7
        pass

    client = ArmClient(args.url, username=args.username, password=args.password)

    if args.dry_run:
        print("[dry-run] 只识别，不发送控制命令")
    elif not client.ping():
        print(f"⚠️  连不上服务器 {args.url}：{client.last_error}")
        print("    机械臂不会动。可以先开服务器，或用 --dry-run 只调识别。")
    elif not client.login():
        # 服务器要求登录才能控制（防止陌生人乱动机械臂）
        print(f"⚠️  登录失败：{client.last_error}")
        print("    控制接口要「用户名 + 密码」两个都要，用参数或环境变量提供。")
        print("    例如： ARM_USERNAME=qiudai ARM_PASSWORD=你的密码 uv run gesture_control.py")
        print("    ⚠️ 识别照常能跑（画面上照样有骨架），但发出去的动作会被服务器拒绝，")
        print("       表现就是「手势识别出来了，机械臂一动不动」。")
    else:
        print(f"✅ 已连上服务器 {args.url} 并登录成功（{client.username}）")

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
    last_send_error = None
    fail_since = None       # 画面读不到的起始时刻（没断就是 None）
    window_title = None     # 只在模式变化时才改标题，不用每帧都设

    print("\n快捷键：q 退出 | m 切换模式 | r 回中 | d 调试输出\n")

    try:
        while True:
            ok, frame = cap.read()
            if not ok:
                # 偶发丢帧不该结束整场演示。
                # macOS 上摄像头被别的 App（微信/腾讯会议）短暂抢走、或者切
                # 输入源时会连续丢几帧，一读不到就退出太脆了。
                # 连续 5 秒都拿不到才认定是真断了。
                if fail_since is None:
                    fail_since = time.monotonic()
                    print("⚠️  读不到画面，重试中…")
                elif time.monotonic() - fail_since > 5.0:
                    print("读不到摄像头画面，退出（连续 5 秒没拿到帧）")
                    break
                time.sleep(0.03)        # 别空转把 CPU 占满
                continue

            if fail_since is not None:
                print(f"→ 画面恢复（中断了 {time.monotonic() - fail_since:.1f} 秒）")
                fail_since = None

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

            # 发送失败必须看得见。
            # 之前登录失败是完全静默的：画面照常画骨架、照常报手势，
            # 但每个命令都被服务器拒掉 —— 现场看就是「识别明明没问题，机械臂就是不动」，
            # 极难往鉴权上想。所以这里宁可吵一点。
            if client.last_error != last_send_error:
                last_send_error = client.last_error
                if last_send_error:
                    print(f"⚠️  发送失败：{last_send_error}（动作不会执行）")

            if fired and debug:
                print(f"[{mode.name if use_command_mode else follow.name}] {status}")

            # ---- 画面叠加信息 ----
            if not args.no_window:
                if hand:
                    mp.solutions.drawing_utils.draw_landmarks(
                        frame, hand, mp.solutions.hands.HAND_CONNECTIONS)

                draw_mode_banner(frame, use_command_mode, w)
                draw_text(frame, status, 90, (255, 255, 255))
                if fingers:
                    draw_text(frame, f"手指: {describe(fingers)}", 124, (180, 255, 180), 0.6)
                if client.last_error:
                    draw_text(frame, "没登进服务器，动作不会执行", 158, (255, 90, 90), 0.6)
                draw_text(frame, f"服务器: {args.url}", h - 16, (170, 170, 170), 0.5)

                cv2.imshow("Gesture Control", frame)

                # 标题栏也带上模式。窗口被别的窗口挡住、或者缩进 Dock 里时，
                # 光靠画面上那条横幅看不见 —— 标题栏能兜住。
                title = f"手势控制 — {MODE_STYLE[use_command_mode][0]}"
                if title != window_title:
                    window_title = title
                    cv2.setWindowTitle("Gesture Control", title)

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
