"""
手势识别核心逻辑

刻意做成「纯计算」模块：不 import mediapipe、不碰摄像头、不发网络请求。
好处是可以直接单元测试（见 tests/test_gestures.py），
也方便以后换别的模型（YOLO 等）而不用改判定逻辑。
"""

# ---------------------------------------------------------------
# MediaPipe 手部 21 个关键点的索引
# ---------------------------------------------------------------
WRIST = 0
THUMB_CMC, THUMB_MCP, THUMB_IP, THUMB_TIP = 1, 2, 3, 4
INDEX_MCP, INDEX_PIP, INDEX_DIP, INDEX_TIP = 5, 6, 7, 8
MIDDLE_MCP, MIDDLE_PIP, MIDDLE_DIP, MIDDLE_TIP = 9, 10, 11, 12
RING_MCP, RING_PIP, RING_DIP, RING_TIP = 13, 14, 15, 16
PINKY_MCP, PINKY_PIP, PINKY_DIP, PINKY_TIP = 17, 18, 19, 20

# 构成「掌心」的 5 个点：手腕 + 四根手指的掌指关节
PALM_POINTS = (WRIST, INDEX_MCP, MIDDLE_MCP, RING_MCP, PINKY_MCP)

# 每根手指的 (指尖, 近端指节)，用来判断伸直还是弯曲
FINGER_JOINTS = (
    (THUMB_TIP, THUMB_IP),
    (INDEX_TIP, INDEX_PIP),
    (MIDDLE_TIP, MIDDLE_PIP),
    (RING_TIP, RING_PIP),
    (PINKY_TIP, PINKY_PIP),
)

FINGER_NAMES = ("拇指", "食指", "中指", "无名指", "小指")

# 稍微大于 1 的余量，避免手指在临界位置反复横跳（识别抖动）
DEFAULT_MARGIN = 1.06


# ---------------------------------------------------------------
# 基础几何
# ---------------------------------------------------------------
def _xy(landmark):
    """把 MediaPipe 的关键点对象转成 (x, y) 二元组。

    只取 x/y：手指伸展与否是平面信息，z 轴（深度）噪声大，不用。
    """
    return (landmark.x, landmark.y)


def _dist(p, q):
    """两点欧氏距离"""
    return ((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2) ** 0.5


def palm_center(landmarks):
    """掌心 = 手腕与四个掌指关节的几何中心"""
    pts = [_xy(landmarks[i]) for i in PALM_POINTS]
    cx = sum(p[0] for p in pts) / len(pts)
    cy = sum(p[1] for p in pts) / len(pts)
    return (cx, cy)


def hand_scale(landmarks):
    """手掌尺度（手腕→中指掌指关节的距离）。

    用来做归一化，让判定不受「手离摄像头远近」影响。
    """
    return _dist(_xy(landmarks[WRIST]), _xy(landmarks[MIDDLE_MCP]))


# ---------------------------------------------------------------
# 核心：判断每根手指是否伸直
# ---------------------------------------------------------------
def fingers_up(landmarks, margin=DEFAULT_MARGIN):
    """返回 5 个布尔值：[拇指, 食指, 中指, 无名指, 小指]，True = 伸直。

    判定思路（不用常见的「指尖 y 比指节小」，那个要求手掌必须竖直朝上）：
        比较「指尖到掌心的距离」和「近端指节到掌心的距离」。
        手指伸直时指尖会伸得更远 → 距离更大；
        手指弯曲时指尖收向掌心 → 距离反而变小。
    这个方法与手掌朝向、旋转、离镜头远近都无关，稳得多。
    """
    center = palm_center(landmarks)
    result = []
    for tip_idx, pip_idx in FINGER_JOINTS:
        d_tip = _dist(_xy(landmarks[tip_idx]), center)
        d_pip = _dist(_xy(landmarks[pip_idx]), center)
        result.append(d_tip > d_pip * margin)
    return result


# ---------------------------------------------------------------
# 手势分类
# ---------------------------------------------------------------
# 手势名 -> 五根手指的伸展模式（1=伸直, 0=弯曲），顺序同 fingers_up
GESTURE_PATTERNS = {
    "握拳":   (0, 0, 0, 0, 0),
    "点赞":   (1, 0, 0, 0, 0),
    "数字1":  (0, 1, 0, 0, 0),
    "剪刀手": (0, 1, 1, 0, 0),
    "数字3":  (0, 1, 1, 1, 0),
    "数字4":  (0, 1, 1, 1, 1),
    "张开":   (1, 1, 1, 1, 1),
    "小指":   (0, 0, 0, 0, 1),
}

_NUM_TO_CHINESE = {1: "数字1", 2: "剪刀手", 3: "数字3", 4: "数字4", 5: "张开"}


def classify(fingers, with_thumb=False):
    """把手指伸展模式翻译成手势名。

    参数:
        fingers:   fingers_up() 的返回值
        with_thumb: 是否把拇指算进「数字」里。
                    False（默认）：只数食指~小指，4 根伸直也算「数字4」，
                                   这样拇指是否被误判不影响数字手势。
                    True：拇指也算，用于区分「点赞」「张开」等。
    返回:
        手势名字符串；无法识别时返回 None。
    """
    pattern = tuple(bool(f) for f in fingers)

    # 先精确匹配带拇指的特殊手势
    for name, pat in GESTURE_PATTERNS.items():
        if pattern == pat:
            return name

    # 再退一步：只数食指~小指，识别成数字（拇指状态忽略）
    if not with_thumb:
        count = sum(pattern[1:])
        if pattern[1:] == tuple([1] * count + [0] * (4 - count)) and count > 0:
            return _NUM_TO_CHINESE.get(count)
    return None


# 不含拇指的四根手指，伸直的数量 → 手势名。
# 为什么干脆不看拇指：实测握拳时拇指会被判成"伸直"（变成点赞），
# 比数字4 时也会（变成张开）。拇指的几何和其它四指不一样，
# 「指尖比指节离掌心远」这条规则对它本来就不成立。
# 与其调阈值拿现场当试验场，不如让它彻底不参与判定。
_FOUR_FINGER_NAMES = {
    0: "握拳",
    1: "数字1",
    2: "剪刀手",
    3: "数字3",
    4: "张开",
}


def classify_tolerant(fingers):
    """摄像头用的手势判定：只数食指~小指，拇指不参与。

    0/1/2/3/4 根伸直 → 握拳 / 数字1 / 剪刀手 / 数字3 / 张开。
    小指单独伸开算一个独立手势（否则它会被数成「数字1」）。

    ⚠️ 「数字4」和「张开」在关键点上没有区别（都是四根手指伸直），
       所以没有数字4 这个手势了；原来绑在它上面的动作改挂在小指上。
    """
    pattern = tuple(bool(f) for f in fingers)

    # 小指单独伸开：其余四根收着。必须排在「数数量」之前，
    # 否则会被当成数字1。
    if pattern[1:] == (False, False, False, True):
        return "小指"

    # 食指 + 小指（摇滚手势）：连续伸法已经全被占满了，
    # 只有不连续组合还能再加一个不冲突的手势。这个姿势好摆、四根手指
    # 各自的方向差别大，距离判定不容易混。
    if pattern[1:] == (True, False, False, True):
        return "摇滚"

    tail = pattern[1:]
    count = sum(tail)
    # 必须是「连续从食指开始伸直」才算数：
    # 食指+无名指这种乱伸法不是合法手势，不能数成 2。
    if tail == tuple([True] * count + [False] * (4 - count)):
        return _FOUR_FINGER_NAMES[count]
    return None


def describe(fingers):
    """给调试/界面用：把伸展状态说成人话，如「食指+中指」"""
    up = [FINGER_NAMES[i] for i, f in enumerate(fingers) if f]
    return "+".join(up) if up else "无"
