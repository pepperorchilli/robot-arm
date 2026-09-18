/*
 * 抖动诊断①：关掉 WiFi，纯硬件 PWM 锁死 5 个舵机
 *
 * 目的：判断"静止小幅抽搐"是不是 ESP32 的 WiFi 射频耦合进信号线引起的。
 *
 * 做法：
 *   1. 保持你现在的接线不动（直驱版：GPIO 13/12/14/27/26 各接一个舵机信号）
 *   2. 烧录本程序（不连 WiFi、不开网页、不接串口也行）
 *   3. 观察 30 秒：所有舵机是否还"小幅抽搐"
 *
 * 结论判读：
 *   - 不抖了  → 元凶是 WiFi 射频/地弹串扰（不是舵机、不是电源）
 *   - 还再抖  → 是共地阻抗 / UBEC 噪声，看下一份固件
 *
 * 若你还在用 PCA9685（还没拆），把下方 PCA 段注释打开、直驱段注释掉即可。
 */

#include <ESP32Servo.h>

const int NUM = 5;
// 直驱版引脚（与 arm-5servo 一致）
const int PIN[NUM] = {13, 12, 14, 27, 26};

// ---- 如果你还没拆 PCA9685，改用下面这段 ----
// #include <Wire.h>
// #include <Adafruit_PWMServoDriver.h>
// Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver(0x40);
// void writeServo(int i, int a) { pwm.setPWM(i, 0, map(a, 0, 180, 102, 512)); }

Servo servos[NUM];

void setup() {
  // 本测试刻意不开 Serial / WiFi / WebServer / I2C，最大程度隔离干扰源

  // ---- 直驱 ----
  for (int i = 0; i < NUM; i++) {
    servos[i].attach(PIN[i]);
    servos[i].write(90);
  }

  // ---- PCA9685（若在用）----
  // Wire.begin(8, 9);
  // pwm.begin();
  // pwm.setPWMFreq(50);
  // for (int i = 0; i < NUM; i++) writeServo(i, 90);
}

void loop() {
  // 什么都不做，纯保持。观察舵机是否还抽搐。
  delay(1000);
}
