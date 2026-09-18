# 五自由度机械臂 · 项目存档

本仓库包含机械臂项目的**两个阶段版本**，用于呈现完整的技术演进过程。

```
robot-arm 1/     2026-08-16 的版本（第一代）
robot-arm2/      当前版本（第二代）
```

---

## 为什么有两个版本

这不是"新旧替换"，而是**两次架构决策前后**的对比记录。

| | robot-arm 1 | robot-arm2 |
|---|---|---|
| **时间** | 2026-08-11 ~ 08-16 | 2026-09-18 起 |
| **定位** | 打通云端链路，实现公网遥控 | 解决抖动、补齐工程化 |
| **舵机驱动** | PCA9685 + 模拟舵机 | 升级中：总线舵机（STS3215） |
| **前端** | 原生 HTML/CSS/JS | Vue 3 + Vite |
| **数据存储** | JSON 文件 | MySQL |
| **接口文档** | 无 | OpenAPI / Swagger |
| **附加功能** | 官网 / 留言板 / PWA | 手势识别 / 力反馈接口 |

**关键差异**：第一代把系统"跑起来"了，但在静止时发现 5 个舵机会小幅抽搐——
根因是模拟舵机的脉宽被 WiFi 射频干扰。第二代换了**串行总线舵机**，
用带校验和的数字协议从**协议层免疫**这个问题，同时获得力反馈能力。

---

## 📁 robot-arm 1 —— 第一代（2026-08-16）

```
robot-arm 1/
├── 3D建模/              六个结构件的 STL（MG996R 尺寸，正在重新建模）
├── server/              云控服务器 + 官网（Node.js，JSON 存储）
├── servo-test/          基础固件（softAP 自建热点 + PCA9685）
├── arm-5servo/          直驱固件（绕开 PCA9685 的实验）
└── servo-direct-test/   单舵机诊断固件
```

**这一代解决了什么**：让 ESP32 主动连出到云服务器，实现公网远程控制。

**局限**：舵机静止时抖动；数据存 JSON 文件；前端为原生 JS。

> 📌 这是**冻结的快照**，仅作技术演进记录，不再更新。

---

## 📁 robot-arm2 —— 第二代（当前版本）

```
robot-arm2/
├── README.md            项目主页（功能介绍 / 架构 / 快速开始）
├── 技术文档.md           系统原理（含完整数据流追踪）
├── server/              云控服务器 + 官网（Node + MySQL + Vue 3）
├── gesture-control/     手势识别控制（Python + MediaPipe）
├── arm-cloud-bus/       云连接 + 总线舵机（当前主线）
└── arm-bus-servo/       总线舵机固件（含串口调试命令）
```

> 所有 MG996R 时代的固件（`arm-cloud/`、`servo-test/`、`arm-5servo/`、
> `servo-direct-test/`、`wifi-off-hold-test/`）与原 3D 模型
> 已归档到 `robot-arm 1/`，不在本目录中重复。

**这一代新增**：

- 🌐 手势识别控制（MediaPipe + OpenCV）
- 💾 MySQL 持久化 + 索引优化（扫描行数 49914 → 20）
- 🖥️ Vue 3 + Vite 重写前端
- 📖 OpenAPI / Swagger 接口文档
- 🔧 总线舵机升级（协议层免疫射频干扰 + 力反馈）
- 🔐 凭据隔离、设备鉴权、最小权限数据库账号

👉 **详细说明请进 [robot-arm2/README.md](robot-arm2/README.md)**

---

## 快速导航

| 我想看… | 去哪里 |
|---|---|
| 项目能做什么、怎么跑 | [robot-arm2/README.md](robot-arm2/README.md) |
| 系统是怎么运作的 | [robot-arm2/技术文档.md](robot-arm2/技术文档.md) |
| 每版固件为什么这么设计 | [固件演进记录.md](固件演进记录.md) |
| 抖动问题怎么排查的 | [抖动诊断结论.md](robot-arm%201/wifi-off-hold-test/抖动诊断结论.md) |
| 数据库怎么设计的 | [robot-arm2/server/DATABASE.md](robot-arm2/server/DATABASE.md) |
| 第一代长什么样 | [robot-arm 1/](robot-arm%201/) |

---

## 技术栈

| 层面 | robot-arm 1 | robot-arm2 |
|---|---|---|
| 嵌入式 | C++ / Arduino、ESP32-S3、I2C、PWM | ＋ TTL 串口总线协议 |
| 后端 | Node.js、Express、ws | ＋ MySQL、OpenAPI |
| 前端 | 原生 HTML/CSS/JS、PWA | ＋ Vue 3、Vite |
| AI | — | Python、MediaPipe、OpenCV |
| 部署 | Linux、Nginx、SSL | 同左 |

---

> ⚠️ **关于凭据**：两个版本中的 WiFi 密码、服务器地址、token 等敏感信息
> 均已抽离到 `secrets.h` / `config.js` 并加入 `.gitignore`，不入库。
> 模板见各自的 `.example` 文件。
