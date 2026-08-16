// 机械臂云控制服务器 + 产品网站
//   - WebSocket：ESP32 连进来，浏览器命令转发给它
//   - Express：静态网站（首页/遥控/留言/下载）+ 留言 API
const http = require('http');
const path = require('path');
const express = require('express');
const WebSocket = require('ws');
const config = require('./config');
const store = require('./store');

let esp32 = null; // 当前连进来的 ESP32（只存一台）

const app = express();
app.use(express.json()); // 解析 POST 的 JSON body
app.use(express.static(path.join(__dirname, 'public')));

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
app.get('/api/messages', (req, res) => {
  res.json(store.list());
});

app.post('/api/messages', (req, res) => {
  const { nickname, content } = req.body || {};
  if (!nickname || !content) {
    return res.status(400).json({ error: '昵称和内容不能为空' });
  }
  const m = store.add(String(nickname).trim(), String(content).trim());
  res.json(m);
});

app.post('/api/messages/:id/reply', (req, res) => {
  const { content, password } = req.body || {};
  if (password !== config.ADMIN_PASSWORD) {
    return res.status(401).json({ error: '密码错误' });
  }
  const m = store.reply(Number(req.params.id), String(content || '').trim());
  if (!m) return res.status(404).json({ error: '留言不存在' });
  res.json(m);
});

app.delete('/api/messages/:id', (req, res) => {
  const { password } = req.body || {};
  if (password !== config.ADMIN_PASSWORD) {
    return res.status(401).json({ error: '密码错误' });
  }
  const ok = store.remove(Number(req.params.id));
  if (!ok) return res.status(404).json({ error: '留言不存在' });
  res.json({ ok: true });
});

// ---------- 页面（无后缀 URL）----------
app.get('/control', (req, res) => res.sendFile(path.join(__dirname, 'public', 'control.html')));
app.get('/messages', (req, res) => res.sendFile(path.join(__dirname, 'public', 'messages.html')));
app.get('/download', (req, res) => res.sendFile(path.join(__dirname, 'public', 'download.html')));

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

server.listen(config.PORT, () => {
  console.log('服务器运行在 http://localhost:' + config.PORT);
});
