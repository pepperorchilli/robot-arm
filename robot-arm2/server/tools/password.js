#!/usr/bin/env node
/*
 * 账号密码小工具 —— 离线验证 / 重置
 *
 * 为什么需要它：
 *   密码只存 scrypt 哈希（见 accounts.js 开头），**单向的，谁也读不回来**。
 *   忘了密码只有两条路：拿候选密码去对，或者直接重置成新的。
 *   而网页登录口有防爆破限流（同一 IP 连错 5 次封 15 分钟），
 *   用来试密码非常不划算 —— 这个工具在本机直接查哈希，不经过登录接口，
 *   试多少次都不会被锁。
 *
 * 用法（在 server/ 目录下跑）：
 *
 *   node tools/password.js check qiudai     # 验证：输入候选密码，只说对不对
 *   node tools/password.js set   qiudai     # 重置：输入两遍新密码，写回数据库
 *
 * 输入是关回显的（终端不显示你敲了什么），密码不会出现在命令行历史里，
 * 也不会打印到屏幕上。
 *
 * ⚠️ 只在能连到数据库的本机用。别把这个文件暴露到任何 HTTP 路由上。
 */

const readline = require('readline');
const { pool } = require('../db');
const { hashPassword, verifyPassword } = require('../accounts');

// 关回显地读一行（终端不显示输入内容）
function askHidden(prompt) {
  return new Promise((resolve) => {
    process.stdout.write(prompt);

    const stdin = process.stdin;
    const wasRaw = stdin.isRaw;
    if (stdin.isTTY) stdin.setRawMode(true);
    stdin.resume();

    let buf = '';
    const onData = (chunk) => {
      for (const c of chunk.toString('utf8')) {
        if (c === '\r' || c === '\n') {
          stdin.removeListener('data', onData);
          if (stdin.isTTY) stdin.setRawMode(wasRaw);
          stdin.pause();
          process.stdout.write('\n');
          return resolve(buf);
        }
        if (c === '\u0003') {              // Ctrl-C
          process.stdout.write('\n');
          process.exit(130);
        }
        if (c === '\u007f' || c === '\b') { // 退格
          buf = buf.slice(0, -1);
          continue;
        }
        buf += c;
      }
    };
    stdin.on('data', onData);
  });
}

async function findAccount(username) {
  const [rows] = await pool.query(
    'SELECT id, username, nickname, role, password_hash FROM accounts WHERE username = ?',
    [username]
  );
  return rows[0];
}

async function main() {
  const [cmd, username] = process.argv.slice(2);

  if (!cmd || !username) {
    console.log('用法:');
    console.log('  node tools/password.js check <用户名>   # 验证候选密码（不触发限流）');
    console.log('  node tools/password.js set   <用户名>   # 重置密码');
    process.exit(1);
  }

  const account = await findAccount(username);
  if (!account) {
    console.error(`❌ 没有这个账号: ${username}`);
    process.exit(1);
  }

  if (cmd === 'check') {
    const candidate = await askHidden(`输入 ${username} 的候选密码: `);
    if (!candidate) { console.error('❌ 空密码'); process.exit(1); }

    if (verifyPassword(candidate, account.password_hash)) {
      console.log(`✅ 对上了 —— ${username} 的密码就是你刚输入的这个`);
    } else {
      console.log('❌ 不对。换个再试（这个操作不经过登录接口，不会被封）');
      process.exit(2);
    }
    return;
  }

  if (cmd === 'set') {
    console.log(`即将重置账号 #${account.id} ${account.username}（${account.nickname}，${account.role}）的密码`);
    const p1 = await askHidden('新密码: ');
    if (p1.length < 6) { console.error('❌ 太短了，至少 6 位（和注册接口的要求一致）'); process.exit(1); }
    const p2 = await askHidden('再输一遍: ');
    if (p1 !== p2) { console.error('❌ 两次输入不一致，没改动'); process.exit(1); }

    await pool.query('UPDATE accounts SET password_hash = ? WHERE id = ?',
                     [hashPassword(p1), account.id]);
    console.log('✅ 已重置。用新密码登录即可。');
    console.log('   （旧密码立刻失效；已登录的会话仍然有效，要踢掉就重启服务器或清 sessions 表）');
    return;
  }

  console.error(`❌ 不认识子命令: ${cmd}（只有 check / set）`);
  process.exit(1);
}

main()
  .then(() => process.exit(0))
  .catch((e) => { console.error('出错了:', e.message); process.exit(1); });
