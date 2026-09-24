/*
 * ESP32-S3 + STS3215 总线舵机 — 云连接版（合并固件）
 *
 * 这是 arm-cloud/（云连接）和 arm-bus-servo/（总线舵机）的合并版本：
 *   保留 arm-cloud 的 WiFi STA + WebSocket 客户端 + token 鉴权
 *   底层驱动从 PCA9685 换成 STS3215 串行总线
 *
 * ⚠️ 尚未上机验证。拿到舵机后请先按 arm-bus-servo/README.md 的
 *    「四步验证法」逐个确认，再烧本固件。
 *
 * ==================== 数据流 ====================
 *   浏览器 ──HTTP──> Node 服务器 ──WebSocket──> 本固件 ──TTL 总线──> 6 个舵机
 *
 * ==================== 和 PCA9685 版的关键区别 ====================
 *   PCA9685 版：软件在主循环里逐度推进（stepMotion），因为模拟舵机
 *               只能"给多少脉宽转多少角度"。
 *   总线舵机版：把「目标位置 + 到达时间」一起下发给舵机，
 *               舵机内部自己按时间平滑走位。**不需要软件插补**，
 *               一条 SYNC_WRITE 指令让 6 轴同时按各自速度运动。
 *
 * ==================== 本固件的自研部分（非 SO-ARM101 参考设计）====================
 *   1. 总线自检：上电 PING 每个 ID，缺哪个直接报出来
 *   2. 碰撞感知与柔顺回退：用舵机回传的实时负载判断撞到东西，
 *      就地退回上一个到位位置（而不是切扭矩 —— 断电会被重力砸下来）
 *   原版 SO-ARM101 / LeRobot 只做遥操作和模仿学习，没有力矩安全行为。
 *   这两项**不需要任何额外硬件**，负载数据舵机本来就在回传。
 *
 * ==================== 供电 ====================
 *   STS3215 有 7.4V 和 12V 两个版本，别买错：
 *     7.4V 版 19.5kg·cm —— 只够 Leader 臂（手拖的，不出力）
 *     12V  版 30kg·cm  —— 官方 Follower 臂用的这个，本机是 Follower
 *   选 12V 版有个额外好处：支持 6–12.6V 宽压，**3S 锂电可以直驱**，
 *   整条 12.6V→7.4V 降压链路（模块、接线、故障点）全部砍掉。
 *
 * ==================== 接线 ====================
 *   6 个舵机的 DATA 并联成一条总线；V+ / GND 分别并到电源 / GND
 *
 *   ESP32 GPIO17 (TX) ──[1kΩ]──→ 总线 DATA   ← 1kΩ 上拉，回读时才需要
 *   ESP32 GPIO16 (RX) ──────────→ 总线 DATA
 *   ESP32 GND         ──────────→ 舵机 GND（必须共地）
 *
 * ==================== 首次使用 ====================
 *   1. cp secrets.h.example secrets.h，填 WiFi 和服务器地址
 *   2. 确认 SERVO_ID[] 和实际改好的舵机 ID 一致（出厂全是 1，必须逐个改）
 *   3. 烧录
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <HardwareSerial.h>
#include "driver/uart.h"        // uart_set_pin：收完把 UART 挂回 TX 脚
#include "driver/gpio.h"        // esp_rom_gpio_connect_out_signal
#include "soc/gpio_sig_map.h"   // SIG_GPIO_OUT_IDX

#include "secrets.h"   // WiFi 账号密码、服务器地址、token（不进 Git）

// ==================== 配置 ====================
/*
 * 接微雪转接板 H2（跳线 A）时的接线规矩，**实测确认**：
 *
 *     ESP32 的 TX ──→ H2 的 TX        （直连，不交叉）
 *     ESP32 的 RX ──→ H2 的 RX
 *     ESP32 的 GND ─→ H2 的 GND       （必须接，半双工没有共地必挂）
 *
 * 板子上印的 TX/RX 是**从主控视角**标的，也就是"你的 TX 接这儿"，
 * 所以看着像接反了，其实是对的。微雪 wiki 说的"TX-TX、RX-RX"就是这个意思。
 *
 * ⚠️ 别和 ESP32-S3 开发板上印着 TX/RX 的那两个脚搞混 —— 那是
 *    GPIO43/44（UART0，跟 USB 日志同一路）。用那两个脚的话，
 *    固件在 16/17 上收发、线却接在 43/44 上，两边各说各话，
 *    现象是**死一样的安静**：自发自收的极性扫描一个字节都收不到。
 */
#define SERVO_TX   16          // 接 H2 的 TX
#define SERVO_RX   17          // 接 H2 的 RX
#define SERVO_BAUD 1000000     // STS3215 出厂默认 1Mbps

const int NUM = 6;   // SO-ARM101 有 6 个关节
// 舵机 ID，按物理顺序从下到上：底座 → 肩 → 肘 → 腕俯仰 → 腕旋转 → 夹爪
// ⚠️ 出厂全是 1，必须逐个改成 1~6，否则总线冲突
uint8_t SERVO_ID[NUM] = {1, 2, 3, 4, 5, 6};
const char* NAMES[NUM] = {"底座", "肩部", "肘部", "腕俯仰", "腕旋转", "夹爪"};

const uint16_t POS_CENTER = 2048;   // 0~4095 对应 0~360°，2048 是 180°（机械中位）
const int MOVE_TIME = 400;          // 到达目标的毫秒数（舵机内部平滑）

// ---- 碰撞感知（自研部分）----
// 首次上机调试建议先设 0 关掉：此时阈值还没标定，可能误触发。
// 标定方法见文件末尾「负载标定」注释。
#define ENABLE_COLLISION 1

#define LOAD_ERR        0xFFFF   // 读寄存器失败时的返回值
#define LOAD_MASK       0x03FF   // 负载只有低 10 位是数值，见下面 pollJoint 的说明

const uint16_t LOAD_DELTA = 250;  // 负载「突变」阈值，不是绝对阈值（需标定）
const uint16_t LOAD_FLOOR = 120;  // 低于此值视为空载抖动，不判碰撞
const uint8_t  SPIKE_NEED = 2;    // 连续 N 次超阈值才认定，防单次抖动误报

// ---- 保护阈值（自研部分）----
// 3S 锂电：满电 12.6V，放到 10.5V 就该收了，再低会过放损坏电芯。
// 整机（6 个舵机同时使劲）能拉到 10V 以下，所以「停止」定在 10.0V。
const uint8_t  TEMP_WARN = 65;      // °C，超过就报警
const uint8_t  TEMP_STOP = 75;      // °C，超过就松力（再高温会退磁）
// ⚠️ 台面联调开关 —— 现在台面用的是 C001（经 UBEC 供 5.1V），不是 12.6V 锂电。
//    按 105 判的话，开机几秒后低压必然锁存 → 松力 → 滑块拖动舵机毫无反应，
//    现象非常像"网页控制坏了"。练动作时先降到 45。
//    换回 3S 锂电之前**必须**改回 105，否则低压保护形同虚设。
const uint16_t VOLT_WARN = 45;      // ← 台面临时值（对应 4.5V）；装车改回 105
const uint16_t VOLT_STOP = 100;     // 10.0V

// ==================== STS/SCS 协议寄存器 ====================
#define REG_TORQUE_ENABLE  0x28
#define REG_GOAL_POSITION  0x2A
#define REG_GOAL_TIME      0x2C

// 只读区。刻意连成一片，好一次读完（见 pollJoint）。
// 0x38 ─┬─ 位置   ×2
//       ├─ 速度   ×2
//       ├─ 负载   ×2
//       ├─ 电压   ×1   （单位 0.1V）
//       ├─ 温度   ×1   （单位 1°C）
//       ├─ 异步写 ×1   （用不到，顺路读掉）
//       ├─ 状态   ×1   （用不到）
//       ├─ 运动中 ×1
//       ├─ 保留   ×2   ← 中间确实空着两个字节，别以为是漏写
// 0x45 ─┴─ 电流   ×2   （单位 6.5mA）
#define REG_PRESENT_POS    0x38
#define REG_TELEM_LEN      15     // 0x38 ~ 0x46，正好覆盖到电流

// 上面那片只读区在回包参数里的字节下标（参数从 buf[5] 开始）
#define TELEM_POS     0
#define TELEM_SPEED   2
#define TELEM_LOAD    4
#define TELEM_VOLT    6
#define TELEM_TEMP    7
#define TELEM_MOVING  10
#define TELEM_CURRENT 13

#define INST_PING        0x01
#define INST_READ        0x02
#define INST_WRITE       0x03
#define INST_SYNC_WRITE  0x83
#define BROADCAST_ID     0xFE

HardwareSerial servoBus(1);
WebSocketsClient ws;

// ==================== 状态 ====================
// ⚠️ 这里必须正好 6 个初值。少写一个的话，缺的那个元素默认 0，
//    上电「全部回中」时那个关节会直接甩到 0°（夹爪会被带到底）。
int target[NUM]     = {90, 90, 90, 90, 90, 90};  // 目标角度（网页的 0~180）
int prevTarget[NUM] = {90, 90, 90, 90, 90, 90};  // 上一个「确实到位」的角度，碰撞时退回这里

// 碰撞感知状态
uint16_t lastLoad[NUM];    // 上次采到的负载大小
uint8_t  spikeCnt[NUM];    // 连续突变计数
uint8_t  pollIdx = 0;      // 轮询指针：一次 loop 只读一个关节

// 保护状态（锁存：触发后要等条件真正恢复才解除，不然会反复触发）
bool     torqueOn      = false;
bool     overtempLatch = false;
bool     tempWarned    = false;
bool     lowbatLatch   = false;
uint8_t  lowVoltCnt    = 0;    // 连续低压计数，滤掉负载造成的瞬时压降

// 到位回报：发完指令后等 MOVE_TIME 再回报，避免高频刷屏
bool reportPending = false;
unsigned long reportAt = 0;

// ==================== 协议底层 ====================
/*
 * 包结构（Feetech SCS / Dynamixel 1.0 兼容）：
 *   [0] 0xFF  帧头
 *   [1] 0xFF  帧头
 *   [2] ID
 *   [3] LEN = 参数个数 + 2
 *   [4] 指令
 *   [5..] 参数
 *   [末] 校验和 = ~(ID + LEN + 指令 + 全部参数) 取低 8 位
 */
void sendPacket(uint8_t* p) {
  uint8_t total = 6 + (p[3] - 2);
  uint8_t sum = 0;
  for (uint8_t i = 2; i < total - 1; i++) sum += p[i];
  p[total - 1] = ~sum;
  servoBus.write(p, total);
  servoBus.flush();
}

// ⚠️ 数组长度要算上 sendPacket 写在校验和位置的**最后那个字节**：
//    total = 6 + (LEN - 2)，下标 0 ~ total-1
//    writeByte: LEN=4 → total=8 → 要 p[8]（以前是 p[7]，越界一字节，
//               正好盖住栈保护金丝雀 → "Stack smashing protect failure"）
//    writeWord: LEN=5 → total=9 → 要 p[9]（同上）
void writeByte(uint8_t id, uint8_t reg, uint8_t v) {
  uint8_t p[8] = {0xFF, 0xFF, id, 4, INST_WRITE, reg, v, 0};
  sendPacket(p);
}

void writeWord(uint8_t id, uint8_t reg, uint16_t v) {
  uint8_t p[9] = {0xFF, 0xFF, id, 5, INST_WRITE, reg,
                  (uint8_t)(v & 0xFF), (uint8_t)(v >> 8), 0};
  sendPacket(p);
}

/*
 * SYNC_WRITE：一条指令让所有舵机同时收到各自的目标位置。
 * 这是总线舵机相比 PCA9685 的优势之一 —— 5 轴严格同步启动，
 * 不会因为逐个发送而产生几毫秒的时序错位。
 */
void syncWritePosition(const uint16_t* pos, uint8_t num) {
  const uint8_t dataLen = 2;
  uint8_t params = 2 + (1 + dataLen) * num;
  uint8_t total = 6 + params;

  uint8_t p[40];
  if (total > sizeof(p)) return;

  p[0] = 0xFF; p[1] = 0xFF;
  p[2] = BROADCAST_ID;
  p[3] = params + 2;
  p[4] = INST_SYNC_WRITE;
  p[5] = REG_GOAL_POSITION;
  p[6] = dataLen;

  uint8_t k = 7;
  for (uint8_t i = 0; i < num; i++) {
    p[k++] = SERVO_ID[i];
    p[k++] = pos[i] & 0xFF;
    p[k++] = (pos[i] >> 8) & 0xFF;
  }
  sendPacket(p);
}

// ==================== 角度换算 ====================
// 网页的 0~180° 映射到舵机量程的一半（0~2048），另一半预留给需要更大行程的关节
uint16_t angleToPos(int a) {
  a = constrain(a, 0, 180);
  return (uint16_t)map(a, 0, 180, 0, POS_CENTER);
}

// 把当前所有目标角度一次性同步下发
void applyTargets() {
  uint16_t pos[NUM];
  for (int i = 0; i < NUM; i++) pos[i] = angleToPos(target[i]);
  syncWritePosition(pos, NUM);

  reportPending = true;
  reportAt = millis() + MOVE_TIME + 50;
}

/*
 * 开/关所有舵机的扭矩（＝松不松力）。
 *
 * 保护动作会调它。为什么不干脆断电：断电后手臂被重力拽下来会自由落体，
 * 松力则是「软下来」，舵机本身还有阻尼，落得慢得多。
 */
void setTorque(bool on) {
  if (torqueOn == on) return;      // 别重复下发，一帧 6 条包不便宜
  for (int i = 0; i < NUM; i++) {
    writeByte(SERVO_ID[i], REG_TORQUE_ENABLE, on ? 1 : 0);
  }
  torqueOn = on;
  // 告诉网页：现在是不是松力状态。网页靠它把控制条变灰并提示
  // 「已松力保护，处理完按恢复」—— 不然用户会以为机械臂坏了。
  ws.sendTXT("E:torque,," + String(on ? 1 : 0));
  Serial.printf("扭矩 %s\n", on ? "已开启" : "已关闭（松力）");
}

// ==================== 半双工方向控制 ====================
/*
 * 总线是半双工的：ESP32 发完必须**真正松手**，舵机才能驱动同一条线回话。
 *
 * ⚠️ 光 pinMode(SERVO_TX, INPUT) 摘不掉 UART —— 实测确认过。
 *    ESP32 的 GPIO 矩阵会把引脚一直钉在 UART 的 TX 输出上；pinMode 只改
 *    GPIO 自己的方向寄存器，矩阵那条输出通路还在推。所以空闲高电平会跟
 *    舵机回话的前几位撞车，把回包开头那几个字节顶坏，症状非常好认：
 *
 *        收到的：FE FE 00 FE | FF FF 01 02 00 FC
 *                 ^^^^^^^^^^^ 被顶坏的    ^^^^^^^^^^^^^^^^^ 真正的应答
 *
 *    而 readTelemetry() 只认 buf[0]==0xFF，于是整包被丢掉，
 *    表现为「自检永远停在 1/6、负载全是 --」。
 *
 *    正确做法：先用 gpio 矩阵把 UART 的 TX 信号从这根脚上**摘下来**，
 *    再切成输入；收完用 uart_set_pin() 挂回去。
 *    （以前这里只有 pinMode，注释里那句"pinMode 未必真能释放"猜对了。）
 */
static inline void busRelease() {
  esp_rom_gpio_connect_out_signal(SERVO_TX, SIG_GPIO_OUT_IDX, false, false);
  pinMode(SERVO_TX, INPUT);
}

static inline void busAcquire() {
  pinMode(SERVO_TX, OUTPUT);
  uart_set_pin(UART_NUM_1, SERVO_TX, SERVO_RX, -1, -1);
}

// ==================== 回读（力反馈用）====================
/*
 * 一次读一整片（15 字节），不是一格格读。
 * 为什么：每读一次要经历「发包 → 等回包 → 超时」，6 个关节分开读
 * 就是 6 倍的等待时间。而 loop() 里这段是阻塞的，等太久 WebSocket
 * 心跳会断（表现为"控制到一半掉线"）。合成一次读，代价降到 1/6。
 */
bool readTelemetry(uint8_t id, uint8_t* out, uint8_t len) {
  // 请求包：LEN=4 表示「指令 1 字节 + 参数 2 字节 + 校验 1 字节」
  uint8_t p[8] = {0xFF, 0xFF, id, 4, INST_READ, REG_PRESENT_POS, len, 0};
  sendPacket(p);

  busRelease();                    // 松手，等舵机回话

  // 回包长度 = 帧头2 + ID1 + LEN1 + 参数 len + 校验1
  uint8_t total = len + 5;
  uint8_t buf[24] = {0};
  uint8_t got = 0;
  uint32_t t0 = millis();
  while (got < total && millis() - t0 < 25) {
    if (servoBus.available()) buf[got++] = servoBus.read();
  }
  while (servoBus.available()) servoBus.read();   // 丢掉多余字节，别留到下一轮

  busAcquire();

  // 收全了才认。半路截断的数据比没数据更危险 —— 会解出乱七八糟的负载值。
  if (got < total || buf[0] != 0xFF || buf[1] != 0xFF || buf[2] != id) return false;

  // 回包里 [4] 是错误码（0 正常），参数从 [5] 开始
  if (buf[4] != 0) return false;
  memcpy(out, buf + 5, len);
  return true;
}

// ==================== 总线自检（自研部分）====================
/*
 * PING 一个 ID，收到回包说明舵机在线。
 *
 * 为什么需要：出厂舵机**全是 ID=1**，如果没逐个改，总线上会撞号，
 * 表现是「上电后什么都不动」——没有任何报错，只能靠猜。
 * 自检把缺的 ID 直接打出来，这类问题一眼就能定位。
 *
 * PING 的回包是最短的：0xFF 0xFF ID LEN ERR CHECKSUM 共 6 字节。
 */
bool pingServo(uint8_t id) {
  uint8_t p[6] = {0xFF, 0xFF, id, 2, INST_PING, 0};
  sendPacket(p);

  busRelease();                    // 松手，等回包
  uint8_t buf[6] = {0};
  uint8_t got = 0;
  uint32_t t0 = millis();
  while (got < 6 && millis() - t0 < 10) {
    if (servoBus.available()) buf[got++] = servoBus.read();
  }
  while (servoBus.available()) servoBus.read();
  busAcquire();

  return buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == id;
}

// ==================== 遥测 + 碰撞 + 保护（自研部分）====================
/*
 * 一次 loop 只处理一个关节（轮询）。为什么不一口气读完 6 个：
 * readTelemetry 是阻塞的（要等回包），读 6 次累计能到几十毫秒，
 * 期间 ws.loop() 得不到调用，心跳就断了 —— 表现是"控制到一半掉线"。
 * 摊到 6 轮之后，每轮只阻塞几毫秒，心跳完全来得及。
 *
 * 服务器那边按关节号填格子，所以顺序、节奏都不敏感，
 * 掉一两轮也只是某个关节的数据晚一点刷新。
 */
void pollJoint() {
  uint8_t i = pollIdx;
  pollIdx = (pollIdx + 1) % NUM;

  uint8_t t[REG_TELEM_LEN];
  if (!readTelemetry(SERVO_ID[i], t, REG_TELEM_LEN)) {
    // 读失败就跳过这轮。不要拿旧值硬凑 —— 会解出假的负载突变。
    spikeCnt[i] = 0;
    return;
  }

  uint16_t pos     = t[TELEM_POS]     | (t[TELEM_POS + 1] << 8);
  uint16_t rawLoad = t[TELEM_LOAD]    | (t[TELEM_LOAD + 1] << 8);
  uint8_t  volt    = t[TELEM_VOLT];
  uint8_t  temp    = t[TELEM_TEMP];
  uint16_t current = t[TELEM_CURRENT] | (t[TELEM_CURRENT + 1] << 8);

  // 负载寄存器：低 10 位是大小（1000 = 额定扭矩的 100%），
  // **bit 10（0x400）才是方向位**。不是最高位 —— 最高位在这颗舵机上
  // 恒为 0，按最高位判方向会永远判成"逆时针"。
  uint16_t mag = rawLoad & LOAD_MASK;

  // ---- 上报遥测：原始值，不做换算 ----
  // 电压 ×0.1V、电流 ×6.5mA 这些单位换算全放在服务器做。
  // 固件只管把寄存器原样丢上去 —— 以后改显示、加图表，
  // 不用重新烧录一块拆下来很麻烦的板子。
  ws.sendTXT("T:" + String(i) + "," + String(pos) + "," + String(rawLoad) + ","
             + String(volt) + "," + String(temp) + "," + String(current));

  // ---- 保护：过温 / 低压 ----
  // 这两个是「慢变量」，不像碰撞那样几十毫秒跳变，
  // 所以顺路判掉，不用另开定时器。
  checkProtection(i, temp, volt);

  // ---- 碰撞判定 ----
  // 用 #if 而不是 if：关掉时这段彻底不编译，省下 flash 也避免
  // 一个"看起来在跑其实被短路"的假象。
#if ENABLE_COLLISION
  // 判据用「负载突变」而不是「负载超过某个值」——
  // 肩部关节静止托着整条手臂时本来就有持续负载（重力），
  // 用绝对阈值会在正常姿态下一直误报。撞到东西的特征是
  // 负载在几十毫秒内**跳变**。所以要的是 delta，不是绝对值。
  uint16_t delta = (mag > lastLoad[i]) ? (mag - lastLoad[i])
                                       : (lastLoad[i] - mag);
  lastLoad[i] = mag;

  if (delta < LOAD_DELTA || mag < LOAD_FLOOR) { spikeCnt[i] = 0; return; }
  if (++spikeCnt[i] < SPIKE_NEED) return;

  // ---- 认定碰撞：退回上一个到位位置 ----
  // 为什么不直接切扭矩：断电后手臂会被重力砸下来，比硬顶一下更危险。
  // 回退是「朝离开障碍物的方向」走，所以相对安全。
  spikeCnt[i] = 0;
  target[i] = prevTarget[i];
  applyTargets();

  Serial.printf("💥 %s 碰撞（负载 %u，突变 %u）→ 回退到 %d°\n",
                NAMES[i], mag, delta, target[i]);
  ws.sendTXT("E:collision," + String(i) + "," + String(mag));
#else
  lastLoad[i] = mag;   // 关掉碰撞也要跟住负载，不然重新打开时第一次必然误判
#endif
}

/*
 * 过温 / 低压保护。
 *
 * 为什么要做：舵机自己不会喊疼。堵转半个小时它只会烫到退磁，
 * 3S 锂电放到 9V 也只会安静地报废。这两个值都是"看一眼就知道"的，
 * 但没做保护就没人看。
 *
 * 过温只松力不锁死：锁死等于让它保持堵转，反而烧得更快。
 */
void checkProtection(uint8_t i, uint8_t temp, uint8_t volt) {
  if (temp >= TEMP_STOP && !overtempLatch) {
    overtempLatch = true;
    Serial.printf("🔥 %s 过温 %u°C（≥%u）→ 松力\n", NAMES[i], temp, TEMP_STOP);
    ws.sendTXT("E:overtemp," + String(i) + "," + String(temp));
    setTorque(false);
  } else if (temp >= TEMP_WARN && !tempWarned) {
    tempWarned = true;
    Serial.printf("🌡️  %s 温度偏高 %u°C\n", NAMES[i], temp);
    ws.sendTXT("E:overtemp_warn," + String(i) + "," + String(temp));
  } else if (temp < TEMP_WARN - 5) {
    // 带 5°C 迟滞：不然在阈值上下抖动会刷屏
    tempWarned = false;
    if (overtempLatch && temp < TEMP_WARN) {
      overtempLatch = false;
      setTorque(true);
      Serial.println("❄️  温度回落，恢复扭矩");
    }
  }

  // 电压：6 个舵机同时使劲时会被拉低，所以只看持续低压。
  // 单次低于阈值不报 —— 那是负载造成的正常压降，不是电池没电了。
  if (volt > 0 && volt < VOLT_WARN) {
    if (++lowVoltCnt >= 10 && !lowbatLatch) {   // 连续 10 轮（约几秒）
      lowbatLatch = true;
      Serial.printf("🪫 电压低 %u.%uV → 停手，请充电\n", volt / 10, volt % 10);
      ws.sendTXT("E:lowbat,," + String(volt));
      setTorque(false);
    }
  } else {
    lowVoltCnt = 0;
    if (lowbatLatch && volt >= VOLT_WARN) lowbatLatch = false;   // 换了电池就复位
  }
}

// ==================== WebSocket ====================
void onMessage(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("✅ 已连上服务器");
      break;

    case WStype_DISCONNECTED:
      Serial.println("❌ 与服务器断开，等待重连…");
      break;

    case WStype_TEXT: {
      String msg = String((char*)payload);
      msg.trim();
      Serial.println("收到命令: " + msg);

      // 命令格式: "舵机编号:角度"  例如 "0:45"
      int colon = msg.indexOf(':');
      if (colon > 0) {
        int idx = constrain(msg.substring(0, colon).toInt(), 0, NUM - 1);
        int tgt = constrain(msg.substring(colon + 1).toInt(), 0, 180);
        target[idx] = tgt;
        applyTargets();     // 立即下发，舵机自己平滑走位
        Serial.printf("  %s → %d°\n", NAMES[idx], tgt);
      }
      break;
    }

    default:
      break;
  }
}

// ==================== 入口 ====================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n================================");
  Serial.println(" ESP32-S3 机械臂 · 云连接 + 总线舵机");
  Serial.println("================================");

  // ---- 总线舵机初始化 ----
  servoBus.begin(SERVO_BAUD, SERIAL_8N1, SERVO_RX, SERVO_TX);

  // ---- 总线自检：先确认谁在线，再开扭矩 ----
  // 放在开扭矩之前：如果 ID 撞号，宁可不动作，也不要让撞号的舵机乱转。
  Serial.print("总线自检: ");
  int online = 0;
  for (int i = 0; i < NUM; i++) {
    bool ok = pingServo(SERVO_ID[i]);
    Serial.printf("%d%s ", SERVO_ID[i], ok ? "✓" : "✗");
    if (ok) online++;
  }
  Serial.printf(" (%d/%d)\n", online, NUM);
  if (online < NUM) {
    Serial.println("⚠️ 有舵机没回应，常见原因：");
    Serial.println("   1) ID 没改 —— 出厂全是 1，会撞号，只能有一个应答");
    Serial.println("   2) 没共地 / 舵机没供电（ESP32 单独供电时最容易漏）");
    Serial.println("   3) DATA 接错 —— ESP32 TX 应经 1kΩ 上拉到总线 DATA");
  }

  for (int i = 0; i < NUM; i++) {
    writeWord(SERVO_ID[i], REG_GOAL_TIME, MOVE_TIME); // 设定到达时间
  }
  // ⚠️ 必须走 setTorque，不能直接写 REG_TORQUE_ENABLE。
  //    torqueOn 初值是 false，绕过 setTorque 的话这个变量就还是 false，
  //    而舵机实际已经上力了 —— 之后保护动作调 setTorque(false) 会因为
  //    "值没变" 被提前 return，**过温保护就成了哑炮**。
  setTorque(true);   // 开扭矩
  delay(100);
  applyTargets();   // 全部回中
  Serial.println("总线舵机初始化完成（全部回中 90°）");

  // ---- 连 WiFi（STA 模式）----
  Serial.printf("连接 WiFi: %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAt < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n❌ WiFi 连接失败，检查 secrets.h");
    Serial.println("   （ESP32 只支持 2.4GHz，连不上 5GHz）");
    return;
  }
  Serial.println("\n✅ WiFi 已连接");
  Serial.print("   本机 IP: ");
  Serial.println(WiFi.localIP());

  // ---- 连服务器（token 放在连接地址里，服务器会校验）----
  // ⚠️ 路径必须是 /ws，不能是 /
  //    因为前面挂了 Nginx：/ 要留给静态首页，WebSocket 走独立路径 /ws。
  //    Nginx 无法按请求头区分 location，所以靠路径来分流。
  //    （服务器只校验 token 不校验路径，所以改路径不需要改后端）
  String path = "/ws?token=" + String(ESP32_TOKEN);
  ws.begin(SERVER_HOST, SERVER_PORT, path.c_str());
  ws.onEvent(onMessage);
  ws.setReconnectInterval(5000);
  ws.enableHeartbeat(15000, 3000, 2);

  Serial.printf("正在连接服务器 %s:%d …\n", SERVER_HOST, SERVER_PORT);
}

/*
 * ==================== 负载标定 ====================
 * LOAD_DELTA / LOAD_FLOOR 是估的，必须实测标定，否则会误触发。
 *
 *   1. 串口发 L → 打印 6 个关节当前负载
 *   2. 让机械臂**空载慢速走完全程**，记下每个关节负载的最大波动
 *      → LOAD_FLOOR 取这个波动的 1.5 倍左右
 *   3. 让机械臂自由运动中**用手轻挡一下**，记下负载跳变量
 *      → LOAD_DELTA 取「正常波动」和「阻挡跳变」之间，偏大一点更稳
 *   4. 都填回文件顶部，重新烧录
 *
 * 标定期间把 ENABLE_COLLISION 设 0，避免误触发干扰观察。
 */
void handleSerial() {
  if (!Serial.available()) return;
  char c = Serial.read();

  // 'S' —— 连接状态。网页上"拖滑块没反应"时，先用它判断卡在哪一段：
  //       WiFi 没连 / 服务器没连 / 连上了但扭矩被保护松掉了。
  if (c == 'S' || c == 's') {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("WiFi:    ✅ 已连接  IP=%s  RSSI=%d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
      // 连不上先看这里配的 SSID 对不对（ESP32 只支持 2.4GHz，别配 5GHz）
      Serial.printf("WiFi:    ❌ 未连接 (status=%d)  固件配的 SSID: \"%s\"\n",
                    (int)WiFi.status(), WIFI_SSID);
    }
    Serial.printf("服务器:  %s:%d  WebSocket: %s\n",
                  SERVER_HOST, SERVER_PORT, ws.isConnected() ? "✅ 已连接" : "❌ 未连接");
    Serial.printf("扭矩:    %s   低压锁存: %s   过温锁存: %s\n",
                  torqueOn ? "开" : "关",
                  lowbatLatch ? "是（会松力）" : "否",
                  overtempLatch ? "是" : "否");

    // 台面模式自报。
    // VOLT_WARN 被临时降到台面电压（C001 走 UBEC 只有 5.1V），低压保护基本等于关掉了。
    // 写在 S 命令里而不是只写注释，是因为**注释没人会回头看**，而这条命令每次查状态都会跑：
    // 忘了改回去就直接接 12.6V 电池 → 保护不动作 → 电芯过放损坏。
    if (VOLT_WARN < 100) {
      Serial.printf("⚠️  台面模式: VOLT_WARN=%u（即 %.1fV），低压保护已降低。\n",
                    (unsigned)VOLT_WARN, VOLT_WARN / 10.0);
      Serial.println("    接 12.6V 3S 锂电之前必须改回 105！");
    }
    return;
  }

  // 'W' —— 扫一遍周围的 WiFi，连不上时用它确认：
  //       SSID 在不在、信号多强、加密方式。ESP32 只支持 2.4GHz，
  //       5GHz 的热点这里**根本扫不到** —— 一个都没有就是这个问题。
  if (c == 'W' || c == 'w') {
    // ⚠️ 必须先把射频停干净再扫。WiFi.begin() 之后后台一直有重连握手在跑，
    //    这种情况下 scanNetworks() 会直接返回 -2（WIFI_SCAN_FAILED），
    //    看起来像"扫描坏了"，其实只是被占着。
    Serial.printf("扫描前 status=%d\n", (int)WiFi.status());
    WiFi.disconnect(true, true);
    delay(300);
    WiFi.mode(WIFI_OFF);
    delay(300);
    WiFi.mode(WIFI_STA);
    delay(300);

    Serial.println("扫描中（几秒）...");
    int n = WiFi.scanNetworks(false, true);
    Serial.printf("scanNetworks() = %d，扫到 %d 个网络（★ = 固件配的那个）：\n", n, n);
    for (int i = 0; i < n; i++) {
      bool mine = (WiFi.SSID(i) == String(WIFI_SSID));
      Serial.printf("  %s%-22s %4d dBm  信道%2d  %s\n",
                    mine ? "★" : " ",
                    WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i), WiFi.channel(i),
                    WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "开放" : "加密");
    }
    if (n == 0) Serial.println("  （一个都没扫到）");
    WiFi.scanDelete();
    return;
  }

  if (c != 'L' && c != 'l') return;

  Serial.print("负载: ");
  for (int i = 0; i < NUM; i++) {
    uint8_t t[REG_TELEM_LEN];
    if (!readTelemetry(SERVO_ID[i], t, REG_TELEM_LEN)) {
      Serial.printf("%s=-- ", NAMES[i]);      // -- = 没读到
      continue;
    }
    uint16_t raw = t[TELEM_LOAD] | (t[TELEM_LOAD + 1] << 8);
    Serial.printf("%s=%u ", NAMES[i], raw & LOAD_MASK);
  }
  Serial.printf(" (阈值 delta=%u floor=%u)\n", LOAD_DELTA, LOAD_FLOOR);
}

void loop() {
  ws.loop();     // 必须高频调用，否则收不到消息、心跳也会断

  handleSerial();

  // 一次 loop 轮询一个关节：既上报遥测，也顺带判碰撞和保护。
  // 关掉碰撞时也要跑 —— 遥测和过温/低压保护是另一回事，
  // 不能因为碰撞阈值没标定好就把力反馈一起关了。
  pollJoint();

  // 到位回报
  if (reportPending && millis() >= reportAt) {
    reportPending = false;

    // 到这里说明 MOVE_TIME 已过，各关节**确实到位**了。
    // 此刻才把 target 记进 prevTarget —— 碰撞回退要退回
    // 「真实到达过的位置」，而不是「发出过但可能没走到的指令」。
    for (int i = 0; i < NUM; i++) prevTarget[i] = target[i];

    String report = "";
    for (int i = 0; i < NUM; i++) {
      report += String(i) + ":" + String(target[i]);
      if (i < NUM - 1) report += ",";
    }
    ws.sendTXT(report);
    Serial.println("到位回报: " + report);
  }
}
