const http = require('http');
const WebSocket = require('ws');

let esp32 = null;

const HTML = `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>机械臂遥控</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#1a1a2e;min-height:100vh;color:#fff;padding:16px}
.container{max-width:500px;margin:0 auto}
h1{text-align:center;font-size:20px;margin-bottom:2px}
.subtitle{text-align:center;font-size:13px;color:rgba(255,255,255,0.5);margin-bottom:16px}
.c{background:rgba(255,255,255,0.06);border-radius:16px;padding:14px;margin-bottom:8px}
.h{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}
.n{font-size:14px;font-weight:600}
.v{font-size:28px;font-weight:700;color:#00e5ff}
.p{display:flex;gap:5px;margin-bottom:6px}
.p button{flex:1;padding:6px 0;border:none;border-radius:8px;background:rgba(255,255,255,0.1);color:#fff;font-size:11px;cursor:pointer}
.p button.on{background:#00e5ff;color:#1a1a2e}
.f{display:flex;justify-content:center;gap:6px}
.f button{width:30px;height:30px;border-radius:50%;border:none;background:rgba(255,255,255,0.1);color:#fff;font-size:14px;cursor:pointer}
input[type=range]{-webkit-appearance:none;width:100%;height:6px;border-radius:3px;background:rgba(255,255,255,0.15);outline:none;margin-bottom:6px}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:26px;height:26px;border-radius:50%;background:#00e5ff;cursor:pointer;border:2px solid #fff}
.r{width:100%;padding:12px;border:none;border-radius:12px;background:linear-gradient(135deg,#ff6b6b,#ee5a24);color:#fff;font-size:15px;font-weight:700;cursor:pointer;margin-top:6px;margin-bottom:16px}
.s{text-align:center;font-size:12px;color:rgba(255,255,255,0.4)}
</style>
</head>
<body>
<div class="container">
<h1>🤖 机械臂遥控</h1><p class="subtitle" id="status">连接中...</p>
<div id="cards"></div>
<button class="r" onclick="reset()">🏠 一键复位</button>
<div class="s">云端 · WebSocket 中转</div>
</div>
<script>
const NAMES=['底座','大臂','小臂','手腕','夹爪'];
const N=5;
let last=[90,90,90,90,90];

function card(i){
  return '<div class="c"><div class="h"><div class="n">'+NAMES[i]+'</div><div class="v" id="a'+i+'">90°</div></div>'
    +'<input type="range" id="s'+i+'" min="0" max="180" value="90">'
    +'<div class="p"><button onclick="go('+i+',0)">0°</button><button onclick="go('+i+',45)">45°</button><button onclick="go('+i+',90)" class="on">90°</button><button onclick="go('+i+',135)">135°</button><button onclick="go('+i+',180)">180°</button></div>'
    +'<div class="f"><button onclick="adj('+i+',-5)">-5</button><button onclick="adj('+i+',-1)">-1</button><button onclick="adj('+i+',1)">+1</button><button onclick="adj('+i+',5)">+5</button></div></div>';
}

let html='';
for(let i=0;i<N;i++)html+=card(i);
document.getElementById('cards').innerHTML=html;

function send(i,a){
  if(a===last[i])return;
  last[i]=a;
  fetch('/set?servo='+i+'&angle='+a)
    .then(r=>r.text())
    .then(t=>{document.getElementById('a'+i).textContent=t+'°';})
    .catch(e=>{document.getElementById('status').textContent='服务器无响应';});
}

for(let i=0;i<N;i++){
  let s=document.getElementById('s'+i);
  s.addEventListener('input',()=>{document.getElementById('a'+i).textContent=s.value+'°';});
  s.addEventListener('change',()=>send(i,+s.value));
}

function go(i,a){document.getElementById('s'+i).value=a;document.getElementById('a'+i).textContent=a+'°';send(i,a);}
function adj(i,d){let cur=parseInt(document.getElementById('a'+i).textContent);let a=Math.min(180,Math.max(0,cur+d));go(i,a);}
async function reset(){for(let i=0;i<N;i++){go(i,90);await new Promise(r=>setTimeout(r,150));}}
</script>
</body>
</html>`;

const server = http.createServer((req, res) => {
  if (req.url.startsWith('/set')) {
    const url = new URL(req.url, 'http://localhost');
    const servo = url.searchParams.get('servo');
    const angle = url.searchParams.get('angle');
    const cmd = servo + ':' + angle;
    console.log('浏览器命令:', cmd);
    if (esp32) {
      esp32.send(cmd);
      res.writeHead(200, {'Content-Type': 'text/plain'});
      res.end(angle);
    } else {
      res.writeHead(503, {'Content-Type': 'text/plain'});
      res.end('ESP32未连接');
    }
  } else if (req.url === '/' || req.url.startsWith('/?')) {
    res.writeHead(200, {'Content-Type': 'text/html; charset=utf-8'});
    res.end(HTML);
  } else {
    res.writeHead(404, {'Content-Type': 'text/plain'});
    res.end('Not Found');
  }
});

const wss = new WebSocket.Server({ server });

wss.on('connection', (ws) => {
  console.log('有设备连进来了');
  if (!esp32) {
    esp32 = ws;
    console.log('✅ 这是 ESP32，已登记');
    ws.on('message', (data) => {
      console.log('ESP32 回报:', data.toString());
    });
  }
});

server.listen(80, () => {
  console.log('服务器运行在 80 端口');
});
