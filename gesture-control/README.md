# 手势控制机械臂

用摄像头识别手势，实时控制 5 自由度机械臂。

```
摄像头 → MediaPipe 手部关键点 → 手势判定 → HTTP → Node 服务器 → WebSocket → ESP32 → 舵机
```

手势模块只是**另一个客户端**，和网页遥控平级。现有服务器一行都不用改。

---

## 快速开始

```bash
cd gesture-control
uv run smoke_test.py        # 先自检：环境、模型、摄像头、服务器
uv run gesture_control.py   # 启动
```

首次运行 macOS 会弹窗申请摄像头权限，**点「允许」**。
如果没弹窗或误点了拒绝：系统设置 → 隐私与安全性 → 摄像头 → 勾选你的终端（Terminal / iTerm）。

### 常用参数

```bash
uv run gesture_control.py --dry-run              # 只识别，不发命令（不接机械臂也能调）
uv run gesture_control.py --url http://192.168.1.5:3000   # 连别的服务器
uv run gesture_control.py --camera 1             # 换摄像头
uv run gesture_control.py --no-window            # 不弹预览窗
```

### 快捷键

| 键 | 作用 |
|---|---|
| `q` | 退出 |
| `m` | 切换「指令模式 / 跟随模式」 |
| `r` | 全部回中 |
| `d` | 开/关调试输出 |

---

## 两种模式

### 指令模式
固定手势触发固定动作，手势需**连续稳定 5 帧**才触发（防止手在过渡姿势时乱动）：

| 手势 | 动作 |
|---|---|
| ✋ 张开 | 夹爪张开 |
| ✊ 握拳 | 夹爪闭合 |
| ☝️ 数字1 | 全部回中 |
| ✌️ 剪刀手 | 预备姿态 |
| 数字3 | 抓取姿态 |
| 数字4 | 放下姿态 |

### 跟随模式
手掌在画面里的位置实时映射：
- 手掌 **左右** → 底座旋转
- 手掌 **上下** → 大臂俯仰

---

## 项目结构

```
gesture-control/
├── gestures.py          # 手势判定逻辑（纯计算，无外部依赖，可单测）
├── arm_client.py        # 与机械臂服务器通信（HTTP）
├── gesture_control.py   # 主程序：摄像头循环 + 模式处理 + 界面
├── smoke_test.py        # 冒烟测试：一键自检环境
└── tests/
    └── test_gestures.py # 单元测试（14 个用例）
```

**分层的意义**：`gestures.py` 不 import mediapipe、不碰摄像头、不发请求，
所以能脱离硬件单元测试；以后想换成 YOLO 或别的模型，也只需替换主程序里的检测部分。

```bash
uv run python -m unittest discover tests -v
```

---

## 两个技术细节

### 1. 手指伸直的判定方法

常见做法是「指尖 y 坐标比指节小就算伸直」——但这**要求手掌必须竖直朝上**，
手一歪就判错。

本项目比较的是**指尖到掌心的距离**和**近端指节到掌心的距离**：

```
伸直 → 指尖伸得更远 → 距离更大
弯曲 → 指尖收回掌心 → 距离反而更小
```

这个方法与手掌朝向、旋转、离镜头远近**都无关**。
测试 `test_orientation_independent` 专门验证了这一点：把整只手旋转 90°，结果不变。

顺带的好处：判定不用左右手信息，所以画面可以放心做镜像翻转（自拍视角）。

### 2. 为什么锁 MediaPipe 0.10.x + Python 3.12

MediaPipe **1.0.x 在 macOS 上初始化即崩溃**：

```
Check failed: service_ Service is unavailable.
  -[DrishtiMetalHelper initWithCalculatorContext:]
```

这是它 GPU（Metal）后端的问题，**强制 CPU delegate 也绕不过**。
所以退回成熟的 0.10.21；而 0.10.x 只支持到 Python 3.12，故用 uv 固定 3.12。

> 依赖版本已写死在 `pyproject.toml`，`uv sync` 会自动装对，不用手动处理。

---

## 接到真实的机械臂

手势模块只依赖服务器的 `/set` 接口：

```
GET /set?servo=<0-4>&angle=<0-180>
```

1. 启动服务器：`cd ../server && npm start`
2. 确认 ESP32 已连上（服务器日志会打印「ESP32 认证通过」）
3. `uv run gesture_control.py`

服务器没开也能用 `--dry-run` 单独调试识别效果。
