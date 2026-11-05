#!/usr/bin/env python3
"""
本地存储（SQLite 单文件）
========================
开源版不依赖任何外部数据库：所有数据放在 server/data/sisi.db，
备份就是拷一个文件。表结构极简，够用且好读：

  voices  音色：名称 + 参考音频路径 + 参考文本（零样本 TTS 需要）
  ais     AI 角色：名称 + 人设提示词 + 绑定音色

参考音频文件本身放在 server/data/voices/<id>.wav，库里只存路径。
"""

from __future__ import annotations

import os
import sqlite3
import time
from pathlib import Path

# 数据目录：默认 server/data，可用环境变量挪走（Docker 挂载卷）
DATA_DIR = Path(os.getenv("SISI_DATA_DIR", Path(__file__).resolve().parent.parent / "data"))
DB_PATH = DATA_DIR / "sisi.db"
VOICE_DIR = DATA_DIR / "voices"


def _conn() -> sqlite3.Connection:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db() -> None:
    VOICE_DIR.mkdir(parents=True, exist_ok=True)
    with _conn() as conn:
        conn.executescript("""
            CREATE TABLE IF NOT EXISTS voices (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT NOT NULL,
                ref_path TEXT NOT NULL,
                ref_text TEXT NOT NULL DEFAULT '',
                duration REAL NOT NULL DEFAULT 0,
                created_at INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS ais (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT NOT NULL,
                prompt TEXT NOT NULL DEFAULT '',
                voice_id INTEGER NOT NULL DEFAULT 0,
                created_at INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS users (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                phone TEXT NOT NULL UNIQUE,
                name TEXT NOT NULL DEFAULT '',
                created_at INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS sessions (
                token TEXT PRIMARY KEY,
                user_id INTEGER NOT NULL,
                created_at INTEGER NOT NULL,
                expires_at INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS messages (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                owner TEXT NOT NULL,
                role TEXT NOT NULL,
                content TEXT NOT NULL,
                created_at INTEGER NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_messages_owner ON messages(owner, id);
        """)


def _voice_dict(row: sqlite3.Row) -> dict:
    return {
        "id": row["id"],
        "name": row["name"],
        "ref_path": row["ref_path"],
        "ref_text": row["ref_text"],
        "duration": row["duration"],
        "created_at": row["created_at"],
    }


def _ai_dict(row: sqlite3.Row) -> dict:
    return {
        "id": row["id"],
        "name": row["name"],
        "prompt": row["prompt"],
        "voice_id": row["voice_id"],
        "created_at": row["created_at"],
    }


# ─── 音色 ──────────────────────────────────────────────────────

def list_voices() -> list[dict]:
    with _conn() as conn:
        rows = conn.execute("SELECT * FROM voices ORDER BY id DESC").fetchall()
    return [_voice_dict(r) for r in rows]


def get_voice(voice_id: int) -> dict | None:
    with _conn() as conn:
        row = conn.execute("SELECT * FROM voices WHERE id=?", (voice_id,)).fetchone()
    return _voice_dict(row) if row else None


def add_voice(name: str, ref_path: str, ref_text: str, duration: float) -> dict:
    with _conn() as conn:
        cur = conn.execute(
            "INSERT INTO voices (name, ref_path, ref_text, duration, created_at) VALUES (?,?,?,?,?)",
            (name, ref_path, ref_text, duration, int(time.time())),
        )
        new_id = cur.lastrowid
    return get_voice(new_id)


def delete_voice(voice_id: int) -> bool:
    v = get_voice(voice_id)
    if not v:
        return False
    with _conn() as conn:
        conn.execute("DELETE FROM voices WHERE id=?", (voice_id,))
        conn.execute("UPDATE ais SET voice_id=0 WHERE voice_id=?", (voice_id,))
    try:
        os.remove(v["ref_path"])
    except OSError:
        pass
    return True


# ─── AI 角色 ───────────────────────────────────────────────────

def list_ais() -> list[dict]:
    with _conn() as conn:
        rows = conn.execute("SELECT * FROM ais ORDER BY id DESC").fetchall()
    return [_ai_dict(r) for r in rows]


def get_ai(ai_id: int) -> dict | None:
    with _conn() as conn:
        row = conn.execute("SELECT * FROM ais WHERE id=?", (ai_id,)).fetchone()
    return _ai_dict(row) if row else None


def add_ai(name: str, prompt: str, voice_id: int) -> dict:
    with _conn() as conn:
        cur = conn.execute(
            "INSERT INTO ais (name, prompt, voice_id, created_at) VALUES (?,?,?,?)",
            (name, prompt, voice_id, int(time.time())),
        )
        new_id = cur.lastrowid
    return get_ai(new_id)


def delete_ai(ai_id: int) -> bool:
    with _conn() as conn:
        cur = conn.execute("DELETE FROM ais WHERE id=?", (ai_id,))
    return cur.rowcount > 0


# ─── 账号与会话 ─────────────────────────────────────────────────

SESSION_DAYS = 30


def upsert_user(phone: str, name: str = "") -> dict:
    """手机号登录成功后落本地用户（同一个手机号复用同一条记录）。"""
    with _conn() as conn:
        row = conn.execute("SELECT * FROM users WHERE phone=?", (phone,)).fetchone()
        if row:
            if name and name != row["name"]:
                conn.execute("UPDATE users SET name=? WHERE id=?", (name, row["id"]))
            uid = row["id"]
        else:
            uid = conn.execute(
                "INSERT INTO users (phone, name, created_at) VALUES (?,?,?)",
                (phone, name, int(time.time())),
            ).lastrowid
        row = conn.execute("SELECT * FROM users WHERE id=?", (uid,)).fetchone()
    return {"id": row["id"], "phone": row["phone"], "name": row["name"]}


def create_session(user_id: int) -> str:
    import secrets
    token = secrets.token_hex(24)
    now = int(time.time())
    with _conn() as conn:
        conn.execute("DELETE FROM sessions WHERE expires_at<?", (now,))
        conn.execute(
            "INSERT INTO sessions (token, user_id, created_at, expires_at) VALUES (?,?,?,?)",
            (token, user_id, now, now + SESSION_DAYS * 86400),
        )
    return token


def session_user(token: str) -> dict | None:
    if not token:
        return None
    with _conn() as conn:
        row = conn.execute(
            "SELECT u.* FROM sessions s JOIN users u ON u.id=s.user_id "
            "WHERE s.token=? AND s.expires_at>?", (token, int(time.time()))).fetchone()
    if not row:
        return None
    return {"id": row["id"], "phone": row["phone"], "name": row["name"]}


def delete_session(token: str) -> None:
    with _conn() as conn:
        conn.execute("DELETE FROM sessions WHERE token=?", (token,))


# ─── 对话历史（按 owner 隔离：u:<uid> 或 g:<guest_id>） ──────────

def append_message(owner: str, role: str, content: str) -> None:
    if not owner or not content:
        return
    with _conn() as conn:
        conn.execute("INSERT INTO messages (owner, role, content, created_at) VALUES (?,?,?,?)",
                     (owner, role, content, int(time.time())))


def list_messages(owner: str, limit: int = 20) -> list[dict]:
    """取最近 limit 条，按时间正序返回（直接喂给 LLM）。"""
    if not owner:
        return []
    with _conn() as conn:
        rows = conn.execute(
            "SELECT role, content, created_at FROM messages WHERE owner=? "
            "ORDER BY id DESC LIMIT ?", (owner, limit)).fetchall()
    return [{"role": r["role"], "content": r["content"], "created_at": r["created_at"]}
            for r in reversed(rows)]


def clear_messages(owner: str) -> None:
    if not owner:
        return
    with _conn() as conn:
        conn.execute("DELETE FROM messages WHERE owner=?", (owner,))


init_db()
