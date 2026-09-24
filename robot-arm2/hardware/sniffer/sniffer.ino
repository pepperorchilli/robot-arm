/*
 * H2 嗅探器 —— 只监听，一个字节都不发
 *
 * 想解决什么：
 *   A 档（H2）打不通舵机，但 ESP32 的 UART 已经被内部回环证明是好的，
 *   两种极性也都在固件里试过了。那最后一个可能性就是：
 *   **那三根杜邦线到底有没有真的接到 H2 的针脚上。**
 *
 * 怎么测：
 *   让 ESP32 退化成一根听筒 —— TX 传 -1，完全不去驱动总线，
 *   所以它不会干扰任何东西。然后从 Mac 走 USB（B 档）让舵机说话。
 *   如果 H2 的 RXD 这根线真的接到了 ESP32，这里就能听见舵机的回话。
 *
 *   两条 UART 分别听 GPIO16 和 GPIO17 —— 因为不知道线接在哪一根上，
 *   所以两根一起听，哪根有动静就说明线接对了。
 *
 * 前提：
 *   · 跳线在 B（这样 USB 那条路才通，舵机才会说话）
 *   · H2 的三根线还接在 ESP32 上，别拔
 *
 * 结果怎么读：
 *   听见字节 → 线是通的，而且能看出接在哪根脚上 → 那 A 档的问题在发送方向
 *   一直安静 → 那三根线跟 H2 之间根本没通（没插实 / 插错针 / 插错排针）
 */

#include "driver/uart.h"

#define PIN_A 17
#define PIN_B 16
#define BAUD  1000000

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n【H2 嗅探器】只监听，不发送任何东西");
  Serial.printf("同时听 GPIO%d 和 GPIO%d @ %u\n", PIN_B, PIN_A, BAUD);

  // TX 传 -1 → 这两条 UART 都不占用任何发送脚，纯接收
  Serial1.begin(BAUD, SERIAL_8N1, PIN_B, -1);
  Serial2.begin(BAUD, SERIAL_8N1, PIN_A, -1);
  delay(50);

  Serial.println("开始听（每 3 秒报一次活）...\n");
}

void loop() {
  while (Serial1.available()) {
    Serial.printf("  【GPIO%d 有动静】%02X\n", PIN_B, Serial1.read());
  }
  while (Serial2.available()) {
    Serial.printf("  【GPIO%d 有动静】%02X\n", PIN_A, Serial2.read());
  }

  static uint32_t t = 0;
  if (millis() - t > 3000) {
    t = millis();
    Serial.println("  ...听着呢");
  }
}
