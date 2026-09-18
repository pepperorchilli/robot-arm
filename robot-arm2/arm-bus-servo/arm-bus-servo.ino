/*
 * ESP32-S3 + Feetech STS3215 总线舵机（第一版）
 *
 * ⚠️ 本固件按 Feetech 官方协议文档编写，尚未上机验证。
 *    第一次拿到舵机，请先按 README 的「四步验证法」逐个确认，别直接跑整套。
 *
 * ==================== 为什么换总线舵机 ====================
 * MG996R 是模拟 PWM 舵机，靠「量脉宽长度」决定角度。ESP32 的 WiFi 射频
 * 耦合进信号线，脉宽被撑长或削短，舵机就当成一次角度变化 → 静止抽搐。
 *
 * STS3215 走数字串口协议，每条指令带校验和。噪声混进来要么被电平阈值
 * 滤掉，要么校验失败被舵机直接丢弃 —— 永远不会被「翻译成角度」。
 * 所以静止时纹丝不动，这是协议层面免疫，不是靠滤波压制。
 *
 * ==================== 供电（和之前完全不同）====================
 *   STS3215 额定 7.4V（范围 6–8.4V），**不是 5V**！
 *   - 方案A：2S 锂电直驱 7.4V（推荐，最省事）
 *   - 方案B：现有 12.6V 锂电 → 降压模块到 7.4V（≥5A）
 *   之前的 UBEC 5V 20A 用不上了。
 *
 *   每个舵机堵转电流约 2–3A，5 个同时动峰值很大：
 *   供电线要粗（≥20AWG），地线同样要粗。
 *
 * ==================== 接线（一条 3 线总线手拉手）====================
 *   5 个舵机的 DATA 全部并联成一条总线
 *   5 个舵机的 V+ / GND 分别并到 7.4V / GND
 *
 *   ESP32 GPIO17 (TX) ──[1kΩ]──→ 总线 DATA
 *   ESP32 GPIO16 (RX) ──────────→ 总线 DATA
 *   ESP32 GND         ──────────→ 舵机 GND（必须共地）
 *
 *   ⚠️ 只发指令时，TX 直连即可。
 *   ⚠️ 要回读（力反馈/读角度）时，必须加 1kΩ 上拉到 3.3V，
 *      因为总线是半双工的，发完要把 TX 释放成高阻态才能让舵机说话。
 *
 * ==================== 舵机 ID ====================
 *   出厂全是 ID=1！必须逐个改成 1~5，否则总线冲突。
 *   用本固件的串口命令 `id <旧ID> <新ID>` 逐个改，
 *   或者买 Feetech 官方 FE-URT-1 调试板用图形界面改（更省事）。
 *
 * ==================== 串口命令（115200）====================
 *   help              显示帮助
 *   scan              扫描总线上有哪些 ID
 *   ping <id>         测试某个舵机是否在线
 *   id <旧id> <新id>  修改舵机 ID（改完断电重启才生效）
 *   move <id> <角度>  单个舵机转到指定角度（0~180）
 *   all <角度>        所有舵机转到同一角度
 *   center            全部回中（90°）
 *   torque <id> <0|1> 关闭/打开扭矩（改 ID 前必须先关）
 *   w                 开启 WiFi + 网页控制
 */

#include <WiFi.h>
#include <WebServer.h>
#include <HardwareSerial.h>

// ==================== 配置 ====================
#define SERVO_RX        16
#define SERVO_TX        17
#define SERVO_BAUD      1000000     // STS3215 出厂默认 1Mbps

const char* WIFI_SSID = "RobotArm-Test";
const char* WIFI_PASS = "12345678";

const int NUM = 5;
// 改好 ID 后填这里，顺序 = 底座/大臂/小臂/手腕/夹爪
uint8_t SERVO_ID[NUM] = {1, 2, 3, 4, 5};
const char* NAMES[NUM] = {"底座", "大臂", "小臂", "手腕", "夹爪"};

const uint16_t POS_MIN = 0;         // 0°
const uint16_t POS_MAX = 4095;      // 360°
const uint16_t POS_CENTER = 2048;   // 180°（舵机机械中位）

const int MOVE_TIME = 300;          // 到达目标的毫秒数（舵机硬件平滑）

// ==================== STS/SCS 协议寄存器 ====================
#define REG_TORQUE_ENABLE  0x28
#define REG_GOAL_POSITION  0x2A
#define REG_GOAL_TIME      0x2C
#define REG_PRESENT_POS    0x38     // 当前位置（只读）
#define REG_PRESENT_LOAD   0x3C     // 当前负载/电流（只读，力反馈用）

#define INST_PING   0x01
#define INST_READ   0x02
#define INST_WRITE  0x03
#define INST_SYNC_WRITE 0x83

#define BROADCAST_ID 0xFE

HardwareSerial servoBus(1);
WebServer server(80);
bool wifiOn = false;

// ==================== 协议底层 ====================
/*
 * 包结构（Feetech SCS / Dynamixel 1.0 兼容）：
 *   [0] 0xFF        帧头
 *   [1] 0xFF        帧头
 *   [2] ID
 *   [3] LEN = 参数个数 + 2
 *   [4] 指令
 *   [5..] 参数
 *   [末] 校验和 = ~(ID + LEN + 指令 + 全部参数) 取低 8 位
 */
void sendPacket(uint8_t* p) {
  uint8_t total = 6 + (p[3] - 2);   // LEN 已包含指令和校验和
  uint8_t sum = 0;
  for (uint8_t i = 2; i < total - 1; i++) sum += p[i];
  p[total - 1] = ~sum;
  servoBus.write(p, total);
  servoBus.flush();
}

// 写寄存器：reg 起始地址，data 为要写的 n 个字节
void writeReg(uint8_t id, uint8_t reg, const uint8_t* data, uint8_t n) {
  uint8_t p[6 + 8];
  if (n > 8) return;
  p[0] = 0xFF; p[1] = 0xFF; p[2] = id;
  p[3] = n + 3;        // 参数 = 1个地址 + n个数据 → LEN = (1+n) + 2
  p[4] = INST_WRITE;
  p[5] = reg;
  for (uint8_t i = 0; i < n; i++) p[6 + i] = data[i];
  sendPacket(p);
}

// 便捷：写 1 字节
void writeByte(uint8_t id, uint8_t reg, uint8_t v) {
  writeReg(id, reg, &v, 1);
}

// 便捷：写 2 字节（小端）
void writeWord(uint8_t id, uint8_t reg, uint16_t v) {
  uint8_t d[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
  writeReg(id, reg, d, 2);
}

// 同步写：一条指令让所有舵机同时到达目标（5 轴协调运动的关键）
void syncWritePosition(const uint16_t* pos, uint8_t num) {
  const uint8_t dataLen = 2;
  uint8_t params = 2 + (1 + dataLen) * num;   // 地址 + 数据长度 + (ID+数据)*个数
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

// ==================== 回读（力反馈用，需要 1kΩ 上拉）====================
/*
 * 半双工总线：ESP32 发完必须把 TX 释放成高阻态，舵机才能驱动同一条线回话。
 * 硬件上需要 TX 到 3.3V 接一个 1kΩ 上拉电阻。
 */
void readReg(uint8_t id, uint8_t reg, uint8_t n, uint8_t* out) {
  uint8_t p[8];
  p[0] = 0xFF; p[1] = 0xFF; p[2] = id;
  p[3] = 4;              // 参数 = 地址 + 读取长度
  p[4] = INST_READ;
  p[5] = reg;
  p[6] = n;
  sendPacket(p);         // 注意：sendPacket 里已 flush

  // 释放总线，让舵机能回话
  pinMode(SERVO_TX, INPUT);

  // 读回包：FF FF ID LEN ERR 数据... 校验和
  uint8_t expect = 6 + n;
  uint8_t got = 0;
  uint32_t t0 = millis();
  while (got < expect && millis() - t0 < 20) {
    if (servoBus.available()) out[got++] = servoBus.read();
  }
  while (servoBus.available()) servoBus.read();   // 清干净

  pinMode(SERVO_TX, OUTPUT);
}

// 读当前位置（0~4095）
uint16_t readPosition(uint8_t id) {
  uint8_t buf[10] = {0};
  readReg(id, REG_PRESENT_POS, 2, buf);
  if (buf[0] != 0xFF || buf[1] != 0xFF) return 0xFFFF;   // 没收到有效回复
  return buf[5] | (buf[6] << 8);
}

// 读当前负载（力反馈：负载 ≈ 力矩，夹爪抓握力度/碰撞检测用）
uint16_t readLoad(uint8_t id) {
  uint8_t buf[10] = {0};
  readReg(id, REG_PRESENT_LOAD, 2, buf);
  if (buf[0] != 0xFF || buf[1] != 0xFF) return 0xFFFF;
  return buf[5] | (buf[6] << 8);
}

// ==================== 上层动作 ====================
uint16_t angleToPos(int a) {
  // 0~180° 映射到舵机量程的一半（0~2048），机械中位在 2048
  a = constrain(a, 0, 180);
  return (uint16_t)map(a, 0, 180, POS_MIN, POS_CENTER);
}

void setTorque(uint8_t id, bool on) {
  writeByte(id, REG_TORQUE_ENABLE, on ? 1 : 0);
}

void moveOne(uint8_t id, int angle) {
  writeWord(id, REG_GOAL_POSITION, angleToPos(angle));
}

void moveAll(int angle) {
  uint16_t pos[NUM];
  for (int i = 0; i < NUM; i++) pos[i] = angleToPos(angle);
  syncWritePosition(pos, NUM);
}

// 逐个开扭矩 + 设运动时间（舵机硬件平滑，不用软件 delay 循环）
void initServos() {
  for (int i = 0; i < NUM; i++) {
    setTorque(SERVO_ID[i], true);
    writeWord(SERVO_ID[i], REG_GOAL_TIME, MOVE_TIME);
  }
}

// ==================== 网页 ====================
const char HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>机械臂（总线舵机）</title><style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,sans-serif;background:#1a1a2e;color:#fff;padding:16px}
.c{background:rgba(255,255,255,.06);border-radius:14px;padding:12px;margin-bottom:8px}
.h{display:flex;justify-content:space-between;margin-bottom:6px}
.n{font-size:14px;font-weight:600}.v{font-size:22px;font-weight:700;color:#00e5ff}
input[type=range]{-webkit-appearance:none;width:100%;height:6px;border-radius:3px;background:rgba(255,255,255,.15);outline:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:24px;height:24px;border-radius:50%;background:#00e5ff;border:2px solid #fff}
button{width:100%;padding:12px;border:none;border-radius:12px;background:#ff6b6b;color:#fff;font-size:15px;font-weight:700;margin-top:6px}
</style></head><body><h2>机械臂（总线舵机）</h2>
<div id="cards"></div><button onclick="fetch('/center')">全部回中</button>
<script>
const NAMES=['底座','大臂','小臂','手腕','夹爪'];
let html='';
for(let i=0;i<5;i++){html+='<div class="c"><div class="h"><div class="n">'+NAMES[i]+'</div><div class="v" id="a'+i+'">90°</div></div><input type="range" id="s'+i+'" min="0" max="180" value="90"></div>';}
document.getElementById('cards').innerHTML=html;
for(let i=0;i<5;i++){let s=document.getElementById('s'+i);
s.addEventListener('input',()=>{document.getElementById('a'+i).textContent=s.value+'°';});
s.addEventListener('change',()=>{fetch('/set?servo='+i+'&angle='+s.value);});}
</script></body></html>
)rawliteral";

void setupWeb() {
  server.on("/", []() { server.send(200, "text/html; charset=utf-8", HTML); });
  server.on("/set", []() {
    if (server.hasArg("servo") && server.hasArg("angle")) {
      int i = constrain(server.arg("servo").toInt(), 0, NUM - 1);
      int a = constrain(server.arg("angle").toInt(), 0, 180);
      moveOne(SERVO_ID[i], a);
      Serial.printf("[网页] %s → %d°\n", NAMES[i], a);
    }
    server.send(200, "text/plain", "OK");
  });
  server.on("/center", []() { moveAll(90); server.send(200, "text/plain", "OK"); });
  server.begin();
}

// ==================== 串口命令 ====================
void printHelp() {
  Serial.println("\n========== 串口命令 ==========");
  Serial.println("  help              显示本帮助");
  Serial.println("  scan              扫描总线上的舵机 ID");
  Serial.println("  ping <id>         测试舵机是否在线");
  Serial.println("  id <旧id> <新id>  修改舵机 ID（改完需断电重启）");
  Serial.println("  torque <id> <0|1> 关闭/打开扭矩（改 ID 前必须先关）");
  Serial.println("  move <id> <角度>  单个舵机转动（0~180）");
  Serial.println("  all <角度>        所有舵机同时转动");
  Serial.println("  center            全部回中（90°）");
  Serial.println("  read <id>         读取位置和负载（需 1kΩ 上拉）");
  Serial.println("  w                 开启 WiFi + 网页控制");
  Serial.println("==============================\n");
}

void handleCommand(String line) {
  line.trim();
  if (line.length() == 0) return;

  int sp1 = line.indexOf(' ');
  String cmd = (sp1 < 0) ? line : line.substring(0, sp1);
  String rest = (sp1 < 0) ? "" : line.substring(sp1 + 1);
  rest.trim();

  if (cmd == "help") {
    printHelp();
  } else if (cmd == "scan") {
    Serial.println("扫描总线上 1~10 号…");
    for (uint8_t id = 1; id <= 10; id++) {
      uint8_t p[] = {0xFF, 0xFF, id, 2, INST_PING, 0};
      sendPacket(p);
      delay(5);
      if (servoBus.available()) {
        while (servoBus.available()) servoBus.read();
        Serial.printf("  ✅ 发现舵机 ID = %d\n", id);
      }
    }
    Serial.println("扫描完成。出厂默认都是 1，所以全是 1 号要逐个改。");
  } else if (cmd == "ping") {
    uint8_t id = rest.toInt();
    uint8_t p[] = {0xFF, 0xFF, id, 2, INST_PING, 0};
    sendPacket(p);
    delay(10);
    bool ok = servoBus.available();
    while (servoBus.available()) servoBus.read();
    Serial.printf("  %s ID=%d\n", ok ? "✅ 在线" : "❌ 无响应", id);
  } else if (cmd == "id") {
    int sp = rest.indexOf(' ');
    if (sp < 0) { Serial.println("用法: id <旧id> <新id>"); return; }
    uint8_t oldId = rest.substring(0, sp).toInt();
    uint8_t newId = rest.substring(sp + 1).toInt();
    setTorque(oldId, false);          // 改 ID 前必须先关扭矩
    delay(20);
    writeByte(oldId, 0x05, newId);    // 0x05 = ID 寄存器（EEPROM）
    delay(50);
    Serial.printf("  已把 %d 改成 %d。⚠️ 断电重启后生效，且总线上不能同时有两个相同 ID\n",
                  oldId, newId);
  } else if (cmd == "torque") {
    int sp = rest.indexOf(' ');
    if (sp < 0) { Serial.println("用法: torque <id> <0|1>"); return; }
    setTorque(rest.substring(0, sp).toInt(), rest.substring(sp + 1).toInt() != 0);
    Serial.println("  OK");
  } else if (cmd == "move") {
    int sp = rest.indexOf(' ');
    if (sp < 0) { Serial.println("用法: move <id> <角度>"); return; }
    uint8_t id = rest.substring(0, sp).toInt();
    int a = rest.substring(sp + 1).toInt();
    moveOne(id, a);
    Serial.printf("  ID=%d → %d°\n", id, a);
  } else if (cmd == "all") {
    moveAll(rest.toInt());
    Serial.printf("  全部 → %d°\n", rest.toInt());
  } else if (cmd == "center") {
    moveAll(90);
    Serial.println("  全部回中 90°");
  } else if (cmd == "read") {
    uint8_t id = rest.toInt();
    uint16_t pos = readPosition(id);
    uint16_t load = readLoad(id);
    if (pos == 0xFFFF) {
      Serial.println("  ❌ 读不到。检查：TX 是否接了 1kΩ 上拉到 3.3V");
    } else {
      Serial.printf("  ID=%d 位置=%d (%.1f°) 负载=%d\n",
                    id, pos, pos * 360.0 / 4096.0, load);
    }
  } else if (cmd == "w") {
    WiFi.softAP(WIFI_SSID, WIFI_PASS);
    setupWeb();
    wifiOn = true;
    Serial.printf("\n✅ WiFi: %s  密码: %s\n   网页: http://%s\n\n",
                  WIFI_SSID, WIFI_PASS, WiFi.softAPIP().toString().c_str());
  } else {
    Serial.printf("未知命令: %s（试试 help）\n", cmd.c_str());
  }
}

// ==================== 入口 ====================
void setup() {
  Serial.begin(115200);
  delay(300);
  servoBus.begin(SERVO_BAUD, SERIAL_8N1, SERVO_RX, SERVO_TX);

  Serial.println("\n\n================================");
  Serial.println(" ESP32-S3 + STS3215 总线舵机");
  Serial.println("================================");
  Serial.println("固件尚未上机验证，请先按顺序确认：");
  Serial.println("  1) scan   —— 确认能发现舵机");
  Serial.println("  2) ping 1 —— 确认通信正常");
  Serial.println("  3) id 1 2 —— 把第二个舵机改成 2 号");
  Serial.println("  4) move 1 90 —— 确认能转动");
  printHelp();

  initServos();
}

void loop() {
  if (Serial.available()) {
    handleCommand(Serial.readStringUntil('\n'));
  }
  if (wifiOn) server.handleClient();
}
