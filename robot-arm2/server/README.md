# 机械臂云控服务器

ESP32 与浏览器/手势模块之间的中转服务器，同时托管产品官网。

---

## 系统架构

```
   Vue 网页遥控 ──┐
                  ├──HTTP──> 本服务器 ──WebSocket──> ESP32 ──I2C──> PCA9685 ──> 5 舵机
   Python 手势  ──┘
```

**为什么要服务器中转？**
ESP32 在局域网里，没有公网 IP。让 ESP32 主动用 WebSocket 连到服务器并保持长连接，
浏览器就能通过服务器把命令转发进去 —— 不需要端口映射，也不用内网穿透。

---

## 快速开始

```bash
# 1. 建库建表（首次）
mysql -u root < schema.sql

# 2. 装依赖（前后端一起）
npm run setup

# 3. 构建前端
npm run build

# 4. 启动
npm start
```

打开 http://localhost:3000

| 页面 | 地址 |
|---|---|
| 首页 | `/` |
| 遥控（Vue 3） | `/control` |
| 留言板 | `/messages` |
| 下载 | `/download` |
| **API 文档** | **`/api-docs`** |

---

## npm 脚本

| 命令 | 作用 |
|---|---|
| `npm start` | 启动服务器 |
| `npm run setup` | 安装前后端全部依赖 |
| `npm run build` | 构建 Vue 前端（产物到 `public/control/`） |
| `npm run dev:web` | Vue 开发模式（热更新，5173 端口） |
| `npm run init-db` | 建库建表 |
| `npm run migrate` | 把旧的 JSON 留言迁移到 MySQL |
| `npm run explain` | **SQL 优化实证**（EXPLAIN 对比 + 压测） |

---

## 目录结构

```
server/
├── server.js          # 入口：路由 + WebSocket 中转
├── swagger.js         # OpenAPI 文档配置
├── db.js              # MySQL 连接池
├── store.js           # 留言数据访问层
├── schema.sql         # 建库建表
├── config.js          # 配置（含密码，已被 gitignore）
├── config.example.js  # 配置模板
├── migrate.js         # 旧数据迁移
├── explain-demo.js    # SQL 优化实证
├── DATABASE.md        # 数据库设计文档
├── web/               # Vue 3 前端源码
└── public/            # 静态资源 + 前端构建产物
```

---

## 接口

完整文档见 **http://localhost:3000/api-docs**（Swagger UI，可在线调试）。

也可以直接取原始规范导入 Postman / Apifox：`GET /api-docs.json`

### 遥控

```
GET /set?servo=0&angle=90
```

| 参数 | 说明 |
|---|---|
| `servo` | 0~5，依次为 底座 / 肩部 / 肘部 / 腕俯仰 / 腕旋转 / 夹爪 |
| `angle` | 0~180（度） |

ESP32 未连接时返回 **503** —— 前端和手势模块以此判断设备是否在线。

### 留言板

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/messages` | 留言列表（含回复） |
| POST | `/api/messages` | 发表留言 |
| POST | `/api/messages/:id/reply` | 回复（需管理员身份） |
| DELETE | `/api/messages/:id` | 删除（需管理员身份） |

### 登录与权限

**登录一次即为管理员**，之后控制机械臂、回复留言、删除留言都不再需要输密码。

| 接口 | 说明 |
|---|---|
| `POST /api/login` | 密码换 token，写入 httpOnly cookie |
| `POST /api/logout` | 退出登录 |
| `GET /api/auth` | 查询登录状态 |

- 机械臂的 `/set` 和控制台需要登录（防止陌生人乱动舵机）
- 留言板**浏览和发留言是公开的**，只有回复/删除需要管理员身份
- 登录接口有频率限制：同一 IP 十分钟内失败 5 次封禁 15 分钟

> 手势模块等本地程序用 `X-Auth-Token` 头携带 token（见 `gesture-control/arm_client.py`）。

---

## WebSocket

ESP32 连进来时要带暗号：

```
ws://<服务器地址>/?token=<ESP32_TOKEN>
```

token 不匹配会被直接断开。见 `config.js` 与 ESP32 固件里的对应配置。

---

## 配置

`config.js` **含密码，已被 `.gitignore` 排除**。部署时用环境变量覆盖：

| 环境变量 | 说明 |
|---|---|
| `PORT` | 服务端口（默认 3000） |
| `ADMIN_PASSWORD` | 留言管理密码 |
| `ESP32_TOKEN` | ESP32 连接暗号 |
| `DB_HOST` / `DB_PORT` / `DB_USER` / `DB_PASSWORD` / `DB_NAME` | MySQL 连接信息 |

模板见 `config.example.js`。

---

## 相关文档

- 数据库设计与 SQL 优化 → [`DATABASE.md`](DATABASE.md)
- Vue 前端 → [`web/README.md`](web/README.md)
- 手势识别 → [`../gesture-control/README.md`](../gesture-control/README.md)
- 总线舵机升级 → [`../arm-bus-servo/README.md`](../arm-bus-servo/README.md)
