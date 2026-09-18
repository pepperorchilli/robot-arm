# 云连接版固件

让 ESP32 **主动连出去**，接入 Node 服务器，实现浏览器远程控制。

---

## 和其他 sketch 的区别

| sketch | 模式 | 控制方式 |
|---|---|---|
| `servo-test/` | `WiFi.softAP` 自建热点 | 浏览器直连 ESP32 的 IP |
| `arm-5servo/` | 自建热点 | 直驱舵机 |
| `arm-bus-servo/` | 自建热点 | 总线舵机 |
| **`arm-cloud/`** | **`WIFI_STA` 连路由器** | **经服务器中转，可公网访问** |

前几个只能局域网直连。这个能穿透到公网——**因为 ESP32 主动往外连，不需要公网 IP，也不用端口映射**。

---

## 数据流

```
浏览器 ──HTTP──> Node 服务器 ──WebSocket 长连接──> ESP32 ──I2C──> PCA9685 ──> 舵机
```

---

## 一、首次配置

### 1. 创建凭据文件

```bash
cd arm-cloud
cp secrets.h.example secrets.h
```

编辑 `secrets.h`，填上：

```c
#define WIFI_SSID       "你的WiFi名称"
#define WIFI_PASSWORD   "你的WiFi密码"
#define SERVER_HOST     "192.168.1.100"      // 见下方"查电脑 IP"
#define SERVER_PORT     3000
#define ESP32_TOKEN     "change-me-to-a-random-token"
```

> ⚠️ `secrets.h` 已在 `.gitignore` 中，**不会提交到 Git**。
> 之前你的 WiFi 密码就是因为写死在固件里、仓库又是公开的而泄露过。
> 永远不要把真实密码写进 `secrets.h.example`（那个文件是要提交的）。

### 2. 确认 token 一致

`secrets.h` 里的 `ESP32_TOKEN` 必须和 `server/config.js` 里的 `ESP32_TOKEN` **完全一致**，否则服务器会直接断开连接。

### 3. ⚠️ ESP32 只支持 2.4GHz

连不上 5GHz 频段的 WiFi。如果连不上，先用手机热点试（手机热点一般是 2.4G）。

---

## 二、局域网测试（推荐先做这步）

不用租服务器，电脑上跑就行。

### 第 1 步：查你电脑的局域网 IP

```bash
# macOS
ipconfig getifaddr en0
# 输出类似 192.168.1.100
```

> 有线和无线网卡不同：Wi-Fi 是 `en0`，网线是 `en1`（不确定就跑 `ifconfig | grep "inet "`）。

### 第 2 步：把 IP 填进 `secrets.h` 的 `SERVER_HOST`

### 第 3 步：启动服务器

```bash
cd server
npm run build     # 如果改过 Vue 前端
npm start
```

看到 `服务器运行在 http://localhost:3000` 和 `✅ 数据库连接正常` 即可。

### 第 4 步：烧录固件

串口监视器（115200）应该看到：

```
连接 WiFi: 你的WiFi名称 ....
✅ WiFi 已连接
   本机 IP: 192.168.1.123
正在连接服务器 192.168.1.100:3000 …
✅ 已连上服务器
```

**同时服务器那边**应该打印：

```
✅ ESP32 认证通过
✅ 这是 ESP32，已登记
```

> ❌ 如果服务器打印 `有设备尝试连接，但 token 错误` —— 说明两边的 token 不一致。
> ❌ 如果什么也不打印 —— ESP32 没连上，检查 IP、端口、防火墙。

### 第 5 步：打开网页控制

浏览器访问 **http://localhost:3000/control**，拖动滑块。

- 舵机应该跟着动
- 到位后服务器日志会打印 `ESP32 回报: 0:90,1:45,...`

> macOS 防火墙可能拦截：系统设置 → 网络 → 防火墙 → 允许 node 接受传入连接。

---

## 三、公网部署（以后再做）

局域网验证通过后，搬到云服务器只需**改一行**：

1. 租一台云服务器（学生机约 ¥10/月）
2. 部署 Node + MySQL + Nginx + SSL（见 `server/README.md`）
3. 把 `secrets.h` 里的 `SERVER_HOST` 改成公网 IP 或域名
4. 重新烧录

其余代码**一行都不用动**。

---

## 四、技术要点

### 1. 非阻塞运动控制

旧版本用 `while` + `delay` 逐个舵机走步，问题有二：

- `delay` 期间整个 `loop()` 卡住，**WebSocket 收不到新消息、心跳也发不出去**，服务器会以为设备掉线
- 5 个舵机只能一个一个动，没法协调

改成主循环里「每隔 `STEP_DELAY` 走一步」：

```cpp
void loop() {
  ws.loop();       // 必须高频调用
  stepMotion();    // 所有舵机同时朝各自目标推进
}
```

所有舵机并行推进，`loop()` 始终保持响应。

### 2. 断线自动重连

```cpp
ws.setReconnectInterval(5000);
ws.enableHeartbeat(15000, 3000, 2);
```

心跳很重要：家用路由器和运营商的 NAT 会掐断长时间空闲的 TCP 连接。
没有心跳的话，连接看着还在，实际已经死了，命令发不进去。

### 3. 到位回报

所有舵机都到达目标后，固件主动回传一次状态：

```
0:90,1:45,2:120,3:90,4:150
```

网页端可以据此显示"已到位"。

---

## 五、排错

| 现象 | 原因 |
|---|---|
| WiFi 连不上 | 用了 5GHz 频段 / 密码错 / 信号太弱 |
| 连不上服务器 | `SERVER_HOST` 填错 / 服务器没启动 / 防火墙拦截 |
| token 错误 | 两边 token 不一致 |

> **调试提示**：token 不对时，WebSocket 的**握手会先成功**，然后服务器立刻把连接断开。
> 所以固件串口会看到「已连上服务器」紧接着「与服务器断开」，反复循环——
> 这不是网络问题，是 token 不匹配。**以服务器日志为准**，它会明确打印
> `❌ 有设备尝试连接，但 token 错误，已拒绝`。
| 连上但舵机不动 | 命令格式不对（应为 `"0:45"`）/ 舵机供电没接 |
| 偶尔断开重连 | 正常，心跳机制会自愈；频繁断检查 WiFi 信号 |
| 编译报错找不到 secrets.h | 忘了 `cp secrets.h.example secrets.h` |

---

## 相关文档

- 服务器部署 → [`../server/README.md`](../server/README.md)
- 数据库设计 → [`../server/DATABASE.md`](../server/DATABASE.md)
- 总线舵机升级 → [`../arm-bus-servo/README.md`](../arm-bus-servo/README.md)
- 抖动问题诊断 → [`../wifi-off-hold-test/抖动诊断结论.md`](../wifi-off-hold-test/抖动诊断结论.md)
