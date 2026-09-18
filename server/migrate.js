// 数据迁移：把旧的 messages.json 导入 MySQL
//
// 执行： node migrate.js
// 幂等：重复执行不会产生重复数据（用 INSERT IGNORE 按原 id 跳过已存在的）
const fs = require('fs');
const path = require('path');
const { pool } = require('./db');

const FILE = path.join(__dirname, 'data', 'messages.json');

async function main() {
  if (!fs.existsSync(FILE)) {
    console.log(`没有找到 ${FILE}，无需迁移。`);
    return;
  }

  const raw = JSON.parse(fs.readFileSync(FILE, 'utf8'));
  console.log(`读到 ${raw.length} 条旧留言，开始迁移…`);

  let msgOk = 0;
  let replyOk = 0;

  for (const m of raw) {
    // 保留原来的 id（原来是 Date.now() 时间戳），这样前端的链接不会失效
    const [res] = await pool.query(
      'INSERT IGNORE INTO messages (id, nickname, content, created_at) VALUES (?, ?, ?, ?)',
      [m.id, m.nickname, m.content, new Date(m.time)]
    );
    if (res.affectedRows > 0) msgOk++;

    for (const r of m.replies || []) {
      await pool.query(
        'INSERT INTO replies (message_id, content, created_at) VALUES (?, ?, ?)',
        [m.id, r.content, new Date(r.time)]
      );
      replyOk++;
    }
  }

  console.log(`完成：新增留言 ${msgOk} 条，回复 ${replyOk} 条。`);
  console.log('（原文件保留未删，确认无误后可自行删除 server/data/messages.json）');
}

main()
  .catch((e) => {
    console.error('迁移失败：', e.message);
    process.exitCode = 1;
  })
  .finally(() => pool.end());
