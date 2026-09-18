// 机械臂云控制服务器 + 产品网站
//   - WebSocket：ESP32 连进来，浏览器命令转发给它
//   - Express：静态网站（首页/遥控/留言/下载）+ 留言 API（数据存 MySQL）
const http = require('http');
const path = require('path');
const express = require('express');
const WebSocket = require('ws');
const config = require('./config');
const store = require('./store');
const db = require('./db');
const swaggerSpec = require('./swagger');
const swaggerUi = require('swagger-ui-express');

let esp32 = null; // 当前连进来的 ESP32（只存一台）

const app = express();
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
 *       503:
 *         description: ESP32 未连接
 *         content:
 *           text/plain:
 *             schema:
 *               type: string
 *               example: ESP32未连接
 */
app.get('/set', (req, res) => {
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
