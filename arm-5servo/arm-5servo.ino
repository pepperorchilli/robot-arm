/*
 * ESP32-S3 直驱 5 路舵机 — 正式版（绕开 PCA9685）
 * 用 ESP32Servo 库（LEDC 硬件 PWM），信号稳定不抖
 *
 * ==================== 接线（防串扰是关键！）====================
 *   舵机1 底座  黄线 ──[100Ω]── GPIO13
 *   舵机2 大臂  黄线 ──[100Ω]── GPIO12
 *   舵机3 小臂  黄线 ──[100Ω]── GPIO14
 *   舵机4 手腕  黄线 ──[100Ω]── GPIO27
 *   舵机5 夹爪  黄线 ──[100Ω]── GPIO26
 *
 *   ⚠️ 5 根信号线：分开走、别并排、别缠绕（防串扰第一要务）
 *   ⚠️ 每根信号线串 100Ω 电阻，把陡边沿磨圆，减少串扰
 *
 *   所有舵机红线 ──→ 盒子 5V(+)
 *   所有舵机棕线 ──→ 盒子 GND(-)
 *   ESP32 GND    ──→ 盒子 GND     （共地，必须）
 *
 *   PCA9685 整个拆掉，不用了
 * ================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>

const char* ssid     = "RobotArm-Test";
const char* password = "12345678";

const int NUM = 5;
const int PIN[NUM] = {13, 12, 14, 27, 26};
const char* NAMES[NUM] = {"底座", "大臂", "小臂", "手腕", "夹爪"};

const int STEP_DELAY = 40; // 每步延时(ms)，越大越慢

int angles[NUM] = {90, 90, 90, 90, 90};
Servo servos[NUM];

void writeServo(int i, int a) {
  servos[i].write(a);
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
<h2>机械臂测试（直驱版）</h2>
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

  for (int i = 0; i < NUM; i++) {
    servos[i].attach(PIN[i]);
    servos[i].write(90);
    Serial.printf("舵机%d %s [GPIO%d] → 90°\n", i + 1, NAMES[i], PIN[i]);
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
