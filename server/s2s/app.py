#!/usr/bin/env python3
"""
FastAPI 应用入口
================
  WS   /s2s/ws        固件接入端点（固件 CONFIG_OTA_URL 配 wss://<host>/s2s/ws）
  GET  /admin         管理后台配置页
  GET  /admin/api/config    读配置
  POST /admin/api/config    写配置（页面自动保存调用）
  POST /admin/api/test/llm  测试 LLM 提供商连通性
  GET  /               跳转 /admin
"""

from __future__ import annotations

import asyncio
import json
import logging
import secrets
import time
from pathlib import Path

import httpx
from fastapi import (FastAPI, File, Form, Header, HTTPException, UploadFile,
                     WebSocket, WebSocketDisconnect)
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse, Response

from . import config as cfgmod
from . import store
from . import voice_audio
from .providers import build_asr, build_llm, build_tts
from .providers.nice import NiceS2SProxy
from .session import FirmwareSession

log = logging.getLogger("s2s")

app = FastAPI(title="sisi-esp-server", docs_url=None, redoc_url=None)

_STATIC = Path(__file__).resolve().parent / "static"


def _check_admin(password: str | None) -> None:
    """admin_password 非空时校验 X-Admin-Token；留空表示内网自用不鉴权。"""
    admin_pw = cfgmod.load_config()["server"].get("admin_password", "")
    if admin_pw and password != admin_pw:
        raise HTTPException(status_code=401, detail="admin token 无效")


@app.get("/")
async def index():
    return RedirectResponse("/admin")


@app.get("/admin")
async def admin_page():
    return HTMLResponse((_STATIC / "admin.html").read_text(encoding="utf-8"))


@app.get("/demo")
async def demo_page():
    """浏览器对话 demo：麦克风 PCM 上行 + WebCodecs 解码 opus 下行。"""
    return HTMLResponse((_STATIC / "demo.html").read_text(encoding="utf-8"))


# ─── 登录与对话历史 ──────────────────────────────────────────────

def _bearer(authorization: str) -> str:
    if authorization and authorization.lower().startswith("bearer "):
        return authorization[7:].strip()
    return (authorization or "").strip()


def _owner_of(authorization: str, guest: str = "") -> str:
    """u:<uid> 为登录用户；g:<guest_id> 为未登录的浏览器（auth.required=false 时用）。"""
    user = store.session_user(_bearer(authorization))
    if user:
        return f"u:{user['id']}"
    return f"g:{guest}" if guest else ""


@app.post("/api/auth/send_code")
async def auth_send_code(body: dict):
    phone = (body.get("phone") or "").strip()
    if not phone:
        raise HTTPException(400, "请输入手机号")
    cfg = cfgmod.load_config()["auth"]
    try:
        async with httpx.AsyncClient(timeout=30) as c:
            r = await c.post(cfg["url"].rstrip("/") + "/api/auth/send_code",
                             json={"phone": phone, "app_name": "ruyi-esp32"})
    except httpx.HTTPError as exc:
        raise HTTPException(502, f"认证服务不可用：{exc}")
    if r.status_code != 200:
        try:
            msg = r.json().get("detail") or "验证码发送失败"
        except ValueError:
            msg = f"验证码发送失败（{r.status_code}）"
        raise HTTPException(r.status_code, msg)
    return {"message": "验证码已发送"}


@app.post("/api/auth/login")
async def auth_login(body: dict):
    phone = (body.get("phone") or "").strip()
    code = (body.get("code") or "").strip()
    if not phone or not code:
        raise HTTPException(400, "请输入手机号和验证码")
    cfg = cfgmod.load_config()["auth"]
    try:
        async with httpx.AsyncClient(timeout=30) as c:
            r = await c.post(cfg["url"].rstrip("/") + "/api/auth/login",
                             json={"phone": phone, "code": code},
                             headers={"X-Db-Name": cfg.get("db_name", "nice_ai"),
                                      "X-Users-Table": "xd_users"})
    except httpx.HTTPError as exc:
        raise HTTPException(502, f"认证服务不可用：{exc}")
    if r.status_code != 200:
        try:
            msg = r.json().get("detail") or "登录失败"
        except ValueError:
            msg = f"登录失败（{r.status_code}）"
        raise HTTPException(r.status_code, msg)

    u = r.json()
    user = store.upsert_user(phone, (u.get("username") or "").strip())
    token = store.create_session(user["id"])
    return {"token": token, "user": user}


@app.get("/api/auth/me")
async def auth_me(authorization: str = Header(default="")):
    cfg = cfgmod.load_config()["auth"]
    user = store.session_user(_bearer(authorization))
    if not user:
        raise HTTPException(401, "请先登录")
    return {"user": user, "required": bool(cfg["required"])}


@app.post("/api/auth/logout")
async def auth_logout(authorization: str = Header(default="")):
    store.delete_session(_bearer(authorization))
    return {"ok": True}


@app.get("/api/history")
async def history_list(authorization: str = Header(default=""), guest: str = ""):
    owner = _owner_of(authorization, guest)
    msgs = store.list_messages(owner, 50) if owner else []
    return {"messages": msgs,
            "auth_required": bool(cfgmod.load_config()["auth"]["required"]),
            "isolated_by": "user" if owner.startswith("u:") else ("guest" if owner else "")}


@app.post("/api/history/clear")
async def history_clear(authorization: str = Header(default=""), guest: str = ""):
    owner = _owner_of(authorization, guest)
    store.clear_messages(owner)
    return {"ok": True}


def _record_round(owner: str, msg: dict) -> None:
    """一轮结束（done/aborted）时落库：用户那句 + AI 那句。"""
    if not owner:
        return
    user_text = (msg.get("asr_text") or "").strip()
    ai_text = (msg.get("llm_text") or "").strip()
    if user_text:
        store.append_message(owner, "user", user_text)
    if ai_text:
        store.append_message(owner, "assistant", ai_text)


# ─── 音色工坊：录/传一段参考音 → 降噪 → 生成音色 → 建 AI 角色 ───

@app.get("/studio")
async def studio_page():
    return HTMLResponse((_STATIC / "studio.html").read_text(encoding="utf-8"))


@app.get("/api/studio/info")
async def studio_info(x_admin_token: str | None = Header(default=None)):
    _check_admin(x_admin_token)
    cfg = cfgmod.load_config()
    tts = _studio_tts(cfg)
    return {
        "tts_provider": cfg["tts"]["provider"],
        "supports_ref": bool(getattr(tts, "supports_ref", False)),
        "sample_rate": int(getattr(tts, "sample_rate", 24000)),
        "asr_enabled": cfg["asr"]["provider"] in ("doubao", "aliyun"),
        "voices": len(store.list_voices()),
        "ais": len(store.list_ais()),
    }


def _studio_tts(cfg: dict):
    """取 TTS 实例；nice 模式（整体转发）没有本地 TTS 实例，返回 None。"""
    try:
        return build_tts(cfg)
    except ValueError:
        return None


@app.get("/api/voices")
async def voices_list(x_admin_token: str | None = Header(default=None)):
    _check_admin(x_admin_token)
    return {"voices": store.list_voices()}


@app.post("/api/voices")
async def voices_create(
    file: UploadFile = File(...),
    name: str = Form(default=""),
    ref_text: str = Form(default=""),
    x_admin_token: str | None = Header(default=None),
):
    """上传/录制的一段话 → 解码、降噪、裁静音、归一化 → 存成音色。"""
    _check_admin(x_admin_token)
    raw = await file.read()
    try:
        pcm = voice_audio.prepare_reference(raw)
    except voice_audio.AudioError as exc:
        raise HTTPException(400, str(exc))

    voice_name = (name or "").strip()[:32] or "我的音色"
    text = (ref_text or "").strip()
    # 自编排模式下顺手用 ASR 补出参考文本（零样本模型给参考文本相似度更高）
    cfg = cfgmod.load_config()
    if not text and cfg["asr"]["provider"] in ("doubao", "aliyun"):
        try:
            text = (await build_asr(cfg).transcribe(pcm)).strip()
        except Exception:
            text = ""

    tmp = store.VOICE_DIR / f"{secrets.token_hex(8)}.wav"
    voice_audio.write_wav(str(tmp), pcm)
    v = store.add_voice(voice_name, str(tmp), text, round(voice_audio.duration(pcm), 2))
    return {"voice": v}


@app.get("/api/voices/{voice_id}/audio")
async def voice_audio_get(voice_id: int, token: str = ""):
    """参考音试听（audio 标签不能带 header，用 query token 校验）。"""
    _check_admin(token or None)
    v = store.get_voice(voice_id)
    if not v:
        raise HTTPException(404, "音色不存在")
    data = Path(v["ref_path"]).read_bytes()
    return Response(data, media_type="audio/wav")


@app.post("/api/voices/{voice_id}/preview")
async def voice_preview(voice_id: int, x_admin_token: str | None = Header(default=None)):
    """用该音色合成一句固定的话，直接返回 wav。"""
    _check_admin(x_admin_token)
    v = store.get_voice(voice_id)
    if not v:
        raise HTTPException(404, "音色不存在")
    cfg = cfgmod.load_config()
    tts = _studio_tts(cfg)
    if tts is None or not getattr(tts, "supports_ref", False):
        raise HTTPException(
            400, f"当前 TTS（{cfg['tts']['provider']}）不支持参考音，"
                 "请把 tts.provider 设为 local 并配置自建零样本 TTS 地址")
    pcm = b"".join([c async for c in tts.synthesize(
        "你好，我是如意，很高兴认识你。", ref={"path": v["ref_path"], "text": v["ref_text"]})])
    return Response(voice_audio.to_wav(pcm, tts.sample_rate), media_type="audio/wav")


@app.delete("/api/voices/{voice_id}")
async def voices_delete(voice_id: int, x_admin_token: str | None = Header(default=None)):
    _check_admin(x_admin_token)
    if not store.delete_voice(voice_id):
        raise HTTPException(404, "音色不存在")
    return {"ok": True}


@app.get("/api/ais")
async def ais_list(x_admin_token: str | None = Header(default=None)):
    _check_admin(x_admin_token)
    return {"ais": store.list_ais(), "voices": store.list_voices()}


@app.post("/api/ais")
async def ais_create(body: dict, x_admin_token: str | None = Header(default=None)):
    _check_admin(x_admin_token)
    name = (body.get("name") or "").strip()[:32]
    if not name:
        raise HTTPException(400, "请填写 AI 名字")
    prompt = (body.get("prompt") or "").strip()
    voice_id = int(body.get("voice_id") or 0)
    if voice_id and not store.get_voice(voice_id):
        raise HTTPException(400, "音色不存在")
    return {"ai": store.add_ai(name, prompt, voice_id)}


@app.delete("/api/ais/{ai_id}")
async def ais_delete(ai_id: int, x_admin_token: str | None = Header(default=None)):
    _check_admin(x_admin_token)
    if not store.delete_ai(ai_id):
        raise HTTPException(404, "AI 角色不存在")
    return {"ok": True}


@app.get("/admin/api/config")
async def get_config(x_admin_token: str | None = Header(default=None)):
    _check_admin(x_admin_token)
    return JSONResponse(cfgmod.load_config())


@app.post("/admin/api/config")
async def set_config(body: dict, x_admin_token: str | None = Header(default=None)):
    _check_admin(x_admin_token)
    # 以默认结构为骨架合并提交，防止页面提交缺键把配置写残
    merged = cfgmod.load_config()
    cfgmod._deep_merge(merged, body)
    ok, missing = cfgmod.all_providers_ready(merged)
    cfgmod.save_config(merged)
    return {"ok": True, "ready": ok, "missing": missing}


@app.post("/admin/api/test/llm")
async def test_llm(x_admin_token: str | None = Header(default=None)):
    """对当前配置的 LLM 做一次最小对话，验证密钥/网络/模型名。"""
    _check_admin(x_admin_token)
    cfg = cfgmod.load_config()
    provider = cfg["llm"]["provider"]
    if provider == "nice":
        return {"ok": bool(cfg["nice"]["api_key"]),
                "message": "nice 模式由云端编排，服务端仅校验 api_key 已填"}
    try:
        llm = build_llm(cfg)
        chunks = []
        async for chunk in llm.stream_chat("只回复两个字：正常", []):
            chunks.append(chunk)
            if len(chunks) > 8:
                break
        return {"ok": True, "message": f"LLM({provider}) 连通：{''.join(chunks)[:40]}"}
    except Exception as exc:
        return {"ok": False, "message": f"LLM({provider}) 失败：{exc}"}


@app.websocket("/s2s/ws")
async def firmware_ws(ws: WebSocket):
    """固件接入点。固件 URL 形如 wss://<host>/s2s/ws?api_key=xxx&firmware_version=xxx。

    浏览器 demo 额外带 ?token=<登录令牌>（或 ?guest=<本机随机 id> 当 auth.required=false）。
    带了身份就按用户隔离存取对话历史：建连时注入历史，每轮结束落库。
    """
    await ws.accept()
    cfg = cfgmod.load_config()
    if not cfg["nice"]["api_key"] and cfg["asr"]["provider"] == "nice" \
            and cfg["llm"]["provider"] == "nice" and cfg["tts"]["provider"] == "nice":
        await ws.send_text(json.dumps({"type": "error",
                                       "message": "如意云 api_key 未配置，请在 /admin 填写"}, ensure_ascii=False))
        await ws.close(code=1008)
        return

    user = store.session_user(ws.query_params.get("token", ""))
    guest = ws.query_params.get("guest", "")
    if cfg["auth"]["required"] and not user:
        await ws.send_text(json.dumps({"type": "auth_required", "message": "请先登录后再对话"},
                                      ensure_ascii=False))
        await ws.close(code=1008)
        return
    owner = f"u:{user['id']}" if user else (f"g:{guest}" if guest else "")

    try:
        if cfg["asr"]["provider"] == cfg["llm"]["provider"] == cfg["tts"]["provider"] == "nice":
            # 默认模式：整体转发云端，全链路编排由如意云完成
            history = [{"role": m["role"], "content": m["content"]}
                       for m in store.list_messages(owner, 20)]
            proxy = NiceS2SProxy(cfg["nice"]["url"], cfg["nice"]["api_key"],
                                 history=history,
                                 on_done=(lambda m: _record_round(owner, m)) if owner else None)
            await proxy.run(ws)
        else:
            # 自编排模式：本机跑 ASR→LLM→TTS 流水线
            await FirmwareSession(ws, cfg, owner=owner).run()
    except WebSocketDisconnect:
        pass
    except Exception as exc:
        log.exception("会话异常退出")
        try:
            await ws.send_text(json.dumps({"type": "error", "message": str(exc)}, ensure_ascii=False))
            await ws.close()
        except Exception:
            pass
