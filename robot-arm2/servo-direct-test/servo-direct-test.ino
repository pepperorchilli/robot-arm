/*
 * 直驱测试：ESP32 直接输出 PWM 给一个舵机，完全绕开 PCA9685
 * 目的：判断抖动是 PCA 板的问题，还是电源/舵机的问题
 *
 * 接线（只动一根黄线，其他全不动）：
 *   舵机红线 ──→ 盒子 5V(+)
 *   舵机棕线 ──→ 盒子 GND(-)
 *   舵机黄线 ──→ ESP32 GPIO13   （从 PCA 的 PWM 口拔下来，改插这里）
 *   ESP32 GND ──→ 盒子 GND      （共地，必须，已经接好了）
 */

#include <ESP32Servo.h>

Servo servo;
const int PIN = 13;

void setup() {
  Serial.begin(115200);
  servo.attach(PIN);
  servo.write(90);
  Serial.println("舵机锁定 90°，观察 10 秒，看静止时抖不抖");
}

void loop() {
  // 来回扫动：0° -> 180° -> 0°，观察运动过程中抖不抖
  for (int a = 0; a <= 180; a += 2) {
    servo.write(a);
    delay(20);
  }
  for (int a = 180; a >= 0; a -= 2) {
    servo.write(a);
    delay(20);
  }
}
