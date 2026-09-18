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

let esp32 = null; // 当前连进来的 ESP32（只存一台）

const app = express();
app.use(express.json()); // 解析 POST 的 JSON body
app.use(express.static(path.join(__dirname, 'public')));

// 把 async 路由的错误统一接住，避免一个异常把整个进程带崩
const wrap = (fn) => (req, res, next) => Promise.resolve(fn(req, res, next)).catch(next);

// ---------- 遥控命令：浏览器 -> ESP32 ----------
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
app.get('/api/messages', wrap(async (req, res) => {
  res.json(await store.list());
}));

app.post('/api/messages', wrap(async (req, res) => {
  const { nickname, content } = req.body || {};
  if (!nickname || !content) {
    return res.status(400).json({ error: '昵称和内容不能为空' });
  }
  res.json(await store.add(String(nickname).trim(), String(content).trim()));
}));

app.post('/api/messages/:id/reply', wrap(async (req, res) => {
  const { content, password } = req.body || {};
  if (password !== config.ADMIN_PASSWORD) {
    return res.status(401).json({ error: '密码错误' });
  }
  const m = await store.reply(Number(req.params.id), String(content || '').trim());
  if (!m) return res.status(404).json({ error: '留言不存在' });
  res.json(m);
}));

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
