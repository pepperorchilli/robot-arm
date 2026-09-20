// 账号与会话
//
// 设计要点：
//
// 1. **密码只存哈希**，用 Node 内置的 scrypt（不引第三方依赖）。
//    格式：scrypt$<盐的hex>$<密钥的hex>
//
// 2. **会话存数据库而不是进程内存** ——
//    这样 Python 写的图书管理服务能直接查同一张表校验 token，
//    全站一套账号，不需要服务间调用。
//
// 3. **第一个注册的账号自动成为管理员**，之后注册的都是普通用户。

const crypto = require('crypto');
const { pool } = require('./db');

// ---------- 密码哈希 ----------

// scrypt 参数：N=16384 是常用的平衡点，单次约几十毫秒，
// 对正常登录无感，但让暴力破解的成本高到不可行。
const SCRYPT_N = 16384;
const SCRYPT_R = 8;
const SCRYPT_P = 1;
const KEY_LEN = 64;

function hashPassword(password) {
  const salt = crypto.randomBytes(16);
  const key = crypto.scryptSync(password, salt, KEY_LEN, {
    N: SCRYPT_N, r: SCRYPT_R, p: SCRYPT_P,
  });
  return `scrypt$${salt.toString('hex')}$${key.toString('hex')}`;
}

function verifyPassword(password, stored) {
  const parts = String(stored || '').split('$');
  if (parts.length !== 3 || parts[0] !== 'scrypt') return false;

  const salt = Buffer.from(parts[1], 'hex');
  const expected = Buffer.from(parts[2], 'hex');

  const actual = crypto.scryptSync(password, salt, expected.length, {
    N: SCRYPT_N, r: SCRYPT_R, p: SCRYPT_P,
  });

  // 恒定时间比较，避免通过响应时间推断密码
  return crypto.timingSafeEqual(expected, actual);
}

// ---------- 会话 ----------

const SESSION_TTL_MS = 30 * 24 * 3600 * 1000;   // 30 天

async function createSession(accountId) {
  const token = crypto.randomBytes(24).toString('hex');   // 48 字符
  const expireAt = new Date(Date.now() + SESSION_TTL_MS);
  await pool.query(
    'INSERT INTO sessions (token, account_id, expire_at) VALUES (?, ?, ?)',
    [token, accountId, expireAt]
  );
  return token;
}

/** 校验 token，有效则返回账号信息，否则 null */
async function getSession(token) {
  if (!token) return null;
  const [rows] = await pool.query(
    `SELECT a.id, a.username, a.nickname, a.role
       FROM sessions s
       JOIN accounts a ON a.id = s.account_id
      WHERE s.token = ? AND s.expire_at > NOW()`,
    [token]
  );
  return rows.length ? rows[0] : null;
}

async function deleteSession(token) {
  if (!token) return;
  await pool.query('DELETE FROM sessions WHERE token = ?', [token]);
}

/** 清理过期会话（由 server.js 定时调用） */
async function cleanupExpired() {
  const [r] = await pool.query('DELETE FROM sessions WHERE expire_at <= NOW()');
  return r.affectedRows;
}

// ---------- 注册 / 登录 ----------

class AccountError extends Error {}
class UsernameTakenError extends AccountError {}
class InvalidCredentialsError extends AccountError {}
class ValidationError extends AccountError {}

const USERNAME_RE = /^[A-Za-z0-9_-]{3,32}$/;

/** 当前账号总数（用来判断是不是第一个注册的） */
async function countAccounts() {
  const [[{ n }]] = await pool.query('SELECT COUNT(*) AS n FROM accounts');
  return n;
}

async function register(username, password, nickname) {
  username = String(username || '').trim();
  nickname = String(nickname || '').trim();
  password = String(password || '');

  if (!USERNAME_RE.test(username)) {
    throw new ValidationError('用户名需 3-32 位，只能用字母、数字、下划线、连字符');
  }
  if (!nickname) {
    throw new ValidationError('昵称不能为空');
  }
  if (nickname.length > 32) {
    throw new ValidationError('昵称最多 32 个字符');
  }
  if (password.length < 6) {
    throw new ValidationError('密码至少 6 位');
  }

  // 第一个注册的账号是管理员
  const isFirst = (await countAccounts()) === 0;
  const role = isFirst ? 'admin' : 'user';

  try {
    const [result] = await pool.query(
      'INSERT INTO accounts (username, nickname, password_hash, role) VALUES (?, ?, ?, ?)',
      [username, nickname, hashPassword(password), role]
    );
    console.log(`✅ 新账号注册: ${username}（${role}）${isFirst ? ' ← 首个账号，自动成为管理员' : ''}`);
    return { id: result.insertId, username, nickname, role };
  } catch (e) {
    if (e.code === 'ER_DUP_ENTRY') {
      throw new UsernameTakenError('这个用户名已经被注册了');
    }
    throw e;
  }
}

async function login(username, password) {
  username = String(username || '').trim();

  const [rows] = await pool.query(
    'SELECT id, username, nickname, role, password_hash FROM accounts WHERE username = ?',
    [username]
  );

  // 用户不存在时也走一遍哈希计算，避免通过响应时间探测用户名是否存在
  if (!rows.length) {
    verifyPassword(password, 'scrypt$00$00');
    throw new InvalidCredentialsError('用户名或密码错误');
  }

  const account = rows[0];
  if (!verifyPassword(password, account.password_hash)) {
    throw new InvalidCredentialsError('用户名或密码错误');
  }

  return { id: account.id, username: account.username, nickname: account.nickname, role: account.role };
}

module.exports = {
  hashPassword,
  verifyPassword,
  createSession,
  getSession,
  deleteSession,
  cleanupExpired,
  register,
  login,
  countAccounts,
  SESSION_TTL_MS,
  AccountError,
  UsernameTakenError,
  InvalidCredentialsError,
  ValidationError,
};
