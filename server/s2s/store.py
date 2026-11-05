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


init_db()
