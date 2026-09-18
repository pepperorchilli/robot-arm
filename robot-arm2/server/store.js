// 留言存储：MySQL 版
//
// 对外暴露的接口和原来的 JSON 文件版完全一致（list / add / reply / remove），
// 只是都变成了 async —— 调用方要 await。
const { pool } = require('./db');

// ---------------------------------------------------------------
// 把数据库行转成前端要的结构
// ---------------------------------------------------------------
// 前端用的是 time（毫秒时间戳），这里统一转换，接口形状保持不变
function toReply(row) {
  return {
    content: row.content,
    time: row.created_at.getTime(),
  };
}

function toMessage(row, replies) {
  return {
    id: Number(row.id),
    nickname: row.nickname,
    content: row.content,
    time: row.created_at.getTime(),
    replies: replies.map(toReply),
  };
}

// ---------------------------------------------------------------
// 查：留言列表（含各自的回复）
// ---------------------------------------------------------------
async function list() {
  // 第 1 条查询：拿全部留言（走 idx_created_at 索引，不用 filesort）
  const [messages] = await pool.query(
    'SELECT id, nickname, content, created_at FROM messages ORDER BY created_at DESC, id DESC'
  );
  if (messages.length === 0) return [];

  // 第 2 条查询：一次性拿回所有回复
  //
  // ⚠️ 这里是最容易踩的坑（N+1 查询）：
  //    如果写成 for (const m of messages) { 查一次 replies }
  //    那么 30 条留言就要查 1 + 30 = 31 次数据库。
  //    用 IN 一次性查完，总共只要 2 次，与留言条数无关。
  const ids = messages.map((m) => m.id);
  const [replies] = await pool.query(
    'SELECT message_id, content, created_at FROM replies WHERE message_id IN (?) ORDER BY created_at ASC',
    [ids]
  );

  // 在内存里按 message_id 归组
  const grouped = new Map();
  for (const r of replies) {
    const key = Number(r.message_id);
    if (!grouped.has(key)) grouped.set(key, []);
    grouped.get(key).push(r);
  }

  return messages.map((m) => toMessage(m, grouped.get(Number(m.id)) || []));
}

// ---------------------------------------------------------------
// 增：发留言
// ---------------------------------------------------------------
async function add(nickname, content) {
  const [result] = await pool.query(
    'INSERT INTO messages (nickname, content) VALUES (?, ?)',
    [nickname, content]
  );
  const [rows] = await pool.query(
    'SELECT id, nickname, content, created_at FROM messages WHERE id = ?',
    [result.insertId]
  );
  return toMessage(rows[0], []);
}

// ---------------------------------------------------------------
// 增：博主回复
// ---------------------------------------------------------------
async function reply(id, content) {
  // 先确认留言存在（否则外键会直接报错，错误信息不友好）
  const [exists] = await pool.query('SELECT id FROM messages WHERE id = ?', [id]);
  if (exists.length === 0) return null;

  await pool.query(
    'INSERT INTO replies (message_id, content) VALUES (?, ?)',
    [id, content]
  );
  return getOne(id);
}

// ---------------------------------------------------------------
// 删：删除留言（它的回复由外键 ON DELETE CASCADE 自动删掉）
// ---------------------------------------------------------------
async function remove(id) {
  const [result] = await pool.query('DELETE FROM messages WHERE id = ?', [id]);
  return result.affectedRows > 0;
}

// ---------------------------------------------------------------
// 查：单条留言
// ---------------------------------------------------------------
async function getOne(id) {
  const [messages] = await pool.query(
    'SELECT id, nickname, content, created_at FROM messages WHERE id = ?',
    [id]
  );
  if (messages.length === 0) return null;

  const [replies] = await pool.query(
    'SELECT content, created_at FROM replies WHERE message_id = ? ORDER BY created_at ASC',
    [id]
  );
  return toMessage(messages[0], replies);
}

module.exports = { list, add, reply, remove, getOne };
