/*
 * ESP32-S3 + PCA9685 — WebSocket 客户端
 * 主动连接阿里云服务器，接收命令控制舵机
 *
 * ==================== 接线 ====================
 *   外部电源 5V/10A(+) ──→ PCA9685 绿色端子 V+
 *   外部电源 5V/10A(-) ──→ PCA9685 绿色端子 GND
 *   ESP32 3.3V  ──→ PCA9685 VCC
 *   ESP32 GND   ──→ PCA9685 GND
 *   ESP32 GPIO8 ──→ PCA9685 SDA
 *   ESP32 GPIO9 ──→ PCA9685 SCL
 *   舵机1 底座 ── PWM0 (三针全插)
 *   舵机2 大臂 ── PWM1
 *   舵机3 小臂 ── PWM2
 *   舵机4 手腕 ── PWM3
 *   舵机5 夹爪 ── PWM4
 * ==============================================
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

// ★★★ 填你的 WiFi ★★★
const char* WIFI_SSID     = "大胃岱";
const char* WIFI_PASSWORD = "Dp3ftldy";

// ★★★ 你的服务器地址 ★★★
const char* SERVER_IP   = "8.140.198.191";
const int   SERVER_PORT = 80;

const int NUM = 5;
const int CH[NUM] = {0, 1, 2, 3, 4};
const char* NAMES[NUM] = {"底座", "大臂", "小臂", "手腕", "夹爪"};

int angles[NUM] = {90, 90, 90, 90, 90};

Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver(0x40);
WebSocketsClient ws;

int angleToPWM(int a) {
  return map(a, 0, 180, 102, 512);
}

void writeServo(int i, int a) {
  pwm.setPWM(CH[i], 0, angleToPWM(a));
}

// 收到服务器消息时调用
void onMessage(WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_TEXT) {
    String msg = String((char*)payload);
    Serial.println("收到命令: " + msg);

    // 命令格式: "舵机编号:角度"  例如 "0:45"
    int colon = msg.indexOf(':');
    if (colon > 0) {
      int idx = msg.substring(0, colon).toInt();
      int tgt = msg.substring(colon + 1).toInt();

      if (idx >= 0 && idx < NUM) {
        idx = constrain(idx, 0, NUM - 1);
        tgt = constrain(tgt, 0, 180);

        int step = (tgt > angles[idx]) ? 1 : -1;
        while (angles[idx] != tgt) {
          angles[idx] += step;
          writeServo(idx, angles[idx]);
          delay(15);
        }
        Serial.printf("%s → %d°\n", NAMES[idx], angles[idx]);

        // 回复服务器
        ws.sendTXT(String(idx) + ":" + String(angles[idx]));
      }
    }
  }
}

void setup() {
  Serial.begin(115200);

  // PCA9685 初始化
  Wire.begin(8, 9);
  pwm.begin();
  pwm.setPWMFreq(50);
  for (int i = 0; i < NUM; i++) {
    writeServo(i, 90);
  }

  // 连接 WiFi
  Serial.printf("\n连接 WiFi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi 已连接");

  // 连接服务器
  ws.begin(SERVER_IP, SERVER_PORT, "/");
  ws.onEvent(onMessage);
  Serial.printf("正在连接服务器 %s:%d ...\n", SERVER_IP, SERVER_PORT);
}

void loop() {
  ws.loop();
}
