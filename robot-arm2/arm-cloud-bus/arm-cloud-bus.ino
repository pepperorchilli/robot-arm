/*
 * ESP32-S3 + STS3215 总线舵机 — 云连接版（合并固件）
 *
 * 这是 arm-cloud/（云连接）和 arm-bus-servo/（总线舵机）的合并版本：
 *   保留 arm-cloud 的 WiFi STA + WebSocket 客户端 + token 鉴权
 *   底层驱动从 PCA9685 换成 STS3215 串行总线
 *
 * ⚠️ 尚未上机验证。拿到舵机后请先按 arm-bus-servo/README.md 的
 *    「四步验证法」逐个确认，再烧本固件。
 *
 * ==================== 数据流 ====================
 *   浏览器 ──HTTP──> Node 服务器 ──WebSocket──> 本固件 ──TTL 总线──> 5 个舵机
 *
 * ==================== 和 PCA9685 版的关键区别 ====================
 *   PCA9685 版：软件在主循环里逐度推进（stepMotion），因为模拟舵机
 *               只能"给多少脉宽转多少角度"。
 *   总线舵机版：把「目标位置 + 到达时间」一起下发给舵机，
 *               舵机内部自己按时间平滑走位。**不需要软件插补**，
 *               一条 SYNC_WRITE 指令让 5 轴同时按各自速度运动。
 *
 * ==================== 供电 ====================
 *   STS3215 额定 7.4V（6–8.4V），**不是 5V**！
 *   3S 锂电(12.6V) 要经降压模块到 7.4V（≥5A），或直接用 2S 锂电。
 *
 * ==================== 接线 ====================
 *   5 个舵机的 DATA 并联成一条总线；V+ / GND 分别并到 7.4V / GND
 *
 *   ESP32 GPIO17 (TX) ──[1kΩ]──→ 总线 DATA   ← 1kΩ 上拉，回读时才需要
 *   ESP32 GPIO16 (RX) ──────────→ 总线 DATA
 *   ESP32 GND         ──────────→ 舵机 GND（必须共地）
 *
 * ==================== 首次使用 ====================
 *   1. cp secrets.h.example secrets.h，填 WiFi 和服务器地址
 *   2. 确认 SERVO_ID[] 和实际改好的舵机 ID 一致（出厂全是 1，必须逐个改）
 *   3. 烧录
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <HardwareSerial.h>

#include "secrets.h"   // WiFi 账号密码、服务器地址、token（不进 Git）

// ==================== 配置 ====================
#define SERVO_RX   16
#define SERVO_TX   17
#define SERVO_BAUD 1000000     // STS3215 出厂默认 1Mbps

const int NUM = 6;   // SO-ARM101 有 6 个关节
// 舵机 ID，按物理顺序从下到上：底座 → 肩 → 肘 → 腕俯仰 → 腕旋转 → 夹爪
// ⚠️ 出厂全是 1，必须逐个改成 1~6，否则总线冲突
uint8_t SERVO_ID[NUM] = {1, 2, 3, 4, 5, 6};
const char* NAMES[NUM] = {"底座", "肩部", "肘部", "腕俯仰", "腕旋转", "夹爪"};

const uint16_t POS_CENTER = 2048;   // 0~4095 对应 0~360°，2048 是 180°（机械中位）
const int MOVE_TIME = 400;          // 到达目标的毫秒数（舵机内部平滑）

// ==================== STS/SCS 协议寄存器 ====================
#define REG_TORQUE_ENABLE  0x28
#define REG_GOAL_POSITION  0x2A
#define REG_GOAL_TIME      0x2C
#define REG_PRESENT_POS    0x38   // 只读
#define REG_PRESENT_LOAD   0x3C   // 只读（力反馈用）

#define INST_PING        0x01
#define INST_READ        0x02
#define INST_WRITE       0x03
#define INST_SYNC_WRITE  0x83
#define BROADCAST_ID     0xFE

HardwareSerial servoBus(1);
WebSocketsClient ws;

// ==================== 状态 ====================
int target[NUM] = {90, 90, 90, 90, 90};   // 目标角度（网页的 0~180）

// 到位回报：发完指令后等 MOVE_TIME 再回报，避免高频刷屏
bool reportPending = false;
unsigned long reportAt = 0;

// ==================== 协议底层 ====================
/*
 * 包结构（Feetech SCS / Dynamixel 1.0 兼容）：
 *   [0] 0xFF  帧头
 *   [1] 0xFF  帧头
 *   [2] ID
 *   [3] LEN = 参数个数 + 2
 *   [4] 指令
 *   [5..] 参数
 *   [末] 校验和 = ~(ID + LEN + 指令 + 全部参数) 取低 8 位
 */
void sendPacket(uint8_t* p) {
  uint8_t total = 6 + (p[3] - 2);
  uint8_t sum = 0;
  for (uint8_t i = 2; i < total - 1; i++) sum += p[i];
  p[total - 1] = ~sum;
  servoBus.write(p, total);
  servoBus.flush();
}

void writeByte(uint8_t id, uint8_t reg, uint8_t v) {
  uint8_t p[7] = {0xFF, 0xFF, id, 4, INST_WRITE, reg, v};
  sendPacket(p);
}

void writeWord(uint8_t id, uint8_t reg, uint16_t v) {
  uint8_t p[8] = {0xFF, 0xFF, id, 5, INST_WRITE, reg,
                  (uint8_t)(v & 0xFF), (uint8_t)(v >> 8)};
  sendPacket(p);
}

/*
 * SYNC_WRITE：一条指令让所有舵机同时收到各自的目标位置。
 * 这是总线舵机相比 PCA9685 的优势之一 —— 5 轴严格同步启动，
 * 不会因为逐个发送而产生几毫秒的时序错位。
 */
void syncWritePosition(const uint16_t* pos, uint8_t num) {
  const uint8_t dataLen = 2;
  uint8_t params = 2 + (1 + dataLen) * num;
  uint8_t total = 6 + params;

  uint8_t p[40];
  if (total > sizeof(p)) return;

  p[0] = 0xFF; p[1] = 0xFF;
  p[2] = BROADCAST_ID;
  p[3] = params + 2;
  p[4] = INST_SYNC_WRITE;
  p[5] = REG_GOAL_POSITION;
  p[6] = dataLen;

  uint8_t k = 7;
  for (uint8_t i = 0; i < num; i++) {
    p[k++] = SERVO_ID[i];
    p[k++] = pos[i] & 0xFF;
    p[k++] = (pos[i] >> 8) & 0xFF;
  }
  sendPacket(p);
}

// ==================== 角度换算 ====================
// 网页的 0~180° 映射到舵机量程的一半（0~2048），另一半预留给需要更大行程的关节
uint16_t angleToPos(int a) {
  a = constrain(a, 0, 180);
  return (uint16_t)map(a, 0, 180, 0, POS_CENTER);
}

// 把当前所有目标角度一次性同步下发
void applyTargets() {
  uint16_t pos[NUM];
  for (int i = 0; i < NUM; i++) pos[i] = angleToPos(target[i]);
  syncWritePosition(pos, NUM);

  reportPending = true;
  reportAt = millis() + MOVE_TIME + 50;
}

// ==================== 回读（力反馈用，需 1kΩ 上拉）====================
/*
 * 半双工总线：发完必须把 TX 释放成高阻态，舵机才能驱动同一条线回话。
 * 硬件上需要 TX 到 3.3V 接一个 1kΩ 上拉电阻。
 */
uint16_t readReg(uint8_t id, uint8_t reg) {
  uint8_t p[8] = {0xFF, 0xFF, id, 4, INST_READ, reg, 2, 0};
  sendPacket(p);

  pinMode(SERVO_TX, INPUT);        // 释放总线

  uint8_t buf[8] = {0};
  uint8_t got = 0;
  uint32_t t0 = millis();
  while (got < 8 && millis() - t0 < 20) {
    if (servoBus.available()) buf[got++] = servoBus.read();
  }
  while (servoBus.available()) servoBus.read();

  pinMode(SERVO_TX, OUTPUT);
  if (buf[0] != 0xFF || buf[1] != 0xFF) return 0xFFFF;   // 无有效回复
  return buf[5] | (buf[6] << 8);
}

uint16_t readPosition(uint8_t i) { return readReg(SERVO_ID[i], REG_PRESENT_POS); }
uint16_t readLoad(uint8_t i)     { return readReg(SERVO_ID[i], REG_PRESENT_LOAD); }

// ==================== WebSocket ====================
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
        applyTargets();     // 立即下发，舵机自己平滑走位
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
  Serial.println(" ESP32-S3 机械臂 · 云连接 + 总线舵机");
  Serial.println("================================");

  // ---- 总线舵机初始化 ----
  servoBus.begin(SERVO_BAUD, SERIAL_8N1, SERVO_RX, SERVO_TX);
  for (int i = 0; i < NUM; i++) {
    writeByte(SERVO_ID[i], REG_TORQUE_ENABLE, 1);   // 开扭矩
    writeWord(SERVO_ID[i], REG_GOAL_TIME, MOVE_TIME); // 设定到达时间
  }
  delay(100);
  applyTargets();   // 全部回中
  Serial.println("总线舵机初始化完成（全部回中 90°）");

  // ---- 连 WiFi（STA 模式）----
  Serial.printf("连接 WiFi: %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAt < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n❌ WiFi 连接失败，检查 secrets.h");
    Serial.println("   （ESP32 只支持 2.4GHz，连不上 5GHz）");
    return;
  }
  Serial.println("\n✅ WiFi 已连接");
  Serial.print("   本机 IP: ");
  Serial.println(WiFi.localIP());

  // ---- 连服务器（token 放在连接地址里，服务器会校验）----
  String path = "/?token=" + String(ESP32_TOKEN);
  ws.begin(SERVER_HOST, SERVER_PORT, path.c_str());
  ws.onEvent(onMessage);
  ws.setReconnectInterval(5000);
  ws.enableHeartbeat(15000, 3000, 2);

  Serial.printf("正在连接服务器 %s:%d …\n", SERVER_HOST, SERVER_PORT);
}

void loop() {
  ws.loop();     // 必须高频调用，否则收不到消息、心跳也会断

  // 到位回报
  if (reportPending && millis() >= reportAt) {
    reportPending = false;
    String report = "";
    for (int i = 0; i < NUM; i++) {
      report += String(i) + ":" + String(target[i]);
      if (i < NUM - 1) report += ",";
    }
    ws.sendTXT(report);
    Serial.println("到位回报: " + report);
  }
}
