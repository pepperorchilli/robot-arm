// MySQL 连接池
//
// 用连接池而不是单条连接：每次请求都新建连接开销很大（TCP 握手 + 认证），
// 连接池把连接复用起来，并且限制了最大并发连接数，避免把数据库拖垮。
const mysql = require('mysql2/promise');
const config = require('./config');

const pool = mysql.createPool({
  ...config.DB,

  waitForConnections: true,
  connectionLimit: 10,   // 池里最多 10 条连接
  queueLimit: 0,         // 超出时排队，不直接报错
  charset: 'utf8mb4',

  // DATETIME 让 mysql2 转成 JS Date 对象（默认行为），
  // 我们在 store.js 里再统一转成毫秒时间戳给前端
});

// 启动时探活一次，配置错了能立刻发现，而不是等第一个请求才报错
async function check() {
  const conn = await pool.getConnection();
  try {
    await conn.query('SELECT 1');
    return true;
  } finally {
    conn.release();   // 必须归还，否则池会被耗尽
  }
}

module.exports = { pool, check };
