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

let esp32 = null; // 当前连进来的 ESP32（只存一台）

// ==================== 遥控鉴权 ====================
//
// 机械臂控制页是公开的（谁都能看到），但**控制需要登录**——
// 否则任何人都能把你的舵机玩坏。
//
// 方案：密码登录 → 签发随机 token → 存内存 + 写 httpOnly cookie。
// 用内存存而不是签名的无状态 token，好处是重启即失效、且可以主动踢人；
// 代价是服务重启后需要重新登录（对这个场景可以接受）。

const sessions = new Map();                 // token -> 过期时间戳
const SESSION_TTL = 7 * 24 * 3600 * 1000;   // 7 天
const COOKIE_NAME = 'arm_token';

function issueToken() {
  const token = crypto.randomBytes(24).toString('hex');
  sessions.set(token, Date.now() + SESSION_TTL);
  return token;
}

function isValidToken(token) {
  if (!token) return false;
  const expireAt = sessions.get(token);
  if (!expireAt) return false;
  if (Date.now() > expireAt) {
    sessions.delete(token);
    return false;
  }
  return true;
}

// 定期清理过期 token，避免内存无限增长
setInterval(() => {
  const now = Date.now();
  for (const [token, expireAt] of sessions) {
    if (now > expireAt) sessions.delete(token);
  }
}, 3600 * 1000);

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

/** 鉴权中间件：cookie 或 X-Auth-Token 头里有有效 token 才放行 */
function requireAuth(req, res, next) {
  const token = parseCookie(req, COOKIE_NAME) || req.get('X-Auth-Token');
  if (isValidToken(token)) return next();
  res.status(401).send('需要登录后才能控制机械臂');
}

// ==================== 登录限流 ====================
//
// 登录密码可能设得比较短，而接口暴露在公网 —— 没有限流的话，
// 脚本每秒能试几千次，短密码几分钟就被爆破。
//
// 策略：同一 IP 在 10 分钟内失败 5 次，封禁 15 分钟。
// 存内存即可（重启清空，对个人站点够用）。

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

// 定期清理过期记录，避免内存无限增长
setInterval(() => {
  const now = Date.now();
  for (const [ip, rec] of loginAttempts) {
    const expired = now - rec.firstAt > ATTEMPT_WINDOW;
    const unblocked = !rec.blockedUntil || now > rec.blockedUntil;
    if (expired && unblocked) loginAttempts.delete(ip);
  }
}, 5 * 60 * 1000);

const app = express();

// 前面挂着 Nginx，要信任 X-Forwarded-For 才能拿到真实客户端 IP
// （登录限流依赖它，否则所有请求都会被算成同一个 IP）
app.set('trust proxy', true);

app.use(express.json()); // 解析 POST 的 JSON body

// ---------- API 文档 ----------
// 在线可调试： http://localhost:3000/api-docs
app.use('/api-docs', swaggerUi.serve, swaggerUi.setup(swaggerSpec, {
  customSiteTitle: '机械臂云控 API',
}));
// 原始 OpenAPI 规范，可导入 Postman / Apifox
app.get('/api-docs.json', (req, res) => res.json(swaggerSpec));

app.use(express.static(path.join(__dirname, 'public')));

// 把 async 路由的错误统一接住，避免一个异常把整个进程带崩
const wrap = (fn) => (req, res, next) => Promise.resolve(fn(req, res, next)).catch(next);

// ---------- 登录 ----------
/**
 * @openapi
 * /api/login:
 *   post:
 *     tags: [遥控]
 *     summary: 登录（控制机械臂需要）
 *     description: |
 *       机械臂控制页公开可见，但**控制需要登录**。
 *       登录成功后服务器签发随机 token，写入 httpOnly cookie。
 *     requestBody:
 *       required: true
 *       content:
 *         application/json:
 *           schema:
 *             type: object
 *             required: [password]
 *             properties:
 *               password:
 *                 type: string
 *                 example: '你的管理员密码'
 *     responses:
 *       200:
 *         description: 登录成功，已写入 cookie
 *       401:
 *         description: 密码错误
 */
app.post('/api/login', (req, res) => {
  const ip = clientIp(req);

  // 先看是否已被封禁
  const limit = checkRateLimit(ip);
  if (!limit.allowed) {
    res.setHeader('Retry-After', limit.retryAfterSec);
    return res.status(429).json({
      error: `尝试过于频繁，请 ${Math.ceil(limit.retryAfterSec / 60)} 分钟后再试`,
    });
  }

  const { password } = req.body || {};
  if (password !== config.ADMIN_PASSWORD) {
    recordFailure(ip);
    return res.status(401).json({ error: '密码错误' });
  }

  clearFailures(ip);   // 登录成功，清空失败计数
  const token = issueToken();
  // httpOnly：JS 读不到，防 XSS 窃取；sameSite=lax：防 CSRF
  res.setHeader('Set-Cookie',
    `${COOKIE_NAME}=${token}; Path=/; Max-Age=${SESSION_TTL / 1000}; HttpOnly; SameSite=Lax`);
  console.log('✅ 有人登录了遥控台');
  res.json({ ok: true });
});

/**
 * @openapi
 * /api/logout:
 *   post:
 *     tags: [遥控]
 *     summary: 退出登录
 *     responses:
 *       200:
 *         description: 已清除登录状态
 */
app.post('/api/logout', (req, res) => {
  const token = parseCookie(req, COOKIE_NAME);
  if (token) sessions.delete(token);
  res.setHeader('Set-Cookie', `${COOKIE_NAME}=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax`);
  res.json({ ok: true });
});

/**
 * @openapi
 * /api/auth:
 *   get:
 *     tags: [遥控]
 *     summary: 查询当前登录状态
 *     responses:
 *       200:
 *         description: 返回 { authed: true/false }
 */
app.get('/api/auth', (req, res) => {
  const token = parseCookie(req, COOKIE_NAME) || req.get('X-Auth-Token');
  res.json({ authed: isValidToken(token) });
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
app.get('/api/messages', wrap(async (req, res) => {
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
app.post('/api/messages', wrap(async (req, res) => {
  const { nickname, content } = req.body || {};
  if (!nickname || !content) {
    return res.status(400).json({ error: '昵称和内容不能为空' });
  }
  res.json(await store.add(String(nickname).trim(), String(content).trim()));
}));

/**
 * @openapi
 * /api/messages/{id}/reply:
 *   post:
 *     tags: [留言板]
 *     summary: 回复留言（博主）
 *     description: 需要管理员密码，密码错误返回 401。
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
 *             required: [content, password]
 *             properties:
 *               content:
 *                 type: string
 *                 maxLength: 500
 *                 example: 谢谢支持！
 *               password:
 *                 type: string
 *                 example: '123456'
 *     responses:
 *       200:
 *         description: 回复成功，返回更新后的留言
 *         content:
 *           application/json:
 *             schema:
 *               $ref: '#/components/schemas/Message'
 *       401:
 *         description: 密码错误
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
app.post('/api/messages/:id/reply', wrap(async (req, res) => {
  const { content, password } = req.body || {};
  if (password !== config.ADMIN_PASSWORD) {
    return res.status(401).json({ error: '密码错误' });
  }
  const m = await store.reply(Number(req.params.id), String(content || '').trim());
  if (!m) return res.status(404).json({ error: '留言不存在' });
  res.json(m);
}));

/**
 * @openapi
 * /api/messages/{id}:
 *   delete:
 *     tags: [留言板]
 *     summary: 删除留言（博主）
 *     description: |
 *       需要管理员密码。
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
 *     requestBody:
 *       required: true
 *       content:
 *         application/json:
 *           schema:
 *             type: object
 *             required: [password]
 *             properties:
 *               password:
 *                 type: string
 *                 example: '123456'
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
 *         description: 密码错误
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
app.delete('/api/messages/:id', wrap(async (req, res) => {
  const { password } = req.body || {};
  if (password !== config.ADMIN_PASSWORD) {
    return res.status(401).json({ error: '密码错误' });
  }
  const ok = await store.remove(Number(req.params.id));
  if (!ok) return res.status(404).json({ error: '留言不存在' });
  res.json({ ok: true });
}));

// ---------- 页面（无后缀 URL）----------
// /control 是 Vue 3 单页应用，构建产物在 public/control/，
// 由上面的 express.static 直接处理（访问 /control 会自动跳到 /control/）
app.get('/messages', (req, res) => res.sendFile(path.join(__dirname, 'public', 'messages.html')));
app.get('/download', (req, res) => res.sendFile(path.join(__dirname, 'public', 'download.html')));

// ---------- 统一错误处理 ----------
app.use((err, req, res, next) => {
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
  if (!esp32) {
    esp32 = ws;
    console.log('✅ 这是 ESP32，已登记');
    ws.on('message', (data) => console.log('ESP32 回报:', data.toString()));
    ws.on('close', () => {
      if (esp32 === ws) esp32 = null;
      console.log('ESP32 断开');
    });
  }
});

// ---------- 启动 ----------
// 先确认数据库能连上，再开始收请求 —— 免得跑起来才发现配置错了
db.check()
  .then(() => {
    console.log('✅ 数据库连接正常');
    server.listen(config.PORT, () => {
      console.log('服务器运行在 http://localhost:' + config.PORT);
    });
  })
  .catch((err) => {
    console.error('❌ 连不上数据库：', err.message);
    console.error('   1) 确认 MySQL 已启动：brew services start mysql');
    console.error('   2) 确认建过表：mysql -u root < server/schema.sql');
    process.exit(1);
  });
