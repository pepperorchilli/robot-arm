"""
手势判定逻辑的单元测试

用手工构造的「假关键点」来测，不需要摄像头、不需要真手。
运行：
    uv run pytest          （或 uv run python -m unittest discover tests）
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from gestures import (  # noqa: E402
    INDEX_PIP, INDEX_TIP, MIDDLE_PIP, MIDDLE_TIP, MIDDLE_MCP,
    PINKY_PIP, PINKY_TIP, RING_PIP, RING_TIP, THUMB_IP, THUMB_TIP, WRIST,
    classify, classify_tolerant, describe, fingers_up, hand_scale, palm_center,
)


class FakeLandmark:
    """模拟 MediaPipe 的关键点对象（只要有 x / y 就行）"""

    def __init__(self, x, y):
        self.x = x
        self.y = y


def make_hand(tips_far=True):
    """造一只「手掌朝上、手指伸直」的假手。

    约定：手腕在 (0.5, 0.9)，掌心在 (0.5, 0.7) 附近，
    手指向 y 变小的方向伸出。tips_far=True 表示指尖比指节离掌心更远（伸直）。
    """
    lms = [FakeLandmark(0.5, 0.5) for _ in range(21)]
    # 手腕
    lms[WRIST] = FakeLandmark(0.5, 0.9)
    # 中指掌指关节（用于 hand_scale）
    lms[MIDDLE_MCP] = FakeLandmark(0.5, 0.7)

    finger_pairs = [
        (THUMB_TIP, THUMB_IP, 0.30),
        (INDEX_TIP, INDEX_PIP, 0.40),
        (MIDDLE_TIP, MIDDLE_PIP, 0.50),
        (RING_TIP, RING_PIP, 0.60),
        (PINKY_TIP, PINKY_PIP, 0.70),
    ]
    for tip_idx, pip_idx, x in finger_pairs:
        # 指节固定在 y=0.5
        lms[pip_idx] = FakeLandmark(x, 0.5)
        # 伸直：指尖更远（y 更小）；弯曲：指尖收回（y 更大，靠近掌心）
        lms[tip_idx] = FakeLandmark(x, 0.2 if tips_far else 0.72)
    return lms


class TestGeometry(unittest.TestCase):

    def test_palm_center_is_between_points(self):
        lms = make_hand()
        cx, cy = palm_center(lms)
        self.assertAlmostEqual(cx, 0.5, places=1)
        self.assertGreater(cy, 0.5)
        self.assertLess(cy, 0.9)

    def test_hand_scale_positive(self):
        self.assertGreater(hand_scale(make_hand()), 0)


class TestFingersUp(unittest.TestCase):

    def test_all_extended(self):
        self.assertEqual(fingers_up(make_hand(tips_far=True)), [True] * 5)

    def test_all_curled(self):
        self.assertEqual(fingers_up(make_hand(tips_far=False)), [False] * 5)

    def test_orientation_independent(self):
        """把整只手旋转 90°，判定结果应该不变（这是本方法与常见做法最大的区别）"""
        lms = make_hand(tips_far=True)
        rotated = [FakeLandmark(lm.y, 1 - lm.x) for lm in lms]
        self.assertEqual(fingers_up(rotated), [True] * 5)


class TestClassify(unittest.TestCase):

    def test_fist(self):
        self.assertEqual(classify([False] * 5), "握拳")

    def test_open_palm(self):
        self.assertEqual(classify([True] * 5), "张开")

    def test_thumbs_up(self):
        self.assertEqual(classify([True, False, False, False, False]), "点赞")

    def test_number_one(self):
        self.assertEqual(classify([False, True, False, False, False]), "数字1")

    def test_scissors(self):
        self.assertEqual(classify([False, True, True, False, False]), "剪刀手")

    def test_thumb_does_not_break_numbers(self):
        """with_thumb=False 时，拇指状态不影响数字手势 —— 实际用摄像头时拇指最容易误判"""
        self.assertEqual(classify([True, True, True, False, False], with_thumb=False), "剪刀手")
        self.assertEqual(classify([False, True, True, False, False], with_thumb=False), "剪刀手")

    def test_unknown_returns_none(self):
        """食指+无名指伸直，不是合法手势"""
        self.assertIsNone(classify([False, True, False, True, False]))


class TestClassifyTolerant(unittest.TestCase):
    """摄像头用的判定：拇指完全不参与。

    现场事故：原来要求五指精确匹配，拇指一被判成伸直，
    剪刀手/数字3/数字4 全部返回 None；握拳则变成点赞。
    """

    def test_thumb_misread_scissors(self):
        self.assertEqual(classify_tolerant([True, True, True, False, False]), "剪刀手")

    def test_thumb_misread_three(self):
        self.assertEqual(classify_tolerant([True, True, True, True, False]), "数字3")

    def test_thumb_does_not_turn_fist_into_thumbs_up(self):
        """握拳时拇指被误判成伸直，结果必须还是握拳（以前是点赞）"""
        self.assertEqual(classify_tolerant([True, False, False, False, False]), "握拳")

    def test_four_fingers_is_open_palm(self):
        """四指伸直（拇指随意）= 张开；没有数字4 这个手势了"""
        self.assertEqual(classify_tolerant([False, True, True, True, True]), "张开")
        self.assertEqual(classify_tolerant([True] * 5), "张开")

    def test_fist_still_works(self):
        self.assertEqual(classify_tolerant([False] * 5), "握拳")

    def test_pinky_only(self):
        self.assertEqual(classify_tolerant([False, False, False, False, True]), "小指")
        # 拇指误判也不影响
        self.assertEqual(classify_tolerant([True, False, False, False, True]), "小指")

    def test_rock_sign(self):
        """食指 + 小指 = 摇滚 / 收拢；拇指误判不影响"""
        self.assertEqual(classify_tolerant([False, True, False, False, True]), "摇滚")
        self.assertEqual(classify_tolerant([True, True, False, False, True]), "摇滚")

    def test_unknown_still_none(self):
        """其它不连续的伸法不算数，不能被硬套成数字"""
        self.assertIsNone(classify_tolerant([False, True, False, True, False]))
        self.assertIsNone(classify_tolerant([False, False, True, False, True]))


class TestDescribe(unittest.TestCase):

    def test_none_up(self):
        self.assertEqual(describe([False] * 5), "无")

    def test_some_up(self):
        self.assertEqual(describe([False, True, True, False, False]), "食指+中指")


if __name__ == "__main__":
    unittest.main(verbosity=2)
