/**
 * 假 ESP32 —— 用来在没有硬件的情况下跑通整条链路
 *
 * 它把自己伪装成一台真的 ESP32 连上服务器：
 *   1. 收 浏览器 -> 服务器 -> 设备 的角度命令，像真机一样回"到位"
 *   2. 持续上报 T: 遥测，让力反馈仪表盘有数据可显示
 *   3. 能按命令伪造碰撞 / 过温 / 低压事件，验证报警链路
 *
 * 为什么需要它：STM32/舵机还没到货，但"服务器能不能把遥测转给浏览器"
 * 这件事现在就能验证。等硬件到了，把固件烧进去，行为对得上就说明通了。
 *
 * 用法：
 *   node simulator/fake-esp32.js                  # 正常运转
 *   node simulator/fake-esp32.js --collision      # 每 20 秒伪造一次碰撞
 *   node simulator/fake-esp32.js --overtemp       # 每 20 秒伪造一次过温
 *   node simulator/fake-esp32.js --silent         # 不刷屏，只报错
 *   node simulator/fake-esp32.js --url ws://...   # 连别的服务器（默认本机 3000）
 *
 * 调试技巧：跑起来后打开网页控制台，随便拖一个滑块 —— 假臂会慢慢转过去，
 * 仪表盘上的负载会跟着变。捏住不放（连续发同一个角度）会看到负载升高。
 */

const WebSocket = require('ws');
const config = require('../config');

// ---------- 命令行参数 ----------
const argv = process.argv.slice(2);
const has = (flag) => argv.includes(flag);
const getArg = (name, dflt) => {
  const i = argv.indexOf(name);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : dflt;
};

const SILENT = has('--silent');
const SCENARIO = has('--collision') ? 'collision'
  : has('--overtemp') ? 'overtemp'
  : has('--lowbat') ? 'lowbat'
  : null;
const URL = getArg('--url', 'ws://127.0.0.1:' + config.PORT + '/?token=' + config.ESP32_TOKEN);

// ---------- 假机械臂的物理模型 ----------
//
// 不求精确，只求"量级和趋势像那么回事"：
//   · 位置平滑趋近目标（不是瞬移），这样仪表盘上能看到运动过程
//   · 肩部/肘部扛重力，越接近水平负载越大 —— 这是碰撞检测要用到的基线
//   · 电压随负载轻微下陷，长时间运行缓慢下降（模拟电池放电）
const JOINTS = [
  { name: '底座',   maxSpeed: 90, gravity: 0.00, stallLoad: 520 },
  { name: '肩部',   maxSpeed: 55, gravity: 1.00, stallLoad: 780 },
  { name: '肘部',   maxSpeed: 70, gravity: 0.70, stallLoad: 640 },
  { name: '腕俯仰', maxSpeed: 95, gravity: 0.25, stallLoad: 420 },
  { name: '腕旋转', maxSpeed: 110, gravity: 0.05, stallLoad: 380 },
  { name: '夹爪',   maxSpeed: 130, gravity: 0.00, stallLoad: 900 },
];

const state = JOINTS.map((j) => ({
  pos: 90,        // 当前角度（度）
  target: 90,     // 目标角度（度）
  temp: 31,       // 温度（摄氏度），从室温起步
  reported: true, // 上一次上报时是否已到位（用于"到位回报"去重）
}));

// 12.6V 满电起步。真机是 3S 锂电，跑到 10.5V 就该报警了。
let packVoltage = 12.6;

// ---------- 单位换算：角度 -> 寄存器原始值 ----------
// 服务器会反着解一遍（raw * 360 / 4096），所以这里必须用 4096 而不是 4095。
const degToRawPos = (deg) => Math.round((deg * 4096) / 360);

// 负载寄存器：低 10 位是大小（1000 = 额定扭矩的 100%），bit10 是方向位。
const LOAD_DIR_BIT = 0x400;

/**
 * 算某个关节此刻的负载原始值。
 *
 * 重力项：关节越接近水平（角度接近 90°）力臂越长，负载越大。
 * 用 sin 而不是线性，是因为力臂 = L·sin(θ)，这是真的物理不是凑数。
 */
function jointLoad(i, moving) {
  const j = JOINTS[i];
  const s = state[i];
  const gravityPart = j.gravity * 620 * Math.abs(Math.sin((s.pos * Math.PI) / 180));

  // 运动时额外加一点 —— 克服惯性和摩擦
  const movePart = moving ? 90 : 0;

  // 温漂：热了以后摩擦力变大，空载负载也会抬高一点点
  const heatPart = Math.max(0, (s.temp - 45) * 3);

  let mag = Math.min(1000, Math.round(gravityPart + movePart + heatPart + 40));

  // 方向位：真机里表示负载指向哪边。这里只在运动时点亮，
  // 因为静止时方向没有意义 —— 正好也验证服务器没把这个位当数值用。
  if (moving) mag |= LOAD_DIR_BIT;
  return mag;
}

// ---------- 主循环：物理推进 ----------
const TICK_MS = 100;

function tickPhysics() {
  JOINTS.forEach((j, i) => {
    const s = state[i];

    // 位置向目标平滑移动
    const diff = s.target - s.pos;
    const step = Math.sign(diff) * Math.min(Math.abs(diff), (j.maxSpeed * TICK_MS) / 1000);
    s.pos = Math.abs(diff) < 0.5 ? s.target : s.pos + step;

    const moving = Math.abs(diff) >= 0.5;

    // 温度：重载升温、空载散热。系数是拍的，但时间常数大致对 ——
    // 堵转几十秒该烫了，这是过温保护要抓的。
    const load = jointLoad(i, moving) & 0x3FF;
    s.temp += (load / 1000) * 0.55 - (s.temp - 28) * 0.02;
    s.temp = Math.max(28, Math.min(95, s.temp));

    // 电压：负载拉到会下陷，另外随时间缓慢放电
    if (moving) packVoltage -= (load / 1000) * 0.004;
    packVoltage -= 0.0002;
    packVoltage = Math.max(9.0, packVoltage);
  });
}

// ---------- 遥测上报：一轮一个关节 ----------
//
// 为什么轮着报而不是一次报 6 个：真机上读寄存器是阻塞的，
// 一口气读完 6 个关节会卡住 WebSocket 心跳导致掉线。
// 服务器那边按关节号填格子，所以顺序乱、丢几个都不影响。
let pollIdx = 0;

function sendTelemetry() {
  const i = pollIdx;
  pollIdx = (pollIdx + 1) % JOINTS.length;

  const s = state[i];
  const moving = Math.abs(s.target - s.pos) >= 0.5;
  const load = jointLoad(i, moving);

  // 电流大致正比于负载：满负载 ≈ 额定电流。STS3215 堵转约 3A，
  // 这里按 2.4A 满量程折算，够仪表盘显示相对大小了。
  const currentMa = 60 + (load & 0x3FF) * 2.4;

  // 寄存器原始值：电流单位是 6.5mA，电压单位是 0.1V
  const fields = [
    i,
    degToRawPos(s.pos),
    load,
    Math.round(packVoltage * 10),
    Math.round(s.temp),
    Math.round(currentMa / 6.5),
  ];
  send('T:' + fields.join(','));
}

// ---------- 到位回报 ----------
//
// 全部关节都到目标位置时，回一条 "0:90,1:90,..."。
// 这是老格式，服务器原样转发给浏览器 —— 保持兼容，别改。
function reportArrival() {
  const arrived = JOINTS.map((j, i) => Math.abs(state[i].target - state[i].pos) < 0.5);
  const allDone = arrived.every(Boolean);

  if (allDone && state.some((s) => !s.reported)) {
    state.forEach((s) => { s.reported = true; });
    const msg = state.map((s, i) => i + ':' + Math.round(s.pos)).join(',');
    send(msg);
    log('到位回报:', msg);
  } else if (!allDone) {
    state.forEach((s) => { s.reported = false; });
  }
}

// ---------- 伪造故障事件 ----------
const EVENT_TYPES = {
  collision: () => {
    // 碰撞的特征是负载「突变」而不是负载高 —— 所以先让它低，再猛拉一下。
    // 固件那边用的是增量判据，这样伪造才和真机行为对得上。
    const i = 1 + Math.floor(Math.random() * 3); // 肩/肘/腕
    state[i].target = state[i].pos;             // 卡住了，转不动
    state[i].temp += 6;                          // 堵转升温快
    const mag = 760 + Math.floor(Math.random() * 200);
    send('E:collision,' + i + ',' + mag);
    log('💥 伪造碰撞: 关节' + i + ' 负载 ' + mag);
  },
  overtemp: () => {
    const i = Math.floor(Math.random() * JOINTS.length);
    state[i].temp = 78;
    send('E:overtemp,' + i + ',' + Math.round(state[i].temp));
    log('🔥 伪造过温: 关节' + i + ' ' + Math.round(state[i].temp) + '°C');
  },
  lowbat: () => {
    packVoltage = 10.4;
    send('E:lowbat,,' + Math.round(packVoltage * 10));
    log('🪫 伪造低压: ' + packVoltage.toFixed(1) + 'V');
  },
};

// ---------- 连接 ----------
let ws = null;
let timers = [];

function send(text) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(text);
}

function log(...args) {
  if (!SILENT) console.log(...args);
}

function cleanup() {
  timers.forEach(clearInterval);
  timers = [];
}

function connect() {
  log('连接 ' + URL.replace(/token=.*/, 'token=***'));
  ws = new WebSocket(URL);

  ws.on('open', () => {
    log('✅ 已连上服务器，开始模拟 ESP32');
    log('   6 个关节 / 遥测每 ' + TICK_MS + 'ms 一个关节（一轮 ' + (TICK_MS * 6) + 'ms）');
    if (SCENARIO) log('   ⚠️  每 20 秒伪造一次「' + SCENARIO + '」事件');

    cleanup();
    timers.push(setInterval(() => { tickPhysics(); reportArrival(); }, TICK_MS));
    timers.push(setInterval(sendTelemetry, TICK_MS));
    if (SCENARIO) timers.push(setInterval(EVENT_TYPES[SCENARIO], 20000));
  });

  // 浏览器 -> 服务器 -> 这里。格式 "关节:角度"，例如 "1:135"
  ws.on('message', (data) => {
    const text = data.toString();
    const m = text.match(/^(\d+):(\d+)$/);
    if (!m) {
      log('收到未知命令:', text);
      return;
    }
    const i = Number(m[1]);
    const deg = Math.min(180, Math.max(0, Number(m[2])));
    if (i >= JOINTS.length) return;

    state[i].target = deg;
    state[i].reported = false;
    log('→ ' + JOINTS[i].name + ' 转到 ' + deg + '°');

    // 如果正卡在碰撞里，这里是"松手"的时机，让它动起来
    if (SCENARIO === 'collision' && Math.abs(state[i].target - state[i].pos) > 1) {
      state[i].target = deg;
    }
  });

  ws.on('close', () => {
    cleanup();
    log('❌ 与服务器断开，3 秒后重连...');
    setTimeout(connect, 3000);
  });

  ws.on('error', (err) => {
    console.error('连接出错:', err.message);
    console.error('  服务器起了吗？cd server && npm start');
  });
}

// Ctrl+C 时干净退出，别在服务器日志里留一条莫名其妙的"设备断开"
process.on('SIGINT', () => {
  log('\n退出中...');
  cleanup();
  if (ws) ws.close();
  process.exit(0);
});

connect();
