// 机械臂云控制服务器 + 产品网站
//   - WebSocket：ESP32 连进来，浏览器命令转发给它
//   - Express：静态网站（首页/遥控/留言/下载）+ 留言 API（数据存 MySQL）
const http = require('http');
const path = require('path');
const crypto = require('crypto');
const express = require('express');
const WebSocket = require('ws');
const config = require('./config');
const store = require('./store');
const db = require('./db');
const swaggerSpec = require('./swagger');
const swaggerUi = require('swagger-ui-express');

let esp32 = null;         // 当前连进来的 ESP32（只存一台）
let esp32Since = null;     // 它是什么时候连上的（用于展示在线时长）

// 把 async 路由的错误统一接住，避免一个异常把整个进程带崩
// （必须定义在这里 —— 下面的鉴权中间件也要用它）
const wrap = (fn) => (req, res, next) => Promise.resolve(fn(req, res, next)).catch(next);

// 全站安全响应头
//
// 这些头不改变功能，只是关掉浏览器的一些"方便但危险"的默认行为：
//   nosniff        不让浏览器猜类型（防把上传内容当脚本执行）
//   DENY           不许被别的站点嵌进 iframe（防点击劫持）
//   no-referrer    跳转时不把本站地址带给第三方
function securityHeaders(req, res, next) {
  res.setHeader('X-Content-Type-Options', 'nosniff');
  res.setHeader('X-Frame-Options', 'DENY');
  res.setHeader('Referrer-Policy', 'no-referrer');
  res.setHeader('X-XSS-Protection', '0');   // 现代浏览器已废弃该头，显式关掉避免旧浏览器的错误过滤
  next();
}

// ==================== 账号与鉴权 ====================
//
// 全站一套账号：访客注册后登录，凭会话 cookie 访问各功能。
//
// 会话存在 MySQL（见 accounts.js），不存进程内存 ——
// 这样 Python 写的图书管理服务能查同一张表校验同一个 token，
// 全站统一登录态，不需要服务间调用。

const accounts = require('./accounts');

const COOKIE_NAME = 'arm_token';
const SESSION_TTL = accounts.SESSION_TTL_MS;

function parseCookie(req, name) {
  const raw = req.headers.cookie;
  if (!raw) return null;
  for (const part of raw.split(';')) {
    const idx = part.indexOf('=');
    if (idx < 0) continue;
    if (part.slice(0, idx).trim() === name) {
      return decodeURIComponent(part.slice(idx + 1).trim());
    }
  }
  return null;
}

/** 从请求里取出当前账号，未登录返回 null */
function currentToken(req) {
  return parseCookie(req, COOKIE_NAME) || req.get('X-Auth-Token');
}

/**
 * 鉴权中间件。
 *   requireAuth  —— 需登录
 *   requireAdmin —— 需管理员
 * 通过后把账号挂在 req.account 上供路由使用。
 */
function authMiddleware(needAdmin) {
  return wrap(async (req, res, next) => {
    const token = currentToken(req);
    const account = await accounts.getSession(token);
    if (!account) {
      return res.status(401).json({ error: '请先登录' });
    }
    if (needAdmin && account.role !== 'admin') {
      return res.status(403).json({ error: '需要管理员权限' });
    }
    req.account = account;
    next();
  });
}

const requireAuth = authMiddleware(false);
const requireAdmin = authMiddleware(true);

// 定期清理过期会话（存数据库，不会无限增长，但清一下更干净）
setInterval(() => {
  accounts.cleanupExpired()
    .then(n => { if (n) console.log(`清理了 ${n} 个过期会话`); })
    .catch(e => console.error('清理会话失败:', e.message));
}, 6 * 3600 * 1000);

// ==================== 限流与防护 ====================
//
// 三套独立的计数：
//   1. 登录失败   同一 IP 10 分钟失败 5 次 → 封 15 分钟（防爆破）
//   2. 注册       同一 IP 1 小时最多 5 个账号（防灌水）
//   3. 全局请求   同一 IP 1 分钟最多 120 次（防扫描/刷接口）
//
// 都存内存，重启清空 —— 对个人站点够用。要更持久可以换 Redis。

const loginAttempts = new Map();   // ip -> { count, firstAt, blockedUntil }
const MAX_ATTEMPTS = 5;
const ATTEMPT_WINDOW = 10 * 60 * 1000;   // 计数窗口：10 分钟
const BLOCK_DURATION = 15 * 60 * 1000;   // 封禁时长：15 分钟

function clientIp(req) {
  // 前面有 Nginx，真实 IP 在 X-Forwarded-For 里（需配合 app.set('trust proxy')）
  return req.ip || req.socket.remoteAddress || 'unknown';
}

function checkRateLimit(ip) {
  const now = Date.now();
  const rec = loginAttempts.get(ip);
  if (!rec) return { allowed: true };

  if (rec.blockedUntil && now < rec.blockedUntil) {
    return { allowed: false, retryAfterSec: Math.ceil((rec.blockedUntil - now) / 1000) };
  }
  return { allowed: true };
}

function recordFailure(ip) {
  const now = Date.now();
  let rec = loginAttempts.get(ip);

  // 窗口过期就重新计数
  if (!rec || now - rec.firstAt > ATTEMPT_WINDOW) {
    rec = { count: 0, firstAt: now, blockedUntil: 0 };
  }

  rec.count += 1;
  if (rec.count >= MAX_ATTEMPTS) {
    rec.blockedUntil = now + BLOCK_DURATION;
    console.log(`🚫 IP ${ip} 登录失败 ${rec.count} 次，封禁 15 分钟`);
  }
  loginAttempts.set(ip, rec);
}

function clearFailures(ip) {
  loginAttempts.delete(ip);
}

// ---- 注册限流：同一 IP 1 小时最多注册 5 个账号 ----
//
// 注册接口原本完全没有限制，脚本可以每秒创几百个账号把库灌满。

const registerAttempts = new Map();   // ip -> { count, windowStart }
const REGISTER_PER_HOUR = 5;
const REGISTER_WINDOW = 60 * 60 * 1000;

function checkRegisterLimit(ip) {
  const now = Date.now();
  const rec = registerAttempts.get(ip);

  if (!rec || now - rec.windowStart > REGISTER_WINDOW) {
    registerAttempts.set(ip, { count: 0, windowStart: now });
    return true;
  }
  return rec.count < REGISTER_PER_HOUR;
}

function recordRegister(ip) {
  const rec = registerAttempts.get(ip);
  if (rec) rec.count += 1;
}

// ---- 全局请求限流 ----
//
// 主要挡扫描器和暴力刷接口（登录限流只保护登录那一个接口）。
//
// ⚠️ 两个坑（都实际踩过）：
//   1. 阈值一开始设成 120/分钟，太严 —— 拖动滑块时前端每 60ms 发一次，
//      拖十秒就是 160+ 个请求，正常操作反而被拦，控制直接失灵。
//   2. /set 必须排除在全局限流之外，理由同上：它是"用起来就会高频"的接口，
//      靠前端节流 + 登录鉴权约束即可，不该由全局限流管。

const requestCounts = new Map();      // ip -> { count, windowStart }
const REQUESTS_PER_MINUTE = 300;      // 5 次/秒，比正常浏览宽裕得多
const REQUEST_WINDOW = 60 * 1000;

// 不参与全局限流的路径
const RATE_LIMIT_EXEMPT = new Set(['/set']);

function rateLimitMiddleware(req, res, next) {
  if (RATE_LIMIT_EXEMPT.has(req.path)) return next();

  const ip = clientIp(req);
  const now = Date.now();
  let rec = requestCounts.get(ip);

  if (!rec || now - rec.windowStart > REQUEST_WINDOW) {
    rec = { count: 0, windowStart: now };
    requestCounts.set(ip, rec);
  }
  rec.count += 1;

  if (rec.count > REQUESTS_PER_MINUTE) {
    res.setHeader('Retry-After', '60');
    return res.status(429).json({ error: '请求过于频繁，请稍后再试' });
  }
  next();
}

// 定期清理过期的限流记录，避免内存无限增长
setInterval(() => {
  const now = Date.now();

  for (const [ip, rec] of loginAttempts) {
    const expired = now - rec.firstAt > ATTEMPT_WINDOW;
    const unblocked = !rec.blockedUntil || now > rec.blockedUntil;
    if (expired && unblocked) loginAttempts.delete(ip);
  }
  for (const [ip, rec] of registerAttempts) {
    if (now - rec.windowStart > REGISTER_WINDOW) registerAttempts.delete(ip);
  }
  for (const [ip, rec] of requestCounts) {
    if (now - rec.windowStart > REQUEST_WINDOW) requestCounts.delete(ip);
  }
}, 5 * 60 * 1000);

const app = express();

// 前面挂着 Nginx，要信任 X-Forwarded-For 才能拿到真实客户端 IP
// （限流依赖它，否则所有请求都会被算成同一个 IP）
app.set('trust proxy', true);

app.use(securityHeaders);      // 安全响应头
app.use(rateLimitMiddleware);  // 全站请求限流
app.use(express.json({ limit: '100kb' })); // 解析 POST 的 JSON body（限制体积，防超大请求打满内存）

// ---------- API 文档 ----------
// 在线可调试： http://localhost:3000/api-docs
app.use('/api-docs', swaggerUi.serve, swaggerUi.setup(swaggerSpec, {
  customSiteTitle: '机械臂云控 API',
}));
// 原始 OpenAPI 规范，可导入 Postman / Apifox
app.get('/api-docs.json', (req, res) => res.json(swaggerSpec));

app.use(express.static(path.join(__dirname, 'public')));

// ---------- 账号：注册 / 登录 / 登出 ----------

/**
 * @openapi
 * /api/register:
 *   post:
 *     tags: [账号]
 *     summary: 注册账号
 *     description: |
 *       **第一个注册的账号自动成为管理员**，之后注册的都是普通用户。
 *
 *       密码用 scrypt 哈希后存储，数据库中不保存明文。
 *       注册成功后自动登录（写入会话 cookie）。
 *     requestBody:
 *       required: true
 *       content:
 *         application/json:
 *           schema:
 *             type: object
 *             required: [username, password, nickname]
 *             properties:
 *               username:
 *                 type: string
 *                 description: 3-32 位，字母/数字/下划线/连字符
 *                 example: qiudai
 *               password:
 *                 type: string
 *                 description: 至少 6 位
 *                 example: '你的密码'
 *               nickname:
 *                 type: string
 *                 description: 显示名称
 *                 example: 阿岱
 *     responses:
 *       200:
 *         description: 注册成功并已登录
 *       400:
 *         description: 参数不合法（用户名格式、密码长度等）
 *       409:
 *         description: 用户名已被占用
 */
app.post('/api/register', wrap(async (req, res) => {
  const ip = clientIp(req);

  if (!checkRegisterLimit(ip)) {
    return res.status(429).json({
      error: `注册过于频繁，同一网络每小时最多注册 ${REGISTER_PER_HOUR} 个账号`,
    });
  }

  const { username, password, nickname } = req.body || {};
  const account = await accounts.register(username, password, nickname);
  recordRegister(ip);

  const token = await accounts.createSession(account.id);
  res.setHeader('Set-Cookie',
    `${COOKIE_NAME}=${token}; Path=/; Max-Age=${SESSION_TTL / 1000}; HttpOnly; SameSite=Lax`);
  res.json({ ok: true, account });
}));

/**
 * @openapi
 * /api/login:
 *   post:
 *     tags: [账号]
 *     summary: 登录
 *     description: |
 *       登录成功后签发会话 token，写入 httpOnly cookie。
 *
 *       有频率限制：同一 IP 十分钟内失败 5 次，封禁 15 分钟。
 *     requestBody:
 *       required: true
 *       content:
 *         application/json:
 *           schema:
 *             type: object
 *             required: [username, password]
 *             properties:
 *               username:
 *                 type: string
 *                 example: qiudai
 *               password:
 *                 type: string
 *                 example: '你的密码'
 *     responses:
 *       200:
 *         description: 登录成功
 *       401:
 *         description: 用户名或密码错误
 *       429:
 *         description: 尝试过于频繁
 */
app.post('/api/login', wrap(async (req, res) => {
  const ip = clientIp(req);

  const limit = checkRateLimit(ip);
  if (!limit.allowed) {
    res.setHeader('Retry-After', limit.retryAfterSec);
    return res.status(429).json({
      error: `尝试过于频繁，请 ${Math.ceil(limit.retryAfterSec / 60)} 分钟后再试`,
    });
  }

  const { username, password } = req.body || {};
  let account;
  try {
    account = await accounts.login(username, password);
  } catch (e) {
    if (e instanceof accounts.InvalidCredentialsError) {
      recordFailure(ip);
      return res.status(401).json({ error: e.message });
    }
    throw e;
  }

  clearFailures(ip);
  const token = await accounts.createSession(account.id);
  res.setHeader('Set-Cookie',
    `${COOKIE_NAME}=${token}; Path=/; Max-Age=${SESSION_TTL / 1000}; HttpOnly; SameSite=Lax`);
  console.log(`✅ ${account.nickname}（${account.username}）登录了`);
  res.json({ ok: true, account });
}));

/**
 * @openapi
 * /api/logout:
 *   post:
 *     tags: [账号]
 *     summary: 退出登录
 *     responses:
 *       200:
 *         description: 已清除登录状态
 */
app.post('/api/logout', wrap(async (req, res) => {
  await accounts.deleteSession(currentToken(req));
  res.setHeader('Set-Cookie', `${COOKIE_NAME}=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax`);
  res.json({ ok: true });
}));

/**
 * @openapi
 * /api/me:
 *   get:
 *     tags: [账号]
 *     summary: 当前登录的账号
 *     responses:
 *       200:
 *         description: |
 *           未登录时返回 `{ authed: false }`；
 *           已登录返回 `{ authed: true, account: {...} }`
 */
app.get('/api/me', wrap(async (req, res) => {
  const account = await accounts.getSession(currentToken(req));
  if (!account) return res.json({ authed: false });
  res.json({ authed: true, account });
}));

// 兼容旧路径（前端曾用 /api/auth）
app.get('/api/auth', wrap(async (req, res) => {
  const account = await accounts.getSession(currentToken(req));
  res.json({ authed: !!account, account: account || null });
}));

/**
 * @openapi
 * /api/device:
 *   get:
 *     tags: [遥控]
 *     summary: 机械臂是否在线
 *     description: |
 *       ESP32 是否已连上服务器。控制页顶部的状态灯用它。
 *       这个接口不需要登录 —— 只是"在线/离线"一个比特，且首页也想展示。
 *     responses:
 *       200:
 *         description: |
 *           `{ online: true, since: "2026-09-20T..." }` 或 `{ online: false }`
 */
app.get('/api/device', (req, res) => {
  if (!esp32) return res.json({ online: false });
  res.json({
    online: true,
    since: new Date(esp32Since).toISOString(),
    onlineSeconds: Math.floor((Date.now() - esp32Since) / 1000),
  });
});

// ---------- 遥控命令：浏览器 -> ESP32 ----------
/**
 * @openapi
 * /set:
 *   get:
 *     tags: [遥控]
 *     summary: 设置单个舵机角度
 *     description: |
 *       命令经服务器通过 WebSocket 转发给 ESP32，再由 ESP32 驱动舵机。
 *
 *       ESP32 未连接时返回 503 —— 网页端和手势模块以这个状态码
 *       作为「机械臂不在线」的判断依据。
 *     parameters:
 *       - in: query
 *         name: servo
 *         required: true
 *         description: 舵机编号（0=底座 1=肩部 2=肘部 3=腕俯仰 4=腕旋转 5=夹爪）
 *         schema:
 *           type: integer
 *           minimum: 0
 *           maximum: 5
 *           example: 0
 *       - in: query
 *         name: angle
 *         required: true
 *         description: 目标角度（度）
 *         schema:
 *           type: integer
 *           minimum: 0
 *           maximum: 180
 *           example: 90
 *     responses:
 *       200:
 *         description: 设置成功，返回设置的角度
 *         content:
 *           text/plain:
 *             schema:
 *               type: string
 *               example: '90'
 *       401:
 *         description: 未登录（需先调用 /api/login）
 *         content:
 *           text/plain:
 *             schema:
 *               type: string
 *               example: 需要登录后才能控制机械臂
 *       503:
 *         description: ESP32 未连接
 *         content:
 *           text/plain:
 *             schema:
 *               type: string
 *               example: ESP32未连接
 */
app.get('/set', requireAuth, (req, res) => {
  const servo = req.query.servo;
  const angle = req.query.angle;
  const cmd = servo + ':' + angle;
  console.log('浏览器命令:', cmd);
  if (esp32) {
    esp32.send(cmd);
    res.send(angle);
  } else {
    res.status(503).send('ESP32未连接');
  }
});

// ---------- 留言 API ----------
/**
 * @openapi
 * /api/messages:
 *   get:
 *     tags: [留言板]
 *     summary: 获取留言列表
 *     description: |
 *       按时间倒序返回全部留言，每条带上它的回复。
 *
 *       实现上用「两条查询 + 内存归组」而不是「一条留言查一次回复」，
 *       避免 N+1 问题 —— 与留言条数无关，固定 2 次数据库查询。
 *     responses:
 *       200:
 *         description: 留言数组
 *         content:
 *           application/json:
 *             schema:
 *               type: array
 *               items:
 *                 $ref: '#/components/schemas/Message'
 */
app.get('/api/messages', requireAuth, wrap(async (req, res) => {
  res.json(await store.list());
}));

/**
 * @openapi
 * /api/messages:
 *   post:
 *     tags: [留言板]
 *     summary: 发表留言
 *     requestBody:
 *       required: true
 *       content:
 *         application/json:
 *           schema:
 *             type: object
 *             required: [nickname, content]
 *             properties:
 *               nickname:
 *                 type: string
 *                 maxLength: 50
 *                 example: 张三
 *               content:
 *                 type: string
 *                 maxLength: 500
 *                 example: 机械臂很酷
 *     responses:
 *       200:
 *         description: 创建成功，返回新留言
 *         content:
 *           application/json:
 *             schema:
 *               $ref: '#/components/schemas/Message'
 *       400:
 *         description: 昵称或内容为空
 *         content:
 *           application/json:
 *             schema:
 *               $ref: '#/components/schemas/Error'
 */
app.post('/api/messages', requireAuth, wrap(async (req, res) => {
  const { content } = req.body || {};
  if (!content || !String(content).trim()) {
    return res.status(400).json({ error: '留言内容不能为空' });
  }
  // 昵称取自登录账号，不让客户端随便传 —— 否则可以冒充任何人发言
  res.json(await store.add(req.account.nickname, String(content).trim()));
}));

/**
 * @openapi
 * /api/messages/{id}/reply:
 *   post:
 *     tags: [留言板]
 *     summary: 回复留言（管理员）
 *     description: |
 *       需要管理员身份 —— 先调用 `/api/login` 登录，
 *       之后本接口通过会话 cookie 鉴权，不再需要传密码。
 *     parameters:
 *       - in: path
 *         name: id
 *         required: true
 *         description: 留言 ID
 *         schema:
 *           type: integer
 *           example: 1
 *     requestBody:
 *       required: true
 *       content:
 *         application/json:
 *           schema:
 *             type: object
 *             required: [content]
 *             properties:
 *               content:
 *                 type: string
 *                 maxLength: 500
 *                 example: 谢谢支持！
 *     responses:
 *       200:
 *         description: 回复成功，返回更新后的留言
 *         content:
 *           application/json:
 *             schema:
 *               $ref: '#/components/schemas/Message'
 *       401:
 *         description: 未登录
 *         content:
 *           application/json:
 *             schema:
 *               $ref: '#/components/schemas/Error'
 *       404:
 *         description: 留言不存在
 *         content:
 *           application/json:
 *             schema:
 *               $ref: '#/components/schemas/Error'
 */
app.post('/api/messages/:id/reply', requireAdmin, wrap(async (req, res) => {
  const { content } = req.body || {};
  const m = await store.reply(Number(req.params.id), String(content || '').trim());
  if (!m) return res.status(404).json({ error: '留言不存在' });
  res.json(m);
}));

/**
 * @openapi
 * /api/messages/{id}:
 *   delete:
 *     tags: [留言板]
 *     summary: 删除留言（管理员）
 *     description: |
 *       需要管理员身份 —— 先调用 `/api/login` 登录，
 *       之后本接口通过会话 cookie 鉴权，不再需要传密码。
 *
 *       该留言下的回复会由数据库外键 `ON DELETE CASCADE` 自动清理，
 *       不会留下孤儿数据。
 *     parameters:
 *       - in: path
 *         name: id
 *         required: true
 *         description: 留言 ID
 *         schema:
 *           type: integer
 *           example: 1
 *     responses:
 *       200:
 *         description: 删除成功
 *         content:
 *           application/json:
 *             schema:
 *               type: object
 *               properties:
 *                 ok:
 *                   type: boolean
 *                   example: true
 *       401:
 *         description: 未登录
 *         content:
 *           application/json:
 *             schema:
 *               $ref: '#/components/schemas/Error'
 *       404:
 *         description: 留言不存在
 *         content:
 *           application/json:
 *             schema:
 *               $ref: '#/components/schemas/Error'
 */
app.delete('/api/messages/:id', requireAdmin, wrap(async (req, res) => {
  const ok = await store.remove(Number(req.params.id));
  if (!ok) return res.status(404).json({ error: '留言不存在' });
  res.json({ ok: true });
}));

// ---------- 页面（无后缀 URL）----------
// /control 是 Vue 3 单页应用，构建产物在 public/control/，
// 由上面的 express.static 直接处理（访问 /control 会自动跳到 /control/）
// 控制台两代版本
//   /control     → 新版（Vue 3，6 轴总线舵机，构建产物在 public/control/）
//   /control/v1  → 旧版（第一代：原生 JS，5 轴模拟舵机）
//   /control/v2  → 跳到新版（让两边的切换按钮对称好写）
app.get('/control/v1', (req, res) => res.sendFile(path.join(__dirname, 'public', 'control-v1.html')));
app.get('/control/v2', (req, res) => res.redirect('/control'));

app.get('/login', (req, res) => res.sendFile(path.join(__dirname, 'public', 'login.html')));
app.get('/register', (req, res) => res.sendFile(path.join(__dirname, 'public', 'login.html')));
app.get('/messages', (req, res) => res.sendFile(path.join(__dirname, 'public', 'messages.html')));
app.get('/download', (req, res) => res.sendFile(path.join(__dirname, 'public', 'download.html')));

// ---------- 统一错误处理 ----------
app.use((err, req, res, next) => {
  // 业务异常：把原始提示返回给用户，而不是笼统的 500
  if (err instanceof accounts.UsernameTakenError) {
    return res.status(409).json({ error: err.message });
  }
  if (err instanceof accounts.ValidationError) {
    return res.status(400).json({ error: err.message });
  }
  if (err instanceof accounts.AccountError) {
    return res.status(400).json({ error: err.message });
  }

  console.error('请求出错:', err.message);
  res.status(500).json({ error: '服务器内部错误' });
});

// ---------- WebSocket（同一台 http server）----------
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

wss.on('connection', (ws, req) => {
  // 校验连接地址里带的暗号，如 ws://IP/?token=xxx
  const url = new URL(req.url, 'http://localhost');
  const token = url.searchParams.get('token');
  if (token !== config.ESP32_TOKEN) {
    console.log('❌ 有设备尝试连接，但 token 错误，已拒绝');
    ws.close();
    return;
  }
  console.log('✅ ESP32 认证通过');

  // 新连接直接顶掉旧的 —— 不要用 if (!esp32) 判断。
  //
  // 设备重连时，旧连接往往还没被 TCP 检测为断开，于是新连接会被忽略；
  // 等旧连接随后真正关闭，esp32 被清空 —— 服务器以为设备离线，
  // 实际上它正连着。用户看到的就是"聊着聊着突然断开"。
  // （实际踩到：机械臂控制到一半，状态灯变灰，但设备其实还在线）
  if (esp32 && esp32 !== ws) {
    console.log('（旧的 ESP32 连接还在，用新连接替换）');
    try { esp32.terminate(); } catch { /* 已经关了就忽略 */ }
  }

  esp32 = ws;
  esp32Since = Date.now();
  console.log('✅ 这是 ESP32，已登记');

  ws.on('message', (data) => console.log('ESP32 回报:', data.toString()));
  ws.on('close', () => {
    if (esp32 === ws) {
      esp32 = null;
      esp32Since = null;
      console.log('ESP32 断开');
    }
  });
});

// ---------- 启动 ----------
// 先确认数据库能连上，再开始收请求 —— 免得跑起来才发现配置错了
db.check()
  .then(() => {
    console.log('✅ 数据库连接正常');
    // 只监听 127.0.0.1 —— 外部访问一律经 Nginx 转发。
    //
    // 默认的 listen(PORT) 会绑到所有网卡，意味着如果有人把云安全组的
    // 3000 端口放开，就能绕过 Nginx 直连后端 —— 限流、安全响应头、
    // 隐藏文件规则全部失效。绑本机是纵深防御的第二层。
    server.listen(config.PORT, '127.0.0.1', () => {
      console.log('服务器运行在 http://127.0.0.1:' + config.PORT + '（仅本机，外部经 Nginx）');
    });
  })
  .catch((err) => {
    console.error('❌ 连不上数据库：', err.message);
    console.error('   1) 确认 MySQL 已启动：brew services start mysql');
    console.error('   2) 确认建过表：mysql -u root < server/schema.sql');
    process.exit(1);
  });
