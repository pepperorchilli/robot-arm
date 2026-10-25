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
 *   STS3215 有 C001（7.4V）和 C047（4~14V）两个版本，别买错：
 *     C001  19.5kg·cm —— 只够 Leader 臂（手拖的，不出力）
 *     C047  30kg·cm   —— 官方 Follower 臂用的这个，本机是 Follower
 *   选 C047 有个额外好处：支持 4~14V 宽压，**3S 锂电 12.6V 可以直驱**，
 *   整条降压链路（模块、接线、故障点）全部砍掉。
 *   ⚠️ 两个版本外观几乎一样；**C001 接 12.6V 会烧**。
 *
 * ==================== 接线 ====================
 *   6 个舵机的 DATA 并联成一条总线；V+ / GND 分别并到电源 / GND
 *
 *   ESP32 GPIO16 (TX) ──────────→ 微雪板 H2 的 TX   ← 直连，不交叉
 *   ESP32 GPIO17 (RX) ──────────→ 微雪板 H2 的 RX
 *   ESP32 GND         ──────────→ 舵机 GND（必须共地）
 *
 *   ⚠️ 完整接线规矩见下方「配置」一节里标注「实测确认」的那份，以它为准。
 *      这里早期写的是「GPIO17 当 TX + 1kΩ 上拉」，两处都是错的：
 *      引脚以 SERVO_TX 16 / SERVO_RX 17 为准，回读也不需要电阻
 *      （真因是半双工没释放，已在软件里解决）。
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

/*
 * ⚠️ 装配校准：每个关节实测的机械行程（raw 位置，0~4095）
 *
 * 为什么需要这组数：官方装配流程要求「先把舵机转到中位，再装舵盘」。
 * 这台机器是在舵盘任意角度下装好的，所以固件里那个绝对角度
 * （90° → raw 1024）和实际机械姿态根本对不上 —— 上电「回中」会让
 * 部分关节直接甩 150~240°，一头撞死在限位上。
 *
 * 解决分两步：
 *   1) 硬件侧：改写每颗舵机的 OFS（位置偏移，寄存器 0x1F，存 EEPROM），
 *      把各自的机械行程挪到 raw 2048 两侧。这样**任何目标都不会跨过
 *      0/4095 边界** —— 实测这颗舵机的目标位置是单向单调的，
 *      一旦跨边界它不会走近路，而是朝反方向绕一大圈。
 *   2) 固件侧：滑块 0~180 线性映射到各自的 [RAW_MIN, RAW_MAX]。
 *
 * 数据来源：松力状态下用手把每个关节推到两端，记录 Present Position。
 * 校准过程见 hardware/servo-tools/。两端各向内收 80 个计数（约 7°）
 * 作为余量，不贴着机械限位走。
 *
 * 顺序：底座 → 肩 → 肘 → 腕俯仰 → 腕旋转 → 夹爪
 *
 * ⚠️ 方向：滑块 0 一律对准「机械臂收拢」的那一端 —— 拖到 0 就是六个关节
 *   抱在一起，往右拖才一点点展开。所以这张表里存的不一定是「小的那个数」：
 *   肘部折叠时的位置在 raw **大**的一端（实测 raw≈3092），
 *   它的上下限就得**倒着写**（MIN > MAX），滑块往右 = raw 变小 = 展开。
 *   angleToPos / posToAngle 都按「MIN 可以是大的」来处理，别假设 MIN < MAX。
 *   底座和腕旋转没有天然的折叠方向（转到哪个角度都不影响收拢），
 *   就按普通递增写，收拢姿态下它们大约落在 90°。
 */
// 夹爪（第 6 个）闭合端**不要余量**：它的机械极限 1285 就是"两片手指碰上"，
// 按常规再收 80 个计数就永远差约 7° 合不拢，肉眼能看出来。
// 所以这里直接给到 1285。代价是闭合时舵机会带一点静载，
// 用串口 L 命令看负载，超过 ~200 说明其实在顶死，再往上收 20。
const uint16_t RAW_MIN[NUM] = { 832, 1477, 3083, 1041, 1473, 1285 };
// 腕俯仰（第 4 个）上限特意收到 3000 而不是 3055：实测命令 3055 时它只到 3028，
// 末段被腕部自重压着上不去。留在这里会让舵机一直顶着劲发热，收一点就到位了。
// 肘部（第 3 个）上限从 1013 收到 1045：全行程实测命令 180°（raw 1013）时
// 它只能到 raw 1037，差 24 个计数，等于每个 180° 姿态都在硬顶限位（负载 144）。
const uint16_t RAW_MAX[NUM] = {3265, 2618, 1045, 3000, 2623, 2731 };

// 到达目标的毫秒数（舵机内部平滑）。
// **运行时可变**：串口按 1/2/3 切换快/中/慢，立即写入所有舵机的 GOAL_TIME，
// 不用重新烧录。录演示视频时切「慢」，日常用「快」。
int MOVE_TIME = 400;

// ---- 碰撞感知（自研部分）----
// 首次上机调试建议先设 0 关掉：此时阈值还没标定，可能误触发。
// 标定方法见文件末尾「负载标定」注释。
#define ENABLE_COLLISION 1

#define LOAD_ERR        0xFFFF   // 读寄存器失败时的返回值
#define LOAD_MASK       0x03FF   // 负载只有低 10 位是数值，见下面 pollJoint 的说明

uint16_t LOAD_DELTA = 250;  // 负载「突变」阈值，不是绝对阈值（需标定，串口 d<数字> 可现场改）
const uint16_t LOAD_FLOOR = 120;  // 低于此值视为空载抖动，不判碰撞
const uint8_t  SPIKE_NEED = 2;    // 连续 N 次超阈值才认定，防单次抖动误报

// ---- 保护阈值（自研部分）----
// 3S 锂电：满电 12.6V，放到 10.5V 就该收了，再低会过放损坏电芯。
// 整机（6 个舵机同时使劲）能拉到 10V 以下，所以「停止」定在 10.0V。
const uint8_t  TEMP_WARN = 65;      // °C，超过就报警
const uint8_t  TEMP_STOP = 75;      // °C，超过就松力（再高温会退磁）
// 连续 N 次读到高温才认。总线偶发坏读数（尤其上电第一次读 15 字节块）
// 会解出七八十度的假温度，只凭一次就松力，手臂会毫无征兆地软一下 ——
// 实测踩到过：底座报 87°C，3 毫秒后肩部报 34°C，力被松开又立刻恢复。
const uint8_t  TEMP_CONFIRM = 3;
// ✅ 已装机：现在是 6 颗 C047 由 3S 锂电 12.6V 直驱，所以用正式阈值 105。
//    台面联调那个临时值 45（4.5V）是给 C001 走 UBEC 5.1V 用的，
//    留在车上的话低压保护形同虚设，会把锂电过放到损坏。
//    以后要是再拿台面 C001 单测，临时把它改回 45，并且**记得改回来**。
const uint16_t VOLT_WARN = 105;     // 10.5V：3S 锂电放到这里就该收了
const uint16_t VOLT_STOP = 100;     // 10.0V：整机同时使劲会拉到 10V 以下，再低直接停

// ---- 关节过载保护（每个关节单独计时）----
// 碰撞看的是「负载突变」，抓不到「一直顶着 90% 不动」——
// 而那恰恰最烤舵机：位置舵机在误差恒定时输出恒定力矩、恒定电流，
// 温度会一直往上爬，最后退磁。所以这里按**持续时间**分级。
// ⚠️ 这几个数是实测出来的，不是估的（2026-10 全行程 + 手挡堵转标定）：
//     自由空间全行程运动，六个关节峰值负载 ≤ 160
//     手掌压住前臂顶住不动，肘部负载直接到 1000（满量程），raw 完全不动
//   负载寄存器的值正比于位置误差，所以"硬顶自己机械限位"反而不高
//   （只差 24 个计数，实测才 144）——只有被**外部**卡住、误差消不掉才会满量程。
//   中间那段留白就是"正常但吃力"：搬东西、横伸托举大概落在 200~500。
//
// ⚠️ 下面四个**运行时可变**（不再加 const）：串口 c / k / m 三个命令可以现场改，
//    不用重烧。改完的代价是可能忘了改回去 —— 所以 S 命令会把当前值打出来，
//    每次查连接状态都提醒一遍。
uint16_t LOAD_WARN    = 600;   // 60%：持续 10 秒只报警，不做动作
uint16_t LOAD_CRIT    = 850;   // 85%：接近实测堵转值 1000，必须处理
uint16_t LOAD_RECOVER = 300;   // 掉到 30% 以下才算负载真解除，计时归零
const uint32_t LOAD_WARN_MS = 10000;
uint32_t LOAD_CRIT_MS = 3000;  // 负载 ≥ LOAD_CRIT 持续这么久才动手

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
int8_t   overtempJoint = -1;   // 哪个关节锁存的：只有**它**降温才能解锁
bool     tempWarned[NUM];      // 按关节记，不然 A 的高温提醒会被 B 的正常读数清掉
uint8_t  hotCnt[NUM];          // 连续高温次数，见 TEMP_CONFIRM
bool     lowbatLatch   = false;
uint8_t  lowVoltCnt    = 0;    // 连续低压计数，滤掉负载造成的瞬时压降

// 过载计时（每关节一份，互不影响）。
// 存「何时开始」而不是「累计了多久」，省掉每个 loop 都加一次的开销，
// 也天然不受 loop 抖动影响。
uint32_t loadWarnSince[NUM];   // 进入 ≥LOAD_WARN 的时刻，0 = 没在计时
uint32_t loadCritSince[NUM];   // 进入 ≥LOAD_CRIT 的时刻
bool     loadWarned[NUM];      // 这一轮高负载已经报过了，别刷屏
bool     loadHeld[NUM];        // 已经为这一轮堵转把目标钉在当前位置了
// 遥测读取失败的累计次数（开机以来）。堵转时如果这个数在涨，
// 说明「保护没触发」不是阈值问题，而是根本读不到负载 —— 两种情况修法完全不同。
uint16_t telemFail[NUM];

// 到位回报：发完指令后等 MOVE_TIME 再回报，避免高频刷屏
bool reportPending = false;
unsigned long reportAt = 0;

// ==================== 夹持加力（夹爪专用）====================
/*
 * 夹住东西以后，光拖滑块是不够的：
 *   位置舵机的输出力矩 ∝ **目标位置和实际位置之差**。
 *   物体挡住了，手指停在原地，你就算再把滑块往闭合方向拖，
 *   遥测同步也会把滑块拽回实际位置（见 useArmControl.syncFromTelemetry），
 *   误差根本累积不起来 —— 手感就是"拖不动了"。
 *
 * 所以单开一条通道：以**开始夹持那一刻的实际位置**为基准，
 * 每按一次「加力」就把目标再往闭合方向多顶 GRIP_STEP_RAW 个计数。
 * 物体不变形、手指不动都没关系，误差一档一档叠上去，
 * 舵机输出的力矩也就一档一档变紧 —— 这就是"微微缩紧"。
 *
 * 位置读数照实回报，不做任何美化：
 * 物体被压扁了、手指真合拢了，遥测里的角度会跟着变；
 * 没变就是没变，界面显示的是实测值而不是我们的命令值。
 */
const int GRIPPER = 5;                 // 夹爪在数组里的下标
const uint16_t GRIP_STEP_RAW  = 16;    // 一档 = 16 counts ≈ 2°
const uint16_t GRIP_MAX_RAW   = 320;   // 最多累计 40°，再多纯粹是顶死发热
const uint16_t GRIP_MAX_LOAD  = 450;   // 负载到 45% 额定扭矩就不再加大

bool     gripActive  = false;   // 是否正在夹持
bool     gripCapped  = false;   // 是否已因负载到顶而锁住（松力才解除）
uint16_t gripBaseRaw = 0;       // 进入夹持时的实际位置
uint16_t gripPushRaw = 0;       // 累计往闭合方向多顶的 raw 计数

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
/*
 * 网页的 0~180° 线性映射到**该关节实测的机械行程**，不再是全量程。
 * 六个关节行程各不相同（114°~228°），所以必须逐关节算。
 */
uint16_t angleToPos(int i, int a) {
  a = constrain(a, 0, 180);
  // ⚠️ 这里不能假设 RAW_MIN < RAW_MAX：肘部是倒着写的（0° = 收拢 = raw 大）。
  //    先转成有符号 long 再算，差值就是负的，加回去自然对。
  long lo = RAW_MIN[i], hi = RAW_MAX[i];
  return (uint16_t)(lo + (hi - lo) * a / 180);
}

/*
 * 反函数：上电时把「当前实际位置」换算回滑块角度。
 * 用途见 setup() 里「保持当前姿态」那段 —— 不回中，原地不动。
 *
 * ⚠️ 同样要认倒序关节。原来写的是
 *      if (p <= RAW_MIN) return 0;  if (p >= RAW_MAX) return 180;
 *    对 RAW_MIN=3083 / RAW_MAX=1013 的肘部，任何 p 都满足第二句，
 *    于是**永远返回 180** —— 上电保持姿态时肘部会被一把拽向另一端。
 *    倒序时改看「离哪一端近」。
 */
int posToAngle(int i, uint16_t p) {
  long lo = RAW_MIN[i], hi = RAW_MAX[i];
  if (lo <= hi) {
    if (p <= lo) return 0;
    if (p >= hi) return 180;
    return (int)((p - lo) * 180 / (hi - lo));
  }
  // 倒序关节：滑块 0 在 raw 大的那一端
  if (p >= lo) return 0;
  if (p <= hi) return 180;
  return (int)((lo - p) * 180 / (lo - hi));
}

/*
 * 夹持状态下夹爪的原始目标位置。
 * 注意是"基准 − 累计顶进量"：0° 那一端是闭合方向（见 RAW_MIN/MAX 的说明）。
 * 夹到最紧也只能到 RAW_MIN[GRIPPER]，那是两片手指碰上的位置，
 * 再往前就是硬顶机械限位，只会发热不会更紧。
 */
uint16_t gripperGoalRaw() {
  long g = (long)gripBaseRaw - (long)gripPushRaw;
  if (g < (long)RAW_MIN[GRIPPER]) g = RAW_MIN[GRIPPER];
  return (uint16_t)g;
}

// 把当前所有目标角度一次性同步下发
void applyTargets() {
  uint16_t pos[NUM];
  for (int i = 0; i < NUM; i++) {
    // 夹持中的夹爪不走角度映射，直接用 raw 目标。
    // 走映射的话目标会被 target[5]（跟着遥测回退的滑块值）盖掉，加力就废了。
    pos[i] = (i == GRIPPER && gripActive) ? gripperGoalRaw() : angleToPos(i, target[i]);
  }
  syncWritePosition(pos, NUM);

  reportPending = true;
  reportAt = millis() + MOVE_TIME + 50;
}

/*
 * 退出夹持：目标回到「开始夹持那一刻的实际位置」，累计顶进量清零。
 *
 * 单独抽出来是因为现在有两个地方要松夹：
 *   网页按「松力」（Z:0），以及过载保护判定夹爪顶死了。
 * 两处必须做**完全一样**的事 —— 抽出来才不会改了一处漏了另一处。
 */
void releaseGrip() {
  if (!gripActive) return;
  target[GRIPPER]     = posToAngle(GRIPPER, gripBaseRaw);
  prevTarget[GRIPPER] = target[GRIPPER];
  gripActive = false;
  gripCapped = false;
  gripPushRaw = 0;
  applyTargets();
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

  /*
   * 最多试 3 次。原因：**上电后第一个被访问的舵机经常收不到回包**。
   * 微雪转接板的半双工收发器上电后方向控制还没稳，第一帧被吃掉。
   * 症状极具迷惑性：ID 1 永远 ✗、2~6 全 ✓ —— 而 ID 1 恰好总是
   * 第一个被 ping 的（循环从 0 开始）。换回 USB 模式又好了，
   * 很容易误判成「底座那根线松了 / 底座舵机坏了」。
   */
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
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

    if (buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == id) return true;
    delay(5);
  }
  return false;
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
  // 读失败重试一次再放弃。实测失败率约 5%（回包开头偶尔被总线抢线顶坏，
  // 见上面 busRelease 那段说明），而保护是**按持续时间**判的 —— 丢一帧
  // 只是少一次采样，但连续丢就会让「持续 3 秒高负载」永远凑不满。
  bool ok = readTelemetry(SERVO_ID[i], t, REG_TELEM_LEN) ||
            readTelemetry(SERVO_ID[i], t, REG_TELEM_LEN);
  if (!ok) {
    // 两次都失败就跳过这轮。不要拿旧值硬凑 —— 会解出假的负载突变。
    // 计数留着事后看：S 命令会打出来。
    if (telemFail[i] < 65535) telemFail[i]++;
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

  // ---- 过载 / 堵转：按「持续时间」判，和上面的温度是两个独立的角 ----
  // 放在夹持分支**之前**：夹持中的夹爪万一真顶到 85%，松夹也比烧舵机强。
  checkOverload(i, mag, pos);

  /*
   * ---- 夹持中的夹爪：只看到达负载上限，不做碰撞回退 ----
   *
   * 夹紧的时候负载就是会猛涨，那是**预期行为**，不是撞到东西了。
   * 放进下面的碰撞逻辑里，用户每按一次加力都会被判成碰撞、
   * 自动退回上一位置 —— 加力按钮等于失效。
   * 所以单独走一条路：超过 GRIP_MAX_LOAD 就锁住，不再允许加大。
   */
  if (i == GRIPPER && gripActive) {
    lastLoad[i] = mag;    // 仍然跟住负载，松力后恢复碰撞判定时不会误报
    spikeCnt[i] = 0;
    if (mag >= GRIP_MAX_LOAD && !gripCapped) {
      gripCapped = true;
      Serial.printf("✋ 夹持到顶（负载 %u ≥ %u）→ 不再加大\n", mag, GRIP_MAX_LOAD);
      ws.sendTXT("E:griplimit," + String(GRIPPER) + "," + String(mag));
    }
    return;
  }

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
  // ---- 温度：先确认再动作 ----
  // 一次坏读数不值得把整条手臂的力松掉，连续 TEMP_CONFIRM 次才算。
  if (temp >= TEMP_WARN) {
    if (hotCnt[i] < 255) hotCnt[i]++;
  } else {
    hotCnt[i] = 0;
    // 迟滞 5°C，免得在阈值上下抖动反复开关力。
    // ⚠️ 只认**锁存的那个关节**降温：以前是任何关节读到凉快就解锁，
    //    底座烧到 76°C、肩部 40°C 就会来回解锁/锁存，力一闪一闪。
    if (overtempLatch && i == overtempJoint && temp < TEMP_WARN - 5) {
      overtempLatch = false;
      overtempJoint = -1;
      setTorque(true);
      Serial.println("❄️  温度回落，恢复扭矩");
    }
  }

  if (temp >= TEMP_STOP && !overtempLatch && hotCnt[i] >= TEMP_CONFIRM) {
    overtempLatch = true;
    overtempJoint = i;
    Serial.printf("🔥 %s 过温 %u°C（≥%u）→ 松力\n", NAMES[i], temp, TEMP_STOP);
    ws.sendTXT("E:overtemp," + String(i) + "," + String(temp));
    setTorque(false);
  } else if (temp >= TEMP_WARN && hotCnt[i] >= TEMP_CONFIRM && !tempWarned[i]) {
    tempWarned[i] = true;
    Serial.printf("🌡️  %s 温度偏高 %u°C\n", NAMES[i], temp);
    ws.sendTXT("E:overtemp_warn," + String(i) + "," + String(temp));
  } else if (temp < TEMP_WARN - 5) {
    tempWarned[i] = false;
  }

  // 电压：6 个舵机同时使劲时会被拉低，所以只看持续低压。
  // 单次低于阈值不报 —— 那是负载造成的正常压降，不是电池没电了。
  if (volt > 0 && volt < VOLT_WARN) {
    // 低于 VOLT_STOP 不再等「持续」：10.5V 是"该充电了"，
    // 10.0V 已经是电芯过放边缘，多撑几秒换不来什么，直接把计数拉满。
    if (volt < VOLT_STOP) lowVoltCnt = 10;
    else if (lowVoltCnt < 10) lowVoltCnt++;

    if (lowVoltCnt >= 10 && !lowbatLatch) {
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

/*
 * 过载 / 堵转保护（防烧舵机）。
 *
 * 和碰撞判定的分工：
 *   碰撞 = 负载**突变** → 几十毫秒内撞上东西，退回上一位置就完事。
 *   过载 = 负载**持续**高 → 慢慢顶死，或者长时间重力负载，
 *          没有突变，碰撞逻辑永远看不见，但它才是最烤舵机的那种。
 *
 * 两级动作：
 *   ≥60% 10 秒  → 只报警。正常托举也可能到 60%，不能动不动就动手臂。
 *   ≥85%  3 秒  → 到上限：把目标改成**当前实际位置**，误差归零，舵机立刻
 *                 停止推进。手臂停在原地，不后退、不松力、不掉。
 *
 * 为什么是「停在原地」而不是退回或松力：
 *   负载寄存器正比于位置误差，被外部挡住时误差消不掉才会顶到满量程。
 *   把目标钉在当前位置，误差当场变成 0，舵机自然就撒劲了 ——
 *   这是唯一既不让它继续烤、又不会让手臂突然塌下来的做法。
 *   代价是挡着的东西挪开以后它不会自己恢复，要重新下指令。
 *
 * 为什么不需要锁存：
 *   停在原地本身就是可恢复的 —— 下一帧目标一到，它照常走。
 *   真顶死的话每 LOAD_CRIT_MS 会重新钉一次，负载永远上不去，
 *   所以没有「必须先断电」这种状态。
 */
void checkOverload(uint8_t i, uint16_t mag, uint16_t pos) {
  uint32_t now = millis();

  // 负载真的降下来了 → 计时全部归零，准备下一次判定。
  // 用 30% 而不是临界值 85%：中间那一段（30%~85%）说明已经脱离堵转，
  // 只是还在使劲。这时把计时清掉，下次真顶死要重新累计满 3 秒才动作 ——
  // 不然一次长动作会被反复判成堵转。
  if (mag < LOAD_RECOVER) {
    loadWarnSince[i] = 0;
    loadCritSince[i] = 0;
    loadWarned[i]    = false;
    loadHeld[i]      = false;
    return;
  }

  // ---- 第一级：提醒 ----
  if (mag >= LOAD_WARN) {
    if (loadWarnSince[i] == 0) {
      loadWarnSince[i] = now;
    } else if (!loadWarned[i] && now - loadWarnSince[i] >= LOAD_WARN_MS) {
      loadWarned[i] = true;
      Serial.printf("⚠️  %s 持续高负载 %u（≥%u，已 10 秒）\n",
                    NAMES[i], mag, LOAD_WARN);
      ws.sendTXT("E:loadwarn," + String(i) + "," + String(mag));
    }
  } else {
    loadWarnSince[i] = 0;
    loadWarned[i]    = false;
  }

  // ---- 第二级：到上限就停在原地 ----
  if (mag < LOAD_CRIT) {
    loadCritSince[i] = 0;
    loadHeld[i]      = false;
    return;
  }

  if (loadCritSince[i] == 0) {          // 刚顶上去，先看满 3 秒
    loadCritSince[i] = now;
    Serial.printf("⏱ %s 负载 %u ≥ %u，开始计时\n", NAMES[i], mag, LOAD_CRIT);
    return;
  }
  if (now - loadCritSince[i] < LOAD_CRIT_MS) return;

  // 还顶死 → 重新钉一次、重新计时。挡着的东西没挪开，就每 3 秒钉一次，
  // 负载不会有机会继续往上冲。
  loadCritSince[i] = now;
  // 主动改了目标，负载必然变化。把基线跟过去，
  // 否则下一轮碰撞判定会把这次自己造成的负载变化当成"撞到了"。
  lastLoad[i] = mag;
  spikeCnt[i] = 0;

  if (i == GRIPPER && gripActive) {
    // 夹持中的夹爪没有「当前目标」可钉 —— 目标被 gripperGoalRaw() 接管了，
    // 改 target[] 不生效。这种时候只能松夹，等价于用户按了「松力」。
    releaseGrip();
    if (!loadHeld[i]) {
      loadHeld[i] = true;
      Serial.printf("🛑 夹爪夹持负载 %u ≥ %u → 松力\n", mag, LOAD_CRIT);
      ws.sendTXT("E:stall," + String(i) + "," + String(mag));
    }
    return;
  }

  int hold = posToAngle(i, pos);
  target[i]     = hold;
  prevTarget[i] = hold;
  applyTargets();

  // 只在**刚进入**这个状态时上报一次，之后每 3 秒的重钉不再刷屏。
  if (!loadHeld[i]) {
    loadHeld[i] = true;
    Serial.printf("🛑 %s 负载 %u ≥ %u → 停在当前位置 %d°（不再使劲）\n",
                  NAMES[i], mag, LOAD_CRIT, hold);
    ws.sendTXT("E:stall," + String(i) + "," + String(mag));
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

      /*
       * 夹持命令 "Z:<档数>"：
       *   Z:2   加 2 档（每档 ≈ 2°），连续按就一档档变紧
       *   Z:-1  退一档（还夹着，只是松一点）
       *   Z:0   完全松力，目标回到开始夹持时的位置
       *
       * ⚠️ 必须放在下面那个通用解析**之前**：通用解析会把 "Z" 当成
       *    舵机编号（toInt 得 0），于是 "Z:2" 被理解成"底座转到 2°"，
       *    一按加力底座就甩过去。
       */
      if (msg.startsWith("Z:")) {
        int n = constrain(msg.substring(2).toInt(), -20, 20);

        if (n == 0) {
          // ---- 松力 ----
          if (gripActive) {
            releaseGrip();
            Serial.println("夹持：松力");
          }
          break;
        }

        if (!gripActive) {
          // ---- 第一次加力：先把"现在夹在哪儿"记成基准 ----
          uint8_t buf[2];
          bool ok = false;
          for (int a = 0; a < 3 && !ok; a++) {
            ok = readTelemetry(SERVO_ID[GRIPPER], buf, 2);
            if (!ok) delay(5);
          }
          if (!ok) {
            // 读不到位置就**一点都不动**。没有基准的加力等于瞎顶。
            Serial.println("夹持：读不到夹爪位置，不加力");
            ws.sendTXT("E:gripfail," + String(GRIPPER) + ",");
            break;
          }
          gripBaseRaw = buf[0] | (buf[1] << 8);
          gripPushRaw = 0;
          gripActive  = true;
          gripCapped  = false;
          Serial.printf("夹持开始，基准 raw=%u\n", gripBaseRaw);
        }

        if (n > 0 && gripCapped) {
          // 已经顶到负载上限了，再按也不加。要更紧得先松力重新夹。
          Serial.println("夹持：已到负载上限，先松力才能再加");
          ws.sendTXT("E:griplimit," + String(GRIPPER) + "," + String(GRIP_MAX_LOAD));
          break;
        }

        long next = (long)gripPushRaw + (long)n * GRIP_STEP_RAW;
        if (next < 0) next = 0;
        if (next > GRIP_MAX_RAW) next = GRIP_MAX_RAW;
        gripPushRaw = (uint16_t)next;
        if (n < 0) gripCapped = false;   // 退了一档就不再算锁死

        applyTargets();
        Serial.printf("夹持：%+d 档，累计 %u raw，目标 raw=%u\n",
                      n, gripPushRaw, gripperGoalRaw());
        break;
      }

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

  // 给转接板的收发器一点上电稳定时间。太短的话第一帧必丢，
  // 表现就是自检里第一个被访问的 ID 显示 ✗（详见 pingServo）。
  delay(300);
  while (servoBus.available()) servoBus.read();   // 清掉上电毛刺

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

  /*
   * 一个都没应答 → **一个字节都不要发**。
   *
   * 以前这里不管自检结果，继续用默认的 90° 调 applyTargets()。
   * 那意味着：先插 USB 让固件跑起来、之后才开舵机电源时，
   * 固件已经朝空总线发过一遍"六个关节全部到 90°"——舵机一上电收到
   * 下一条命令就会被同时拽到 90°，手臂会毫无征兆地甩出去。
   * 没读到真实位置，就没有资格发目标。
   */
  if (online == 0) {
    Serial.println("⛔ 总线无任何应答 → 不发送任何目标位置，舵机保持原状。");
    Serial.println("   先确认：舵机电源开关开了吗？共地了吗？");
    Serial.println("   接线检查完按 RST 重新自检。");
  } else {
    if (online < NUM) {
      // 部分应答：读不到位置的那个关节会用默认 90°，
      // 一旦下命令它就会往 90° 跑。宁可说清楚，也不要用户被吓一跳。
      Serial.println("⚠️ 有舵机没应答：读不到位置的那个关节会按 90° 处理，"
                     "下命令前先确认它当前位置。");
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

    /*
     * ⚠️ 上电**不回中**，原地保持。
     *
     * 舵盘是在舵机任意角度下装上去的，「回中」对这台机器意味着让关节
     * 甩几十上百度过某个陌生位置，很容易撞到桌子或自己。
     * 所以先读当前位置、反算成滑块角度、原样下发一遍 —— 手臂纹丝不动，
     * 只是把力打上，之后就由网页从当前姿态接着控制。
     */
    for (int i = 0; i < NUM; i++) {
      uint8_t buf[2];
      bool ok = false;
      for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        ok = readTelemetry(SERVO_ID[i], buf, 2);
        if (!ok) delay(5);
      }
      if (ok) {
        uint16_t p = buf[0] | (buf[1] << 8);
        target[i] = posToAngle(i, p);
        prevTarget[i] = target[i];
        Serial.printf("   %s 当前位置 %u → 滑块 %d°\n", NAMES[i], p, target[i]);
      } else {
        Serial.printf("   %s 读不到当前位置，保持默认 %d°\n", NAMES[i], target[i]);
      }
    }
    applyTargets();
    Serial.println("总线舵机初始化完成（保持当前姿态）");
  }   // end: online > 0

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

  /*
   * ⚠️ 发射功率必须**连上之后再压**，不能放在 WiFi.begin() 前面。
   *
   * 这台板子是**电脑 USB 供电**，WiFi 持续发射时的电流尖峰会把 3.3V
   * 拉塌，日志里表现为 `E BOD: Brownout detector was triggered` 然后复位，
   * 无限重启循环 —— 而且往往正好发生在连上服务器前后，看着像服务器的问题。
   *
   * 以前把 setTxPower 写在 begin() 之前，等于白写：STA 驱动在 begin()
   * 里会把功率重新设回默认 20dBm，之前那句被覆盖掉了。所以时好时坏。
   * 现在放在连上之后，才是真正生效的时机。
   *
   * 8.5dBm 在同一个房间里连路由器绰绰有余，用一点信号余量换稳定性很划算。
   * 如果换了好线好口还是掉，再往下调到 WIFI_POWER_5dBm。
   */
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  Serial.printf("   RSSI: %d dBm\n", WiFi.RSSI());

  // 给电源一点缓冲时间再开 WebSocket：紧接着 WiFi 关联结束后
  // 立刻跑 TLS/TCP 握手，电流尖峰会叠在一起，容易正好踩在 BOD 上。
  delay(300);

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
/*
 * 读串口里紧跟着的一串数字，用来实现 c850 这类带参数的调试命令。
 *
 * 只在**已经读到命令字母之后**调用：数字是连着敲进来的，正常几个毫秒就读完。
 * 超时设 300ms 而不是更长 —— 这段是阻塞的，卡太久 ws.loop() 得不到调用，
 * 服务器心跳会断。
 *
 * 返回 -1 表示后面没有数字（用户只敲了字母就回车）。
 */
long readSerialNumber() {
  long v = -1;
  uint32_t last = millis();
  while (millis() - last < 300) {
    if (!Serial.available()) { delay(2); continue; }
    char ch = Serial.read();
    last = millis();
    if (ch >= '0' && ch <= '9') {
      if (v < 0) v = 0;
      if (v < 100000) v = v * 10 + (ch - '0');
    } else {
      break;                       // 回车、空格、其它字母都算结束
    }
  }
  return v;
}

void handleSerial() {
  if (!Serial.available()) return;
  char c = Serial.read();

  // '1' '2' '3' '4' —— 切换动作速度（到位时间）。
  //      录演示视频时切「很慢」，底盘不容易被甩倒，动作也看得清；
  //      平时用「快」跟手。改的是舵机的 GOAL_TIME 寄存器，立即生效，不用重烧。
  if (c >= '1' && c <= '4') {
    MOVE_TIME = (c == '1') ? 400 : (c == '2') ? 900 : (c == '3') ? 1800 : 3000;
    for (int i = 0; i < NUM; i++) {
      writeWord(SERVO_ID[i], REG_GOAL_TIME, (uint16_t)MOVE_TIME);
    }
    Serial.printf("动作速度: %d ms/段（1=快 2=中 3=慢 4=很慢）\n", MOVE_TIME);
    return;
  }

  // 'c' 'k' 'm' 'd' —— 现场改保护阈值，不用重烧板子。
  //   用法是「字母 + 数字」，数字打到回车为止：c850 / k300 / m3000 / d250
  //   c = 过载上限（负载绝对值）      k = 负载降到多少算真解除（计时归零）
  //   m = 持续多少毫秒才动手          d = 碰撞的负载突变阈值
  //
  //   为什么要有这几个：判「堵转」靠的是负载持续高，而阈值定得对不对
  //   只有真顶死一次才知道。有了它们可以在负载小的动作上先把触发链路验一遍
  //   （c100 k30 m800），不用真的把舵机顶到 1000。改完记得改回去 ——
  //   S 命令每次都会把当前值打出来提醒。
  if (c == 'c' || c == 'k' || c == 'm' || c == 'd') {
    char which = c;
    long v = readSerialNumber();
    if (v < 0) {
      Serial.printf("用法: %c<数字>  当前 临界=%u 解除=%u 时长=%ums 碰撞=%u\n",
                    which, LOAD_CRIT, LOAD_RECOVER, (unsigned)LOAD_CRIT_MS, LOAD_DELTA);
      return;
    }
    switch (which) {
      case 'c': LOAD_CRIT    = (uint16_t)constrain(v, 1, 1000); break;
      case 'k': LOAD_RECOVER = (uint16_t)constrain(v, 1, 1000); break;
      case 'm': LOAD_CRIT_MS = (uint32_t)constrain(v, 50, 60000); break;
      case 'd': LOAD_DELTA   = (uint16_t)constrain(v, 10, 1000); break;
    }
    Serial.printf("阈值已改: 临界=%u 解除=%u 时长=%ums 碰撞突变=%u\n",
                  LOAD_CRIT, LOAD_RECOVER, (unsigned)LOAD_CRIT_MS, LOAD_DELTA);
    // 改了阈值，正在进行中的计时按旧阈值算的，全部作废重来
    for (int i = 0; i < NUM; i++) {
      loadCritSince[i] = 0;
      loadHeld[i] = false;
    }
    return;
  }

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
    // 阈值是运行时可改的，这里每次都打出来：改完忘了改回去是最容易犯的错
    Serial.printf("保护:    过载临界=%u 解除=%u 持续=%ums  碰撞突变=%u  动作=%dms\n",
                  LOAD_CRIT, LOAD_RECOVER, (unsigned)LOAD_CRIT_MS, LOAD_DELTA, MOVE_TIME);

    // 遥测读失败的累计次数。0 才是正常；堵转时如果它一直涨，
    // 说明保护看不到负载，问题在总线而不是阈值。
    bool anyFail = false;
    for (int i = 0; i < NUM; i++) if (telemFail[i]) anyFail = true;
    if (anyFail) {
      Serial.print("读失败:  ");
      for (int i = 0; i < NUM; i++) Serial.printf("%s=%u ", NAMES[i], telemFail[i]);
      Serial.println();
    }

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

  // 'R' —— 松力。台面校准要用：固件正常运行时是带力保持的，
  //       不松力就没法用手把机械臂摆到机械限位。
  if (c == 'R' || c == 'r') {
    setTorque(false);
    Serial.println("已松力 —— 手臂会软下来，先扶稳再撒手");
    return;
  }

  // 'T' —— 上力。摆完要接着控制时用它，不用重新上电。
  if (c == 'T' || c == 't') {
    setTorque(true);
    Serial.println("已上力（原地保持，不会动）");
    return;
  }

  // 'P' —— 打印六个关节的原始位置。校准抄数用。
  //       angle 是**当前映射**下的滑块角度，用来判断这一端该算 0 还是 180。
  if (c == 'P' || c == 'p') {
    for (int i = 0; i < NUM; i++) {
      uint8_t buf[2];
      bool ok = false;
      for (int a = 0; a < 3 && !ok; a++) {
        ok = readTelemetry(SERVO_ID[i], buf, 2);
        if (!ok) delay(5);
      }
      if (ok) {
        uint16_t p = buf[0] | (buf[1] << 8);
        Serial.printf("%d %s raw=%u angle=%d\n",
                      SERVO_ID[i], NAMES[i], p, posToAngle(i, p));
      } else {
        Serial.printf("%d %s 读不到\n", SERVO_ID[i], NAMES[i]);
      }
    }
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
  Serial.printf(" (碰撞 delta=%u floor=%u / 过载 warn=%u crit=%u)\n",
                LOAD_DELTA, LOAD_FLOOR, LOAD_WARN, LOAD_CRIT);

  // 哪个关节正在往「堵转」的方向累计时间。台面调阈值时靠这行看，
  // 不然只能等它真触发才知道阈值定高了还是低了。
  uint32_t now = millis();
  bool any = false;
  for (int i = 0; i < NUM; i++) {
    if (loadWarnSince[i] == 0 && loadCritSince[i] == 0) continue;
    if (!any) {
      Serial.print("过载计时: ");
      any = true;
    }
    Serial.printf("%s", NAMES[i]);
    if (loadCritSince[i]) {
      Serial.printf("(临界 %.1fs)", (now - loadCritSince[i]) / 1000.0);
    } else {
      Serial.printf("(偏高 %.1fs)", (now - loadWarnSince[i]) / 1000.0);
    }
    Serial.print(" ");
  }
  if (any) Serial.println();
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
