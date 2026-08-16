// 留言存储：用 JSON 文件保存，重启不丢
const fs = require('fs');
const path = require('path');

const DATA_DIR = path.join(__dirname, 'data');
const FILE = path.join(DATA_DIR, 'messages.json');

let cache = null; // 读进内存，写时回盘

function load() {
  if (cache) return cache;
  try {
    cache = JSON.parse(fs.readFileSync(FILE, 'utf8'));
  } catch (e) {
    cache = [];
  }
  return cache;
}

function save() {
  fs.mkdirSync(DATA_DIR, { recursive: true });
  fs.writeFileSync(FILE, JSON.stringify(cache, null, 2));
}

function list() {
  return load();
}

// 新留言放到最前面
function add(nickname, content) {
  const m = {
    id: Date.now(),
    nickname,
    content,
    time: Date.now(),
    replies: [],
  };
  load().unshift(m);
  save();
  return m;
}

function reply(id, content) {
  const m = load().find((x) => x.id === id);
  if (!m) return null;
  m.replies.push({ content, time: Date.now() });
  save();
  return m;
}

function remove(id) {
  const list = load();
  const i = list.findIndex((x) => x.id === id);
  if (i < 0) return false;
  list.splice(i, 1);
  save();
  return true;
}

module.exports = { list, add, reply, remove };
