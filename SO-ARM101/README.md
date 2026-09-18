# SO-ARM101 机械结构模型（第三方）

本目录是 **SO-ARM101** 开源机械臂的 3D 模型文件，用于本项目第二代机械臂的结构件打印。

> ⚠️ **这不是本项目的原创内容**，是从上游开源项目下载的第三方素材。
> 保留在此是为了让结构件与配套固件、文档处于同一仓库，方便对照。

---

## 来源与许可

| | |
|---|---|
| **上游项目** | [TheRobotStudio/SO-ARM100](https://github.com/TheRobotStudio/SO-ARM100) |
| **设计方** | RobotStudio × Hugging Face（LeRobot 项目官方硬件） |
| **许可证** | **Apache License 2.0**（见本目录 [`LICENSE`](LICENSE)） |
| **下载日期** | 2026-09-18 |

Apache-2.0 允许再分发和修改，但要求：

- ✅ 保留版权声明与许可证副本 —— 已在本目录放置 `LICENSE`
- ✅ 标注是否做了修改 —— **本目录内容原样下载，未做任何修改**

---

## 目录内容

```
SO-ARM101/
├── LICENSE      Apache-2.0 许可证（上游原文件）
├── STL/         用于 3D 打印的模型文件
│   ├── Base_SO101.stl                  底座
│   ├── Base_motor_holder_SO101.stl     底座舵机支架
│   ├── Upper_arm_SO101.stl             上臂
│   ├── Under_arm_SO101.stl             下臂
│   ├── Rotation_Pitch_SO101.stl        旋转俯仰件
│   ├── Motor_holder_SO101_Base.stl     舵机支架（底座）
│   ├── Motor_holder_SO101_Wrist.stl    舵机支架（腕部）
│   ├── Wrist_Roll_SO101.stl            腕部旋转
│   ├── Wrist_Roll_Pitch_SO101.stl      腕部旋转俯仰
│   ├── Wrist_Roll_Follower_SO101.stl   腕部旋转（Follower 专用）
│   ├── Moving_Jaw_SO101.stl            活动爪
│   ├── Trigger_SO101.stl               扳机
│   ├── Handle_SO101.stl                手柄（Leader 臂用）
│   ├── Seeedstudio_Mounting_Plate_SO101.stl   厂商安装板
│   ├── WaveShare_Mounting_Plate_SO101.stl     厂商安装板
│   ├── Gauge_0.STL                     ★ 尺寸校验件
│   └── Gauge_tight_1.STL               ★ 尺寸校验件
└── STEP/        可编辑的 CAD 源文件（改模型时用）
```

---

## ⚠️ 打印前必读

### 1. 先打校验件

**别急着打全部零件。** 先打 `STL/Gauge_0.STL`（20 分钟），
拿它套一下 STS3215 舵机或标准乐高积木：

- 松紧合适 → 打印机精度够，可以打全部
- 太紧/太松 → **先校准打印机**，否则所有零件都白打

这一步能省下几十小时和一整卷耗材。

### 2. 只需要 Follower 臂的零件

SO-ARM101 有 **Follower（干活的）** 和 **Leader（手拖的遥操作臂）** 两个版本。

**只打印 Follower 需要的件。** `Handle_SO101.stl` 是 Leader 臂用的，本项目不需要。

### 3. 打印参数

见上游仓库的 `3DPRINT.md`，里面有推荐层高和填充率。

---

## 配套信息

| 项目 | 位置 |
|---|---|
| 舵机型号与采购清单 | [`../robot-arm2/arm-bus-servo/README.md`](../robot-arm2/arm-bus-servo/README.md) |
| 固件（总线舵机 + 云连接） | [`../robot-arm2/arm-cloud-bus/`](../robot-arm2/arm-cloud-bus/) |
| 系统技术文档 | [`../robot-arm2/技术文档.md`](../robot-arm2/技术文档.md) |

---

## 关于本项目的结构选型

第一代的机械结构是自建的（见 [`../robot-arm 1/3D建模/`](../robot-arm%201/3D建模/)），
但换总线舵机时发现 STS3215 的尺寸与原模型不匹配（宽 +5mm、长 +4.5mm、高 −7.9mm）。

与其重新设计并承担未经验证的风险，**直接采用经过大量用户验证的开源设计** ——
这是工程上的合理取舍：**能复用成熟方案时，不重复造轮子。**
