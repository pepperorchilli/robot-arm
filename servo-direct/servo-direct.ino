/*
 * ESP32-S3 + ESP32Servo 单舵机测试版（绕过 PCA9685）
 * 用 ESP32Servo 库直接控制 1 个 MG996R
 *
 * ==================== 接线 ====================
 *   舵机 信号(黄) ──→ GPIO13
 *   舵机 红线(V+)  ──→ 外部电源 5V(+)
 *   舵机 棕线(GND) ──→ 外部电源 GND(-)
 *
 *   ⚠️ 共地（必须接，否则信号没有回路，舵机不动！）：
 *        ESP32 GND ──→ 外部电源 GND(-)
 * ==============================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>

const char* ssid     = "RobotArm-Test";
const char* password = "12345678";

const int SERVO_PIN = 13;
const int STEP_DELAY = 15; // 每步延时(ms)，越大越慢

Servo servo;
WebServer server(80);

int currentAngle = 90;

// 简单网页：一个滑块
const char HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>单舵机测试</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,sans-serif;background:#1a1a2e;color:#fff;padding:16px}
.c{background:rgba(255,255,255,0.06);border-radius:14px;padding:12px;margin-bottom:8px}
.h{display:flex;justify-content:space-between;margin-bottom:6px}
.n{font-size:14px;font-weight:600}
.v{font-size:28px;font-weight:700;color:#00e5ff}
input[type=range]{-webkit-appearance:none;width:100%;height:6px;border-radius:3px;background:rgba(255,255,255,0.15);outline:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:24px;height:24px;border-radius:50%;background:#00e5ff;border:2px solid #fff}
button{width:100%;padding:12px;border:none;border-radius:12px;background:#ff6b6b;color:#fff;font-size:15px;font-weight:700;margin-top:6px}
</style>
</head>
<body>
<h2>单舵机测试</h2>
<div class="c"><div class="h"><div class="n">舵机</div><div class="v" id="a0">90°</div></div><input type="range" id="s0" min="0" max="180" value="90"></div>
<button onclick="rst()">回中</button>
<script>
let s=document.getElementById('s0');
s.addEventListener('input',()=>{document.getElementById('a0').textContent=s.value+'°';});
s.addEventListener('change',()=>{fetch('/set?servo=0&angle='+s.value);});
function rst(){s.value=90;document.getElementById('a0').textContent='90°';fetch('/set?servo=0&angle=90');}
</script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);

  // 挂载舵机：500~2500us 对应 0~180°
  servo.attach(SERVO_PIN, 500, 2500);
  servo.write(90);
  Serial.printf("舵机 [GPIO%d] → 90°\n", SERVO_PIN);

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
    if (server.hasArg("angle")) {
      int t = constrain(server.arg("angle").toInt(), 0, 180);
      int step = (t > currentAngle) ? 1 : -1;
      while (currentAngle != t) {
        currentAngle += step;
        servo.write(currentAngle);
        delay(STEP_DELAY);
      }
      Serial.printf("舵机 → %d°\n", currentAngle);
    }
    server.send(200, "text/plain", "OK");
  });

  server.begin();
}

void loop() {
  server.handleClient();
}
