/*
 * ESP32-S3 串口自检（接到微雪板 H2 打不通舵机时用）
 *
 * 为什么要有这个：
 *   走 H2（跳线 A）打舵机一直没反应。但这条路上一共有三个未知量缠在一起：
 *     ① ESP32 的 UART 收发本身
 *     ② H2 那两根线该直连还是交叉（微雪说 TX-TX，反直觉）
 *     ③ 板载缓冲器 U1 + 跳线 A 本身
 *   一次只测一个才能定位。这个固件专门解决 ①，**而且一根线都不用接**。
 *
 * [1] 内部回环 —— 芯片内部把 TX 接到 RX，完全不经过 GPIO 引脚
 *     ✅ 说明 UART 驱动和收发逻辑是好的 → 锅在"RX 没接到 GPIO16"这个路由上
 *     ❌ 说明 UART1 本身有问题       → 别用 16/17 了，直接换引脚
 *
 * [2] 手动 uart_set_pin 后再做外部回环（**这一组需要 GPIO16↔17 短接**）
 *     如果 [1] ✅ 而 [2] 也 ✅，但最初的普通回环 ❌ —— 那修法就是在主固件
 *     begin() 之后补一行 uart_set_pin()，问题当场解决
 *
 * 已经查清的事实：
 *   · GPIO16/17 当普通 IO 用完全正常（电平测试通过）→ 引脚没坏
 *   · TX 确实在驱动 GPIO17（裸 GPIO 采样看到翻转）→ 发送侧没坏
 *   · 但普通回环收不全，且波特率越低收得越多（9600→5字节, 1M→0字节）
 *     → 接收端**根本不是通过 UART 在收**，收到的那几个字节是串扰凑出来的假帧
 *   · pinMode(INPUT)/OUTPUT 折腾与成败无关
 *
 * 用法：
 *   什么都不用接，直接跑，串口 115200（每 5 秒一轮，随时开串口都能看）
 */

#include "driver/uart.h"

#define TX_PIN   17
#define RX_PIN   16

static const uint8_t PATTERN[] = {0xFF, 0xFF, 0x01, 0x04, 0x02, 0x38, 0x0F, 0x00};
static const size_t  PATLEN   = sizeof(PATTERN);
static uint8_t       RXBUF[32];

// 发一段固定数据，把收回来的东西填进 RXBUF，返回收到的字节数
static size_t exchange() {
  while (Serial1.available()) Serial1.read();

  Serial1.write(PATTERN, PATLEN);
  Serial1.flush();
  delay(60);

  size_t n = 0;
  while (Serial1.available() && n < sizeof(RXBUF)) RXBUF[n++] = Serial1.read();
  return n;
}

static void report(const char* tag, size_t n) {
  bool ok = (n == PATLEN) && (memcmp(RXBUF, PATTERN, PATLEN) == 0);
  Serial.printf("  %-26s 收 %u 字节 %s\n", tag, (unsigned)n,
                ok ? "✅ 完全一致" : "❌");
  if (n > 0 && !ok) {
    Serial.print("     收到: ");
    for (size_t i = 0; i < n && i < 16; i++) Serial.printf("%02X ", RXBUF[i]);
    Serial.println();
  }
}

// ---------------------------------------------------------------- [1]
// 芯片内部把 TX 接到 RX，完全不经过 GPIO 引脚。
// 这是唯一一个能把"UART 本身"和"引脚路由"分开的测试。
static void testInternalLoop(uint32_t baud) {
  Serial1.begin(baud, SERIAL_8N1, RX_PIN, TX_PIN);
  delay(30);
  // 注意：IDF 5.x 里叫 uart_set_loop_back（旧的 uart_set_loopback 已删）
  uart_set_loop_back(UART_NUM_1, true);     // ← 关键：硬件内部短接 TX→RX
  delay(5);

  size_t n = exchange();

  uart_set_loop_back(UART_NUM_1, false);
  delay(5);
  Serial1.end();
  delay(20);

  char tag[40];
  snprintf(tag, sizeof(tag), "[1] 内部回环 @ %u", baud);
  report(tag, n);
}

// ---------------------------------------------------------------- [2]
// begin() 之后再手动调一次 uart_set_pin() 强制重新路由。
// **这一组需要 GPIO16 和 GPIO17 用杜邦线短接**，不接的话必然 ❌。
static void testManualPin(uint32_t baud) {
  Serial1.begin(baud, SERIAL_8N1, RX_PIN, TX_PIN);
  delay(20);
  uart_set_pin(UART_NUM_1, TX_PIN, RX_PIN, -1, -1);   // 注意顺序：tx, rx
  delay(20);

  size_t n = exchange();

  Serial1.end();
  delay(20);

  char tag[40];
  snprintf(tag, sizeof(tag), "[2] 手动重路由 @ %u", baud);
  report(tag, n);
}

static void runTest() {
  static int round = 0;
  Serial.printf("\n============ 第 %d 轮 ============\n", ++round);

  Serial.println("--- [1] 内部回环（不需要任何接线）---");
  for (uint32_t baud : {9600u, 115200u, 1000000u}) testInternalLoop(baud);

  Serial.println("--- [2] 手动 uart_set_pin 后回环（需要 GPIO16↔17 短接）---");
  for (uint32_t baud : {9600u, 115200u, 1000000u}) testManualPin(baud);

  Serial.println("---------------------------------");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n【ESP32-S3 串口自检】");
  Serial.println("[1] 那三行不用接任何线，[2] 那三行需要把 16 和 17 短接");
}

void loop() {
  runTest();
  delay(5000);
}
