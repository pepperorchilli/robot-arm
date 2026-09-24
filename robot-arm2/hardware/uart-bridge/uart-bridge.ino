/*
 * 把 ESP32-S3 当成一根 USB 转串口线用
 *
 * 为什么需要它：
 *   走 H2（跳线 A）这条路，因为 A/B 互斥，USB 那条就断了 —— 唯一能到舵机的
 *   只有 H2。而 H2 这条路（杜邦线 + 板载缓冲器 U1 + 跳线 A 本身）我们从来没
 *   单独验证过是好的。
 *
 *   用 ESP32 自己写一套协议去打 H2，等于"拿没验证过的工具测没验证过的路"，
 *   两头都是黑的。这个固件把 ESP32 降级成一根透传线，Mac 上跑**已经验证过
 *   好用的 bench.py**，这样一次只引入一个未知量。
 *
 * 接线（跳线保持在 A）：
 *   ESP32 GPIO17 (TX) ──▶ H2 的 TX 或 RX   ← 命名规则反直觉，先按 TX→TX 试，
 *   ESP32 GPIO16 (RX) ──▶ H2 的 RX 或 TX      不通就把这两根对调（就一次）
 *   ESP32 GND        ──▶ H2 的 GND         ← 必须接，半双工没有共同参考电平必挂
 *
 * 注意：
 *   · 不要接 H2 以外的任何东西；也不要从总线上引 V+（12.6V 灌进 GPIO 会烧）
 *   · 微雪板的 USB-C 还是插着（跳线 A 已经把 CH343P 从总线断开了，不冲突），
 *     给板子逻辑供电
 *   · 这个固件不做电平方向控制 —— H2 那条路的方向是板载缓冲器 U1 自己管的，
 *     正好顺便验证 U1 是不是好的
 *
 * 用法（Mac 上）：
 *   python3 ../servo-tools/bench.py scan /dev/cu.usbmodem11301
 *   查端口：ls /dev/cu.usbmodem*
 *     11301         = ESP32-S3 自己
 *     ...49761      = 微雪板（CH343P）
 *
 * 结果怎么读：
 *   ✅ 扫到舵机  → H2 这条路 + 板载缓冲器 + ESP32 的 UART 全都是好的，
 *                  那问题就 100% 在主固件的半双工代码里
 *   ❌ 扫不到    → 先对调 TX/RX 两根线再试一次；还不行就是这条路本身的硬件问题
 */

#define SERVO_RX   16
#define SERVO_TX   17
#define SERVO_BAUD 1000000      // STS3215 出厂默认

void setup() {
  // USB CDC 那条（连 Mac），波特率是摆设，写什么都行
  Serial.begin(115200);
  delay(300);
  Serial.println("\n【UART 透传模式】ESP32 现在只是一根 USB 转串口线");
  Serial.printf("桥接：USB CDC  <->  GPIO%d(RX)/GPIO%d(TX) @ %d\n",
                SERVO_RX, SERVO_TX, SERVO_BAUD);

  // 真正对着舵机总线的那条
  Serial1.begin(SERVO_BAUD, SERIAL_8N1, SERVO_RX, SERVO_TX);
  delay(50);

  Serial.println("开始透传（后面不再打印任何东西，打印会污染 bench.py 的协议）");
}

void loop() {
  // Mac → 舵机
  while (Serial.available()) {
    Serial1.write((uint8_t)Serial.read());
  }
  // 舵机 → Mac
  while (Serial1.available()) {
    Serial.write((uint8_t)Serial1.read());
  }
  // 没有 delay —— 透传要尽量快，让 bench.py 的超时窗口够用
}
