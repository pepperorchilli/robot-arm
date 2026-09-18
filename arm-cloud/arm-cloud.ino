/*
 * ESP32-S3 + PCA9685 — 云连接版固件
 *
 * 和前几个 sketch 的区别：
 *   别的 sketch 是「ESP32 自建热点 + 自带网页」，只能局域网直连。
 *   这个是「ESP32 主动连出去」，连到 Node 服务器，再由服务器转发浏览器的命令。
 *
 * ==================== 为什么要服务器中转 ====================
 *   ESP32 在局域网里，没有公网 IP。让它主动用 WebSocket 连到服务器并保持长连接，
 *   浏览器就能通过服务器把命令转发进来 —— 不需要端口映射，也不用内网穿透。
 *
 * ==================== 数据流 ====================
 *   浏览器 ──HTTP──> Node 服务器 ──WebSocket──> 本固件 ──I2C──> PCA9685 ──> 舵机
 *
 * ==================== 首次使用 ====================
 *   1. 把 secrets.h.example 复制成 secrets.h，填上你的 WiFi 和服务器地址
 *      （secrets.h 已被 .gitignore 排除，不会被提交到 Git）
 *   2. 确认服务器 config.js 里的 ESP32_TOKEN 和 secrets.h 里的一致
 *   3. 编译烧录
 *
 * ==================== 接线 ====================
 *   ESP32 3.3V ──→ PCA9685 VCC（就近加 0.1µF 去耦）
 *   ESP32 GND  ──→ PCA9685 GND
 *   ESP32 GPIO8 ─→ PCA9685 SDA
 *   ESP32 GPIO9 ─→ PCA9685 SCL
 *
 *   舵机供电走独立电源，不要从 ESP32 取电：
 *   UBEC 5V ──→ PCA9685 绿色端子 V+ / GND
 *   舵机1 底座 ──[100Ω]── PWM0
 *   舵机2 大臂 ──[100Ω]── PWM1
 *   舵机3 小臂 ──[100Ω]── PWM2
 *   舵机4 手腕 ──[100Ω]── PWM3
 *   舵机5 夹爪 ──[100Ω]── PWM4
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

#include "secrets.h"   // WiFi 账号密码、服务器地址、token（不进 Git）

// ==================== 配置 ====================
const int NUM = 5;
const int CH[NUM] = {0, 1, 2, 3, 4};
const char* NAMES[NUM] = {"底座", "大臂", "小臂", "手腕", "夹爪"};

const int STEP_DELAY = 15;   // 每步间隔(ms)，决定运动速度

// ==================== 状态 ====================
Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver(0x40);
WebSocketsClient ws;

int current[NUM] = {90, 90, 90, 90, 90};   // 当前角度
int target[NUM]  = {90, 90, 90, 90, 90};   // 目标角度
bool moving = false;                        // 是否有舵机在动
unsigned long lastStepAt = 0;

// 角度 → PCA9685 脉宽计数（500~2500us 对应 0~180°，每计数约 4.88us）
int angleToPWM(int a) {
  return map(a, 0, 180, 102, 512);
}

void writeServo(int i, int a) {
  pwm.setPWM(CH[i], 0, angleToPWM(a));
}

// ==================== 运动控制（非阻塞）====================
/*
 * 为什么不用 while + delay：
 *   delay 期间整个 loop 卡住，WebSocket 收不到新消息、心跳也发不出去，
 *   服务器会以为设备掉线。而且多轴没法同时动。
 *
 * 改成「主循环里每隔 STEP_DELAY 走一步」：
 *   所有舵机同时朝各自目标推进，loop 始终保持响应。
 */
void stepMotion() {
  if (millis() - lastStepAt < STEP_DELAY) return;
  lastStepAt = millis();

  bool anyMoving = false;
  for (int i = 0; i < NUM; i++) {
    if (current[i] != target[i]) {
      current[i] += (target[i] > current[i]) ? 1 : -1;
      writeServo(i, current[i]);
      anyMoving = true;
    }
  }

  // 刚停下来 → 回报服务器，网页端就能显示"已到位"
  if (moving && !anyMoving) {
    moving = false;
    String report = "";
    for (int i = 0; i < NUM; i++) {
      report += String(i) + ":" + String(current[i]);
      if (i < NUM - 1) report += ",";
    }
    ws.sendTXT(report);
    Serial.println("到位回报: " + report);
  }
  if (anyMoving) moving = true;
}

// ==================== WebSocket 回调 ====================
void onMessage(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("✅ 已连上服务器");
      break;

    case WStype_DISCONNECTED:
      Serial.println("❌ 与服务器断开，等待重连…");
      break;

    case WStype_TEXT: {
      String msg = String((char*)payload);
      msg.trim();
      Serial.println("收到命令: " + msg);

      // 命令格式: "舵机编号:角度"  例如 "0:45"
      int colon = msg.indexOf(':');
      if (colon > 0) {
        int idx = constrain(msg.substring(0, colon).toInt(), 0, NUM - 1);
        int tgt = constrain(msg.substring(colon + 1).toInt(), 0, 180);
        target[idx] = tgt;
        moving = true;
        Serial.printf("  %s → %d°\n", NAMES[idx], tgt);
      }
      break;
    }

    default:
      break;
  }
}

// ==================== 入口 ====================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n================================");
  Serial.println(" ESP32-S3 机械臂 · 云连接版");
  Serial.println("================================");

  // ---- PCA9685 ----
  Wire.begin(8, 9);
  pwm.begin();
  pwm.setOutputMode(true);   // 推挽输出，边沿更陡、抗干扰更好
  pwm.setPWMFreq(50);
  for (int i = 0; i < NUM; i++) writeServo(i, 90);

  // ---- 连 WiFi（STA 模式：连路由器，不是自建热点）----
  Serial.printf("连接 WiFi: %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAt < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n❌ WiFi 连接失败，检查 secrets.h 里的账号密码");
    Serial.println("   （注意：ESP32 只支持 2.4GHz，连不上 5GHz 频段）");
    return;
  }
  Serial.println("\n✅ WiFi 已连接");
  Serial.print("   本机 IP: ");
  Serial.println(WiFi.localIP());

  // ---- 连服务器 ----
  // token 放在连接地址里，服务器会校验，不对直接断开
  String path = "/?token=" + String(ESP32_TOKEN);
  ws.begin(SERVER_HOST, SERVER_PORT, path.c_str());
  ws.onEvent(onMessage);
  ws.setReconnectInterval(5000);   // 断了自动重连
  ws.enableHeartbeat(15000, 3000, 2);  // 心跳，防止中间设备掐断空闲连接

  Serial.printf("正在连接服务器 %s:%d …\n", SERVER_HOST, SERVER_PORT);
}

void loop() {
  ws.loop();      // 必须高频调用，否则收不到消息、心跳也会断
  stepMotion();
}
