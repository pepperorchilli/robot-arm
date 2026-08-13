/*
 * ESP32-S3 + MG996R 单舵机 — WiFi 网页遥控
 *
 * ESP32 创建 WiFi 热点，手机/电脑连接后打开网页操控舵机。
 *
 * ==================== 接线说明 ====================
 * ⚠️ 舵机必须使用外部电源供电（5V-6V），不可从 ESP32 取电！
 * ⚠️ 外部电源 GND 必须与 ESP32 GND 共地！
 *
 *   舵机红线 (VCC)  → 外部电源正极
 *   舵机棕线 (GND)  → 外部电源负极 + ESP32 GND
 *   舵机橙线 (SIG)  → GPIO 13
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>

const char* ssid     = "Servo-Test";   // WiFi 名称
const char* password = "12345678";     // WiFi 密码

const int SERVO_PIN = 13;
Servo myServo;
int angle = 90;

WebServer server(80);

const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>舵机遥控</title>
<style>
  *{margin:0;padding:0;box-sizing:border-box}
  body{
    font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
    background:#1a1a2e;color:#fff;display:flex;
    align-items:center;justify-content:center;
    min-height:100vh;padding:16px;
  }
  .card{
    background:rgba(255,255,255,0.06);
    border-radius:24px;padding:28px 20px;
    max-width:380px;width:100%;
    border:1px solid rgba(255,255,255,0.1);
  }
  h1{text-align:center;font-size:20px;margin-bottom:20px}

  .angle-display{text-align:center;margin-bottom:16px}
  .angle-num{font-size:64px;font-weight:700;color:#00e5ff}
  .angle-deg{font-size:20px;color:rgba(255,255,255,0.4)}

  input[type=range]{
    -webkit-appearance:none;width:100%;height:8px;
    border-radius:4px;background:rgba(255,255,255,0.15);
    outline:none;margin-bottom:18px;
  }
  input[type=range]::-webkit-slider-thumb{
    -webkit-appearance:none;width:36px;height:36px;
    border-radius:50%;background:#00e5ff;cursor:pointer;
    border:3px solid #fff;box-shadow:0 0 20px rgba(0,229,255,0.5);
  }

  .presets{display:flex;gap:8px;margin-bottom:14px}
  .presets button{
    flex:1;padding:10px 0;border:none;border-radius:10px;
    background:rgba(255,255,255,0.1);color:#fff;
    font-size:14px;font-weight:600;cursor:pointer;
  }
  .presets button.active{background:#00e5ff;color:#1a1a2e}

  .fine{display:flex;justify-content:center;gap:10px;margin-bottom:16px}
  .fine button{
    width:48px;height:48px;border-radius:50%;border:none;
    background:rgba(255,255,255,0.1);color:#fff;
    font-size:18px;font-weight:700;cursor:pointer;
  }
  .fine button:active{background:#00e5ff;color:#1a1a2e}

  .sweep-btn{
    width:100%;padding:14px;border:none;border-radius:14px;
    background:linear-gradient(135deg,#00e5ff,#0077ff);
    color:#fff;font-size:16px;font-weight:700;cursor:pointer;
  }

  .status{text-align:center;margin-top:14px;font-size:12px;color:rgba(255,255,255,0.4)}
</style>
</head>
<body>
<div class="card">
  <h1>🤖 舵机遥控</h1>

  <div class="angle-display">
    <span class="angle-num" id="ang">90</span>
    <span class="angle-deg">°</span>
  </div>

  <input type="range" id="slider" min="0" max="180" value="90">

  <div class="presets">
    <button onclick="go(0)">0°</button>
    <button onclick="go(45)">45°</button>
    <button onclick="go(90)" class="active">90°</button>
    <button onclick="go(135)">135°</button>
    <button onclick="go(180)">180°</button>
  </div>

  <div class="fine">
    <button onclick="adj(-5)">-5</button>
    <button onclick="adj(-1)">-1</button>
    <button onclick="adj(1)">+1</button>
    <button onclick="adj(5)">+5</button>
  </div>

  <button class="sweep-btn" id="sw" onclick="sweep()">🔄 扫描 0°→180°</button>

  <div class="status">已连接</div>
</div>

<script>
  let last = 90, sweeping = false;

  function show(a) {
    document.getElementById('ang').textContent = a;
    document.getElementById('slider').value = a;
    document.querySelectorAll('.presets button').forEach(b => {
      b.classList.toggle('active', +b.textContent.replace('°','') === a);
    });
  }

  function send(a) {
    if (a === last) return;
    last = a;
    fetch('/set?angle=' + a).then(r => r.text()).then(v => show(+v));
  }

  const s = document.getElementById('slider');
  s.addEventListener('input', () => show(+s.value));
  s.addEventListener('change', () => send(+s.value));

  function go(a)   { send(a); }
  function adj(d)  { send(Math.min(180, Math.max(0, (+document.getElementById('ang').textContent) + d))); }

  async function sweep() {
    if (sweeping) return;
    sweeping = true;
    document.getElementById('sw').textContent = '扫描中...';
    await fetch('/sweep').then(r => r.text()).then(v => show(+v));
    sweeping = false;
    document.getElementById('sw').textContent = '🔄 扫描 0°→180°';
  }

  fetch('/angle').then(r => r.text()).then(v => { last = +v; show(+v); });
</script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);

  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN, 500, 2500);
  myServo.write(angle);

  WiFi.softAP(ssid, password);
  Serial.println("\n===================================");
  Serial.println("  单舵机 WiFi 遥控");
  Serial.println("===================================");
  Serial.printf("  📶 WiFi: %s\n", ssid);
  Serial.printf("  🔑 密码: %s\n", password);
  Serial.printf("  🌐 网页: http://%s\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("  🔌 GPIO: %d\n", SERVO_PIN);
  Serial.println("===================================\n");

  server.on("/", []() {
    server.send(200, "text/html; charset=utf-8", HTML_PAGE);
  });

  server.on("/angle", []() {
    server.send(200, "text/plain", String(angle));
  });

  server.on("/set", []() {
    if (server.hasArg("angle")) {
      int target = constrain(server.arg("angle").toInt(), 0, 180);
      int step = (target > angle) ? 1 : -1;
      while (angle != target) {
        angle += step;
        myServo.write(angle);
        delay(15);
      }
      Serial.printf("角度 → %d°\n", angle);
    }
    server.send(200, "text/plain", String(angle));
  });

  server.on("/sweep", []() {
    server.send(200, "text/plain", String(angle));
    for (int a = 0; a <= 180; a++) { myServo.write(a); delay(10); }
    for (int a = 180; a >= 0; a--) { myServo.write(a); delay(10); }
    myServo.write(angle);
  });

  server.begin();
}

void loop() {
  server.handleClient();
}
