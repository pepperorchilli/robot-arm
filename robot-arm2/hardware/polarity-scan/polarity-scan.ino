/*
 * ESP32-S3 → 微雪板 H2 接线极性自动扫描
 *
 * 解决什么问题：
 *   走 H2（跳线 A）打舵机一直没反应。ESP32 自己的 UART 已经用内部回环证明
 *   是好的，剩下的未知量是"H2 那两根线该直连还是交叉"（微雪 wiki 说 TX-TX，
 *   反直觉，很容易反）。
 *
 *   与其让人拔线重插、每插一次测一轮，不如让固件自己试：
 *   ESP32 的两个 GPIO 是对称的，谁当 TX 谁当 RX 完全可以在软件里换。
 *   所以一次烧录就能把两种接法都试完，人一根线都不用动。
 *
 *   还顺带试了"发完要不要放开总线"——因为主固件的 readTelemetry() 里那句
 *   pinMode(SERVO_TX, INPUT) 就是干这个的，但如果 H2 那条路的方向是板载
 *   缓冲器 U1 自己管的，这句反而多余甚至有害。两种都试。
 *
 * 前提（跑之前的状态）：
 *   · 跳线在 A
 *   · H2 的 TX / RX / GND 三根线接在 ESP32 上（哪种接法无所谓，本固件自己试）
 *   · DC 电源接着
 *
 * 结果怎么读：
 *   哪一行出现 ✅  → 那就是正确接法，把那两行记下来，主固件照这个来
 *   四行全是 (空) → ①②都排除了，问题在 ③（缓冲器 U1 / 跳线 A / 板子本身）
 */

#include "driver/uart.h"
#include "driver/gpio.h"
#include "soc/gpio_sig_map.h"

#define PIN_A 17
#define PIN_B 16
#define BAUD  1000000

// ping ID=1：FF FF 01 02 01 FB
static const uint8_t PING[] = {0xFF, 0xFF, 0x01, 0x02, 0x01, 0xFB};

// 用指定的 TX/RX 发一个 ping，听 70ms，把收到的原始字节打出来
static void attempt(const char* tag, int txPin, int rxPin, bool releaseTx) {
  Serial1.begin(BAUD, SERIAL_8N1, rxPin, txPin);
  delay(40);

  while (Serial1.available()) Serial1.read();      // 清干净

  // 先纯听 25ms：万一总线上本来就有东西在动，这能看出来
  uint8_t idleN = 0;
  uint32_t t = millis();
  while (millis() - t < 25 && idleN < 8) {
    if (Serial1.available()) { Serial1.read(); idleN++; }
  }

  Serial1.write(PING, sizeof(PING));
  Serial1.flush();

  if (releaseTx) {
    // 把 UART 的 TX 信号从这根脚上摘下来（光 pinMode 未必真能摘干净），
    // 再切成输入 —— 这样总线交给舵机去驱动
    esp_rom_gpio_connect_out_signal(txPin, SIG_GPIO_OUT_IDX, false, false);
    pinMode(txPin, INPUT);
  }

  uint8_t buf[24];
  uint8_t n = 0;
  t = millis();
  while (millis() - t < 70 && n < sizeof(buf)) {
    if (Serial1.available()) buf[n++] = Serial1.read();
  }

  bool ok = (n >= 6) && (buf[0] == 0xFF) && (buf[1] == 0xFF);

  Serial.printf("  %-28s 静默%u | 收 %u 字节: ", tag, idleN, n);
  if (n == 0) {
    Serial.print("(空)");
  } else {
    for (uint8_t i = 0; i < n; i++) Serial.printf("%02X ", buf[i]);
  }
  Serial.println(ok ? "   ✅✅ 舵机回了！" : "");

  Serial1.end();
  delay(40);
}

static void runTest() {
  static int round = 0;
  Serial.printf("\n============ 第 %d 轮 ============\n", ++round);

  attempt("GPIO17=TXD 16=RXD 不放开", PIN_A, PIN_B, false);
  attempt("GPIO16=TXD 17=RXD 不放开", PIN_B, PIN_A, false);
  attempt("GPIO17=TXD 16=RXD 发完放开", PIN_A, PIN_B, true);
  attempt("GPIO16=TXD 17=RXD 发完放开", PIN_B, PIN_A, true);

  Serial.println("---------------------------------");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n【H2 接线极性扫描】");
  Serial.println("前提：跳线在 A，H2 三根线接着，DC 电源开着");
  Serial.println("哪一行出现 ✅✅ 就是正确接法");
}

void loop() {
  runTest();
  delay(5000);
}
