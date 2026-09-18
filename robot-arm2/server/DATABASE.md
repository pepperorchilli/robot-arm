# 留言板数据库（MySQL）

原本留言存在 `server/data/messages.json` 里（整个数组读进内存，每次改动全量回写）。
现在改为 MySQL，本文档说明设计思路和用法。

---

## 一、快速开始

```bash
# 1. 确保 MySQL 在跑
brew services start mysql

# 2. 建库建表建账号（首次执行一次即可）
mysql -u root < schema.sql

# 3. 装依赖并启动
npm install
npm start
```

启动时会先探测数据库连接，连不上会直接报错退出，不会等到第一个请求才发现。

---

## 二、表结构设计

```
messages                          replies
┌────────────────────┐            ┌────────────────────┐
│ id      BIGINT  PK │◄──────┐    │ id         BIGINT PK│
│ nickname VARCHAR(50)│       └────│ message_id BIGINT FK│
│ content  VARCHAR(500)│          │ content    VARCHAR(500)│
│ created_at DATETIME(3)│         │ created_at DATETIME(3)│
│  KEY idx_created_at  │          │  KEY idx_message_id    │
└────────────────────┘            └────────────────────┘
                                         │
                    外键 ON DELETE CASCADE：删留言自动删掉它的回复
```

### 几个刻意的选择

| 决定 | 原因 |
|---|---|
| 回复单独建表，不塞进消息行 | 符合第一范式。塞 JSON 数组进一列的话，没法按回复查询、没法分页、也没法加约束 |
| `ON DELETE CASCADE` | 删留言时自动清理回复，数据库层面保证不留孤儿数据 |
| 字符集 `utf8mb4` | 支持完整 Unicode（含 emoji）。`utf8` 是阉割版，存不了 4 字节字符 |
| `DATETIME(3)` 而非 `TIMESTAMP` | 毫秒精度；且 `TIMESTAMP` 有 2038 年上限 |
| 存储引擎 `InnoDB` | 支持事务和外键（MyISAM 都不支持） |

### 索引

| 索引 | 服务于 |
|---|---|
| `messages.idx_created_at (created_at DESC)` | 列表页固定按时间倒序，直接顺着索引读，不用排序 |
| `replies.idx_message_id (message_id)` | 按留言查回复；外键列也必须有索引，否则删留言会全表扫 |

---

## 三、SQL 优化：实证

```bash
npm run explain
```

脚本会建 5 万行数据，对比加索引前后的执行计划和实测耗时。
全程只操作 `robot_arm_demo` 演示库，跑完自动删表，**不碰业务数据**。

实测结果：

| | 扫描行数 | 耗时 | Extra |
|---|---|---|---|
| 优化前 | 49914 | 11.76 ms | `Using filesort` |
| 优化后 | **20** | **0.30 ms** | `(空)` |

**扫描行数减少 99.96%，耗时快 39 倍。**

### 两个坑（都踩过）

**1. `ANALYZE TABLE` 不能省。**
批量插入后如果不更新统计信息，优化器没有依据，实测会**放弃索引去全表扫描**。
写完数据一定要 `ANALYZE TABLE`。

**2. MySQL 8.0.16+ 的 EXPLAIN 默认是树形格式。**
直接 `EXPLAIN` 返回的列名叫 `EXPLAIN`，拿到的是这种字符串：

```
-> Limit: 20 row(s)  (cost=2000 rows=20)
    -> Sort: t1.a DESC  (cost=2000 rows=20000)
        -> Table scan on t1  (cost=2000 rows=20000)
```

要经典的表格输出必须显式写 `EXPLAIN FORMAT=TRADITIONAL`。

---

## 四、另一个优化：消除 N+1 查询

列表接口要返回「每条留言 + 它的回复」，最容易写成这样：

```js
// ❌ N+1：30 条留言 = 1 + 30 = 31 次查询
const [messages] = await pool.query('SELECT * FROM messages ORDER BY created_at DESC');
for (const m of messages) {
  const [replies] = await pool.query('SELECT * FROM replies WHERE message_id = ?', [m.id]);
  m.replies = replies;
}
```

留言越多，查询次数线性增长。改成两条查询：

```js
// ✅ 固定 2 次查询，和留言条数无关
const [messages] = await pool.query('SELECT ... FROM messages ORDER BY created_at DESC');
const ids = messages.map(m => m.id);
const [replies] = await pool.query('SELECT ... FROM replies WHERE message_id IN (?)', [ids]);
// 然后在内存里按 message_id 归组
```

见 `store.js` 的 `list()`。

---

## 五、安全上的考虑

- **最小权限**：应用账号 `arm_app` 只有业务库的 `SELECT/INSERT/UPDATE/DELETE`，
  **没有 DROP/ALTER**。演示库另开一个 `robot_arm_demo` 放开权限。
- **全程参数化查询**：所有用户输入都走 `?` 占位符，杜绝 SQL 注入。
- **密码走环境变量**：`config.js` 里的是本地开发默认值，部署时必须用
  `DB_PASSWORD` / `ADMIN_PASSWORD` 覆盖，不要把生产密码写进代码。

---

## 六、从旧数据迁移

如果你之前用 JSON 文件存过留言：

```bash
npm run migrate
```

按原 id 导入（`INSERT IGNORE`，可重复执行不会重复插入），原文件保留不删。

---

## 文件一览

| 文件 | 作用 |
|---|---|
| `schema.sql` | 建库、建表、建账号，一条命令搞定 |
| `db.js` | MySQL 连接池 + 启动探活 |
| `store.js` | 数据访问层（对外接口和 JSON 版完全一致） |
| `migrate.js` | 旧 JSON 数据迁移 |
| `explain-demo.js` | SQL 优化实证（EXPLAIN + 压测） |
