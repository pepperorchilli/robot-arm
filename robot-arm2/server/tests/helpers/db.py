"""测试库的准备与清理。

设计原则：**表结构只有一个来源** —— server/schema.sql。
测试库不另存一份 DDL，而是在启动时从 schema.sql 派生，避免两份结构漂移
（真实项目里 DDL 漂移是最常见的"测试环境和线上不一样"事故来源）。

schema.sql 里带了 `CREATE DATABASE robot_arm` / `USE robot_arm` / 建用户 / 授权
这几条只适合生产初始化，这里在执行前剔掉，剩下的 CREATE TABLE 原样执行。
"""

from __future__ import annotations

import os
import pathlib
import re

import pymysql

SERVER_DIR = pathlib.Path(__file__).resolve().parents[2]
SCHEMA_FILE = SERVER_DIR / "schema.sql"

TEST_DB = os.environ.get("TEST_DB_NAME", "robot_arm_test")

# 业务账号：只做增删改查（与生产 arm_app 的权限一致）
DB = dict(
    host=os.environ.get("TEST_DB_HOST", "127.0.0.1"),
    port=int(os.environ.get("TEST_DB_PORT", "3306")),
    user=os.environ.get("TEST_DB_USER", "arm_app"),
    password=os.environ.get("TEST_DB_PASSWORD", "arm_dev_password"),
    database=TEST_DB,
    charset="utf8mb4",
    autocommit=True,
)

# 管理账号：只在建表时用一次
ADMIN_DB = dict(
    host=DB["host"],
    port=DB["port"],
    user=os.environ.get("TEST_DB_ADMIN_USER", "root"),
    password=os.environ.get("TEST_DB_ADMIN_PASSWORD", ""),
    database=TEST_DB,
    charset="utf8mb4",
    autocommit=True,
)

# 按外键依赖倒序删，避免约束报错
TABLES = ("replies", "messages", "sessions", "accounts")

_SKIP = re.compile(
    r"^\s*(CREATE\s+DATABASE|USE\b|CREATE\s+USER|GRANT\b|FLUSH\s+PRIVILEGES)",
    re.IGNORECASE,
)


def connect(admin: bool = False):
    return pymysql.connect(**(ADMIN_DB if admin else DB))


def schema_statements() -> list[str]:
    """从 schema.sql 里取出可以直接在测试库上执行的语句。"""
    sql = SCHEMA_FILE.read_text(encoding="utf-8")

    # 去掉行注释，否则按分号切分时会把注释和语句粘在一起
    lines = [ln for ln in sql.splitlines() if not ln.strip().startswith("--")]
    sql = "\n".join(lines)

    return [
        stmt.strip()
        for stmt in sql.split(";")
        if stmt.strip() and not _SKIP.match(stmt)
    ]


def ensure_schema() -> bool:
    """幂等地建表。成功返回 True；没有管理权限时返回 False（表应已存在）。"""
    try:
        conn = connect(admin=True)
    except pymysql.MySQLError:
        return False

    try:
        with conn.cursor() as cur:
            for stmt in schema_statements():
                cur.execute(stmt)
        return True
    finally:
        conn.close()


def reset() -> None:
    """清空业务数据，让每个用例从同一个起点开始。

    不用 TRUNCATE：业务账号没有 DROP 权限（这也是生产最小权限的体现）。
    DELETE + 关闭外键检查可以达到同样效果，只是不自增 ID 不归零 —— 测试
    不应该依赖具体主键值，依赖了反而是设计问题。
    """
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute("SET FOREIGN_KEY_CHECKS=0")
            for table in TABLES:
                cur.execute(f"DELETE FROM {table}")
            cur.execute("SET FOREIGN_KEY_CHECKS=1")
    finally:
        conn.close()
