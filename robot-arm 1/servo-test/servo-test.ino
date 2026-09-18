/*
 * ESP32-S3 + PCA9685 — 局域网测试版（信号/电源分离）
 *
 * 接线思路：
 *   PCA9685 只发 PWM 信号，不给舵机供电（VCC 接 3.3V 即可）
 *   舵机电源独立走 2进12出盒
 *
 * ==================== 接线 ====================
 * 【① 信号部分 - PCA9685 只发信号，3.3V 逻辑供电】
 *   ESP32 3.3V ──→ PCA9685 VCC
 *   ESP32 GND  ──→ PCA9685 GND
 *   ESP32 GPIO8 ──→ PCA9685 SDA
 *   ESP32 GPIO9 ──→ PCA9685 SCL
 *
 * 【② 电源部分 - 舵机供电走盒子，不碰 PCA】
 *   外部电源 5V(+)  ──→ 2进12出盒 ──→ 每个舵机红线(V+)
 *   外部电源 GND(-) ──→ 2进12出盒 ──→ 每个舵机棕线(GND)
 *
 * 【③ 舵机信号 - 只插黄线】
 *   舵机1 底座  黄线 ──→ PCA9685 PWM0
 *   舵机2 大臂  黄线 ──→ PWM1
 *   舵机3 小臂  黄线 ──→ PWM2
 *   舵机4 手腕  黄线 ──→ PWM3
 *   舵机5 夹爪  黄线 ──→ PWM4
 *   （PCA 排针的 V+/GND 脚 空着不插）
 *
 * 【④ 共地 - 必须！否则信号没有回路】
 *   ESP32 GND ──→ 2进12出盒 GND
 *
 * 【绿色端子】可空着（舵机内部有上拉电阻，能补齐信号高电平）
 *   想更稳的话，绿色端子 V+ 也可接盒子 5V（可选）
 * ==============================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

const char* ssid     = "RobotArm-Test";
const char* password = "12345678";

const int NUM = 5;
const int CH[NUM] = {0, 1, 2, 3, 4};
const char* NAMES[NUM] = {"底座", "大臂", "小臂", "手腕", "夹爪"};

const int STEP_DELAY = 40; // 每步延时(ms)，越大越慢

int angles[NUM] = {90, 90, 90, 90, 90};

Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver(0x40);
WebServer server(80);

int angleToPWM(int a) {
  // 500~2500us 对应 0~180°，PCA9685 每计数约 4.88us
  return map(a, 0, 180, 102, 512);
}

void writeServo(int i, int a) {
  pwm.setPWM(CH[i], 0, angleToPWM(a));
}

// 简单网页：每个舵机一个滑块
const char HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>机械臂测试</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,sans-serif;background:#1a1a2e;color:#fff;padding:16px}
.c{background:rgba(255,255,255,0.06);border-radius:14px;padding:12px;margin-bottom:8px}
.h{display:flex;justify-content:space-between;margin-bottom:6px}
.n{font-size:14px;font-weight:600}
.v{font-size:22px;font-weight:700;color:#00e5ff}
input[type=range]{-webkit-appearance:none;width:100%;height:6px;border-radius:3px;background:rgba(255,255,255,0.15);outline:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:24px;height:24px;border-radius:50%;background:#00e5ff;border:2px solid #fff}
button{width:100%;padding:12px;border:none;border-radius:12px;background:#ff6b6b;color:#fff;font-size:15px;font-weight:700;margin-top:6px}
</style>
</head>
<body>
<h2>机械臂测试</h2>
<div class="c"><div class="h"><div class="n">底座</div><div class="v" id="a0">90°</div></div><input type="range" id="s0" min="0" max="180" value="90"></div>
<div class="c"><div class="h"><div class="n">大臂</div><div class="v" id="a1">90°</div></div><input type="range" id="s1" min="0" max="180" value="90"></div>
<div class="c"><div class="h"><div class="n">小臂</div><div class="v" id="a2">90°</div></div><input type="range" id="s2" min="0" max="180" value="90"></div>
<div class="c"><div class="h"><div class="n">手腕</div><div class="v" id="a3">90°</div></div><input type="range" id="s3" min="0" max="180" value="90"></div>
<div class="c"><div class="h"><div class="n">夹爪</div><div class="v" id="a4">90°</div></div><input type="range" id="s4" min="0" max="180" value="90"></div>
<button onclick="rst()">全部回中</button>
<script>
for(let i=0;i<5;i++){
  let s=document.getElementById('s'+i);
  s.addEventListener('input',()=>{document.getElementById('a'+i).textContent=s.value+'°';});
  s.addEventListener('change',()=>{
    fetch('/set?servo='+i+'&angle='+s.value);
  });
}
function rst(){for(let i=0;i<5;i++){document.getElementById('s'+i).value=90;document.getElementById('a'+i).textContent='90°';fetch('/set?servo='+i+'&angle=90');}}
</script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);

  // PCA9685 初始化（只发信号，I2C 逻辑供电 3.3V）
  Wire.begin(8, 9);   // SDA=GPIO8, SCL=GPIO9
  pwm.begin();
  pwm.setPWMFreq(50);

  for (int i = 0; i < NUM; i++) {
    writeServo(i, 90);
    Serial.printf("舵机%d %s [PWM%d] → 90°\n", i + 1, NAMES[i], CH[i]);
  }

  // 开热点
  WiFi.softAP(ssid, password);
  Serial.println("\n===================================");
  Serial.printf("  WiFi: %s\n", ssid);
  Serial.printf("  密码: %s\n", password);
  Serial.printf("  网页: http://%s\n", WiFi.softAPIP().toString().c_str());
  Serial.println("===================================\n");

  server.on("/", []() {
    server.send(200, "text/html; charset=utf-8", HTML);
  });

  server.on("/set", []() {
    if (server.hasArg("servo") && server.hasArg("angle")) {
      int i = constrain(server.arg("servo").toInt(), 0, NUM - 1);
      int t = constrain(server.arg("angle").toInt(), 0, 180);

      int step = (t > angles[i]) ? 1 : -1;
      while (angles[i] != t) {
        angles[i] += step;
        writeServo(i, angles[i]);
        delay(STEP_DELAY);
      }
      Serial.printf("%s → %d°\n", NAMES[i], angles[i]);
    }
    server.send(200, "text/plain", "OK");
  });

  server.begin();
}

void loop() {
  server.handleClient();
}
