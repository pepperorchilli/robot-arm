// SQL 优化实证：用 EXPLAIN 和实测数据，证明索引到底带来了什么
//
// 执行： node explain-demo.js   （或 npm run explain）
//
// 全程只操作演示库 robot_arm_demo 里的临时表，跑完自动删除，**不碰业务数据**。
//
// 结论一句话：
//   没索引 → 全表扫描 + 文件排序（扫 N 行，按时间排完序再取 20 条）
//   有索引 → 索引本身就是按时间排好序的，直接读最后 20 条（只扫 20 行）

const mysql = require('mysql2/promise');
const config = require('./config');

// 用单独的演示库：要反复建表删表，不该动业务库
const pool = mysql.createPool({
  ...config.DB,
  database: 'robot_arm_demo',
  waitForConnections: true,
  connectionLimit: 5,
});

const ROWS = 50000;
const TABLE = 'demo_messages';

// 和真实表同结构，但**故意不加索引**
const CREATE_TABLE = `
  CREATE TABLE ${TABLE} (
    id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    nickname   VARCHAR(50)     NOT NULL,
    content    VARCHAR(500)    NOT NULL,
    created_at DATETIME(3)     NOT NULL,
    PRIMARY KEY (id)
  ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
`;

// 我们关心的查询：按时间倒序取最新 20 条（留言列表页就是这个）
const QUERY = `SELECT id, nickname, content, created_at
               FROM ${TABLE}
               ORDER BY created_at DESC
               LIMIT 20`;

// ---------------------------------------------------------------
// 拿执行计划
// ---------------------------------------------------------------
// ⚠️ 两个坑：
//   1) MySQL 8.0.16+ 起 EXPLAIN 默认输出树形格式（列名叫 EXPLAIN），
//      要经典表格必须显式写 FORMAT=TRADITIONAL
//   2) 批量插入后必须 ANALYZE TABLE 更新统计信息，
//      否则优化器没有依据，会做出错误选择（实测会放弃索引去全表扫）
async function explain(label) {
  const [rows] = await pool.query(`EXPLAIN FORMAT=TRADITIONAL ${QUERY}`);
  const r = rows[0];
  console.log(`\n  ${label}`);
  console.log(`    type         : ${r.type}   ${r.type === 'ALL' ? '← 全表扫描！' : '← 走索引'}`);
  console.log(`    key          : ${r.key || '(没用索引)'}`);
  console.log(`    rows         : ${r.rows}   ← 预估要扫多少行`);
  console.log(`    Extra        : ${r.Extra || '-'}`);
  return r;
}

// 实测耗时：跑多次取平均，减少偶然波动
async function benchmark(times = 20) {
  await pool.query(QUERY); // 预热
  const start = process.hrtime.bigint();
  for (let i = 0; i < times; i++) await pool.query(QUERY);
  const end = process.hrtime.bigint();
  return Number(end - start) / 1e6 / times;
}

async function main() {
  console.log('='.repeat(64));
  console.log(' SQL 优化实证：索引对 ORDER BY ... LIMIT 的影响');
  console.log('='.repeat(64));

  // ---------- 准备 ----------
  console.log(`\n[准备] 建演示表并插入 ${ROWS} 行…`);
  await pool.query(`DROP TABLE IF EXISTS ${TABLE}`);
  await pool.query(CREATE_TABLE);

  // 批量插入：一条 INSERT 带多个 VALUES，比逐条插快几十倍
  const BATCH = 1000;
  const base = Date.now() - ROWS * 1000;
  const tInsert = Date.now();
  for (let start = 0; start < ROWS; start += BATCH) {
    const values = [];
    const params = [];
    for (let i = start; i < Math.min(start + BATCH, ROWS); i++) {
      values.push('(?, ?, ?)');
      params.push(`用户${i}`, `这是第 ${i} 条留言`, new Date(base + i * 1000));
    }
    await pool.query(
      `INSERT INTO ${TABLE} (nickname, content, created_at) VALUES ${values.join(',')}`,
      params
    );
  }
  const [[{ n }]] = await pool.query(`SELECT COUNT(*) AS n FROM ${TABLE}`);
  console.log(`       完成，共 ${n} 行，插入耗时 ${((Date.now() - tInsert) / 1000).toFixed(1)}s`);

  // 关键：更新统计信息，否则优化器瞎选
  await pool.query(`ANALYZE TABLE ${TABLE}`);

  // ---------- 优化前 ----------
  console.log('\n' + '-'.repeat(64));
  console.log('【优化前】created_at 上没有索引');
  console.log('-'.repeat(64));
  const before = await explain('EXPLAIN 结果：');
  const slow = await benchmark();
  console.log(`    实测平均耗时 : ${slow.toFixed(2)} ms`);

  // ---------- 加索引 ----------
  console.log('\n' + '-'.repeat(64));
  console.log('【优化】加索引');
  console.log('-'.repeat(64));
  console.log(`  ALTER TABLE ${TABLE} ADD INDEX idx_created_at (created_at DESC);`);
  const t0 = Date.now();
  await pool.query(`ALTER TABLE ${TABLE} ADD INDEX idx_created_at (created_at DESC)`);
  await pool.query(`ANALYZE TABLE ${TABLE}`);
  console.log(`  建索引耗时 : ${((Date.now() - t0) / 1000).toFixed(2)}s（一次性成本，之后每次查询都受益）`);

  // ---------- 优化后 ----------
  console.log('\n' + '-'.repeat(64));
  console.log('【优化后】走 idx_created_at');
  console.log('-'.repeat(64));
  const after = await explain('EXPLAIN 结果：');
  const fast = await benchmark();
  console.log(`    实测平均耗时 : ${fast.toFixed(2)} ms`);

  // ---------- 结论 ----------
  const scanBefore = Number(before.rows);
  const scanAfter = Number(after.rows);
  console.log('\n' + '='.repeat(64));
  console.log(' 结论');
  console.log('='.repeat(64));
  console.log(`  数据量        : ${ROWS} 行`);
  console.log('');
  console.log(`  扫描行数      : ${scanBefore}  →  ${scanAfter}`);
  console.log(`                  （减少 ${(100 - (scanAfter / scanBefore) * 100).toFixed(2)}%）`);
  console.log('');
  console.log(`  实测耗时      : ${slow.toFixed(2)} ms  →  ${fast.toFixed(2)} ms`);
  console.log(`                  （快 ${(slow / fast).toFixed(1)} 倍）`);
  console.log('');
  console.log('  Extra 字段的变化说明了本质：');
  console.log('    优化前  Using filesort  → 把全部数据捞出来再排序，数据越多越慢');
  console.log('    优化后  (空)            → 索引本身就是有序的，直接读最后 20 条');
  console.log('');
  console.log(`  数据量越大差距越夸张：扫描行数是 O(N) 和 O(20) 的区别。`);
  console.log('='.repeat(64));

  // ---------- 清理 ----------
  await pool.query(`DROP TABLE IF EXISTS ${TABLE}`);
  console.log('\n演示表已删除，业务数据未受影响。');
}

main()
  .catch((e) => {
    console.error('演示失败：', e.message);
    process.exitCode = 1;
  })
  .finally(() => pool.end());
