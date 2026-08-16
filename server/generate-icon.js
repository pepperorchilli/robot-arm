// 生成 PWA 图标（icon-192.png / icon-512.png），纯 Node 无依赖
// 用法：node generate-icon.js
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

let TABLE = null;
function crc32(buf) {
  if (!TABLE) {
    TABLE = new Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
      TABLE[n] = c >>> 0;
    }
  }
  let c = 0xFFFFFFFF;
  for (let i = 0; i < buf.length; i++) c = TABLE[(c ^ buf[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length, 0);
  const t = Buffer.from(type, 'ascii');
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(Buffer.concat([t, data])), 0);
  return Buffer.concat([len, t, data, crc]);
}

function makeIcon(size) {
  const w = size, h = size;
  const px = Buffer.alloc(w * h * 4);
  const BG = [26, 26, 46, 255], FG = [0, 229, 255, 255], EYE = [26, 26, 46, 255];

  for (let i = 0; i < w * h; i++) {
    px[i * 4] = BG[0]; px[i * 4 + 1] = BG[1]; px[i * 4 + 2] = BG[2]; px[i * 4 + 3] = BG[3];
  }
  function set(x, y, c) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    const i = (y * w + x) * 4;
    px[i] = c[0]; px[i + 1] = c[1]; px[i + 2] = c[2]; px[i + 3] = c[3];
  }
  function circle(cx, cy, r, c) {
    for (let y = Math.floor(cy - r); y <= cy + r; y++)
      for (let x = Math.floor(cx - r); x <= cx + r; x++) {
        const dx = x - cx, dy = y - cy;
        if (dx * dx + dy * dy <= r * r) set(x, y, c);
      }
  }
  function rect(x0, y0, x1, y1, c) {
    for (let y = Math.floor(y0); y <= y1; y++)
      for (let x = Math.floor(x0); x <= x1; x++) set(x, y, c);
  }

  const s = size;
  // 天线
  rect(s * 0.48, s * 0.10, s * 0.52, s * 0.20, FG);
  circle(s * 0.5, s * 0.08, s * 0.035, FG);
  // 头
  circle(s * 0.5, s * 0.38, s * 0.19, FG);
  // 眼睛
  circle(s * 0.42, s * 0.36, s * 0.05, EYE);
  circle(s * 0.58, s * 0.36, s * 0.05, EYE);
  // 身体
  rect(s * 0.30, s * 0.58, s * 0.70, s * 0.82, FG);

  const raw = Buffer.alloc(h * (1 + w * 4));
  for (let y = 0; y < h; y++) {
    raw[y * (1 + w * 4)] = 0; // filter 类型：None
    px.copy(raw, y * (1 + w * 4) + 1, y * w * 4, (y + 1) * w * 4);
  }

  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0);
  ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8;  // 位深
  ihdr[9] = 6;  // 颜色类型 RGBA
  // 其余压缩/滤波/隔行均为 0

  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
    chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw)),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

const out = path.join(__dirname, 'public');
fs.mkdirSync(out, { recursive: true });
fs.writeFileSync(path.join(out, 'icon-192.png'), makeIcon(192));
fs.writeFileSync(path.join(out, 'icon-512.png'), makeIcon(512));
console.log('✅ 图标已生成：public/icon-192.png、public/icon-512.png');
