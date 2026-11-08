# 机械臂云控服务 · 自动化测试套件

针对 `server/` 的**黑盒集成测试**：把真实的 `server.js` 作为独立进程拉起来，
通过 HTTP 与 WebSocket 从外部驱动，覆盖接口、协议、鉴权、限流、留言板、
时延，以及"文档 vs 实现"的契约一致性。

- 用例数：**109**（104 通过 / 5 个 `xfail` 是被钉住的已知缺陷）
- 单机全量耗时：约 26 秒
- 不需要真实硬件：设备端由 `helpers/device.py` 里的假 ESP32 模拟

---

## 快速开始

前置条件：Node 18+、Python 3.10+、本地 MySQL。

```bash
# 1. 建测试库和最小权限账号（只做一次）
mysql -uroot <<'SQL'
CREATE DATABASE IF NOT EXISTS robot_arm_test
  CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE USER IF NOT EXISTS 'arm_app'@'%' IDENTIFIED BY 'arm_dev_password';
GRANT SELECT, INSERT, UPDATE, DELETE ON robot_arm_test.* TO 'arm_app'@'%';
SQL

# 2. 装服务端依赖
cd .. && npm install && cd tests

# 3. 装测试依赖并运行
make setup
make test
```

表结构不用手工建：`conftest.py` 会从 `server/schema.sql` 派生并在测试库里幂等执行。

`server/config.js` 含真实密码、被 `.gitignore` 排除，所以新 clone 的仓库和 CI
里都没有这个文件。测试启动服务时会用 `config.example.js` 兜底生成一份，
端口、库名、账号、密码全部由环境变量覆盖，占位值不会被用到。

常用命令（`server/` 目录下也可以直接 `npm test` / `npm run test:fast` / `npm run lint:test`）：

| 命令 | 用途 |
|---|---|
| `make test` | 全量 |
| `make fast` | 跳过性能基线（改代码时用） |
| `make perf` | 只跑时延统计并打印分位数 |
| `make lint` / `make fmt` | 静态检查 / 自动修复 |
| `make report` | 生成 `reports/junit.xml` 与 `reports/report.html` |

可覆盖的环境变量：`TEST_DB_HOST/PORT/USER/PASSWORD`、`TEST_DB_ADMIN_USER/PASSWORD`、
`TEST_DB_NAME`、`TEST_ESP32_TOKEN`。

---

## 目录结构

```
tests/
├── conftest.py            # 服务进程、假设备、隔离的 HTTP 客户端、账号夹具
├── pytest.ini             # 超时 60s、严格 marker、默认参数
├── ruff.toml              # 静态检查规则
├── Makefile
├── helpers/
│   ├── db.py              # 从 schema.sql 派生建表 + 清空数据
│   └── device.py          # 假 ESP32（WebSocket 客户端）+ wait_until
├── test_00_smoke.py       # 冒烟：服务活着吗
├── test_10_auth.py        # 账号、会话、越权
├── test_20_control.py     # 遥控链路：浏览器 → 服务器 → 设备
├── test_30_telemetry.py   # 遥测解析：寄存器值 → 物理量
├── test_40_websocket.py   # 设备连接：鉴权、顶号、断线清理
├── test_50_security.py    # 路径穿越、令牌泄漏、注入、请求体上限
├── test_60_messages.py    # 留言板：昵称归属、权限、级联删除
├── test_70_contract.py    # OpenAPI 文档 vs 真实实现
├── test_80_performance.py # 端到端时延基线（p50/p95/p99）
└── test_90_rate_limit.py  # 三套限流：登录爆破、注册灌水、全局限流
```

---

## 六个设计决策

这六条是这套测试真正的价值所在，也是面试时最该讲的部分。

### 1. 黑盒 + 子进程，而不是 `require('./server.js')`

`server.js` 在被 import 的那一刻就连数据库、监听端口、注册三个定时器——
这些副作用无法撤销，用例之间会互相污染。改为 `subprocess` 拉起真实进程后：

- 测的是"部署后真实跑起来的样子"，不是一堆 mock 拼出来的想象；
- 进程内状态（限流计数、设备连接）天然按测试文件隔离；
- 服务崩溃能被直接观察到（进程提前退出会带着日志报错）。

代价是节奏慢一点（每个文件起一次服务，约 1 秒），换来的是可信度。

### 2. 不改一行原代码

整套测试只在 `server/tests/` 里新增文件。`server.js`、`accounts.js`、
`schema.sql` 一行没动——所以它可以随时被删除、随时被替换，
不构成对生产代码的侵入。这是测试层的底线：**测试不该为了好测而改造被测对象。**

### 3. 每个用例分配一个模拟 IP

服务器按 IP 做三道限流（注册 5 次/小时、登录失败 5 次/10 分钟、全站 300 次/分钟）。
如果所有用例都从 `127.0.0.1` 发起，跑到第五个用例就会被自己的防护拦住。

所以 `make_client()` 给每个客户端分配一个独立的 `X-Forwarded-For`（`10.42.x.y`），
`req.ip` 取到的就是它。这既隔离了计数，也顺带验证了 `trust proxy` 在反向代理下
能正确识别真实 IP。

`test_90_rate_limit.py` 则反过来，**故意**用同一个 IP 把每道闸打满，验证阈值、
状态码和 `Retry-After` 都对得上。

### 4. 表结构只有一个来源：`schema.sql`

测试库不另存一份 DDL。`helpers/db.py` 在启动时读取 `server/schema.sql`，
剔掉 `CREATE DATABASE` / `USE` / `CREATE USER` / `GRANT`，剩下的 `CREATE TABLE`
在测试库上幂等执行。

真实项目里"测试环境和线上结构不一致"绝大多数来自两份 DDL 各自演变。
只要测试直接消费生产 DDL，这种漂移就不可能发生。

清理数据用 `DELETE` 而不是 `TRUNCATE`：业务账号 `arm_app` 在生产里只有
增删改查权限，测试也照这个权限走——**权限模型也是被测试的一部分**。

### 5. 没有覆盖率数字，是故意的

被测系统是 Node，Python 这边只是脚手架。对 `helpers/` 跑覆盖率会得到一个
漂亮但毫无意义的百分比。真正的行覆盖率需要 Node 侧插桩（c8/nyc），
那要么改原服务代码，要么接受 `NODE_V8_COVERAGE` 的粗粒度输出——都不划算。

**宁可不报覆盖率，也不报假覆盖率。** 这套测试的可信度来自断言的密度和边界，
不是来自一个数字。

### 6. 用 `xfail(strict=True)` 记录已知缺陷

发现的 5 个缺陷没有被写成注释或 TODO，而是写成了**可执行的断言**：

```python
@pytest.mark.xfail(strict=True, reason="body-parser 的 413 被兜底错误处理改写成了 500")
def test_oversized_json_body_is_rejected_with_413(api):
    assert api.post(...).status_code == 413
```

现在的实现不满足，所以显示为 `xfail`；一旦有人修好，它会变成 `XPASS`，
`strict=True` 会让 CI 立刻变红，逼着人来删掉标记。
缺陷不会因为没人记得而悄悄消失。

---

## 已知缺陷（5 条，均由用例钉住）

| # | 现象 | 正确行为 | 用例 |
|---|---|---|---|
| 1 | 请求体超过 100kb 返回 **500** | **413**（客户端错误，不该让客户端重试） | `test_50_security.py` |
| 2 | 留言内容超过 500 字返回 **500** | **400** + 可读提示（OpenAPI 里写了 `maxLength: 500`，代码没校验） | `test_60_messages.py` |
| 3 | 登录缺 `password` 字段返回 **500**（`crypto.scrypt` 抛类型错误） | **400** | `test_10_auth.py` |
| 4 | `GET /api/auth` 未写入 OpenAPI 文档 | 补文档（它是 `/api/me` 的别名，但调用方看不见） | `test_70_contract.py` |
| 5 | `/set` 不校验 `servo` / `angle` 范围，`"9:999"` 原样下发给固件 | **400**（文档声明 servo 0-5、angle 0-180） | `test_70_contract.py` |

前三条是同一个根因：**兜底错误处理把所有非业务异常都变成了 500**，
于是"客户端传错了"和"服务器炸了"对调用方来说长得一模一样。

---

## 性能基线（本机实测）

```
set_round_trip:  samples=200  p50=2.34ms  p95=3.95ms  p99=5.59ms
telemetry_read:  samples=200  p50=2.04ms  p95=3.34ms  p99=4.63ms
```

`set_round_trip` 测的是"HTTP 请求发出 → 假设备收到 `关节:角度`"这一段，
包含一次 WebSocket 转发，是整条遥控链路里唯一完全由服务器决定的部分。

阈值定得很宽（p95 < 200ms）：目的是**发现退化**，不是卡死某个数字。
换一台机器绝对值会变，但同一台机器上从 4ms 变成 80ms，就说明有东西退化了。
每次运行都会把结果追加到 `reports/perf.json`，方便做趋势对比。

---

## CI

`.github/workflows/tests.yml`：MySQL 8 service 容器 + Node 20 + Python 3.12，
依次跑建库、`npm ci`、`pip install`、`ruff check`、`pytest`，
报告（junit / HTML / 被测服务日志）作为 artifact 上传，保留 14 天。

只在 `robot-arm2/server/**` 和 workflow 自身变更时触发。

---

## 不测什么

- **真实硬件**：整套用例只连假设备，不需要接舵机和 ESP32。真机验证另走手工流程。
- **舵机物理行为**：堵转、温升、掉电复位这些属于硬件验收，不在软件测试范围。
- **前端渲染**：只测接口契约（字段、类型、时间戳格式），不测 Vue 组件的 DOM。
- **并发压测**：`test_80_performance.py` 是时延基线，不是负载测试。
  真要做压测应该上 Locust/k6，并且用独立的压测环境。
