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
from pathlib import Path

from fastapi import FastAPI, Header, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse

from . import config as cfgmod
from .providers import build_llm
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
    """固件接入点。固件 URL 形如 wss://<host>/s2s/ws?api_key=xxx&firmware_version=xxx。"""
    await ws.accept()
    cfg = cfgmod.load_config()
    if not cfg["nice"]["api_key"] and cfg["asr"]["provider"] == "nice" \
            and cfg["llm"]["provider"] == "nice" and cfg["tts"]["provider"] == "nice":
        await ws.send_text(json.dumps({"type": "error",
                                       "message": "如意云 api_key 未配置，请在 /admin 填写"}, ensure_ascii=False))
        await ws.close(code=1008)
        return
    try:
        if cfg["asr"]["provider"] == cfg["llm"]["provider"] == cfg["tts"]["provider"] == "nice":
            # 默认模式：整体转发云端，全链路编排由如意云完成
            proxy = NiceS2SProxy(cfg["nice"]["url"], cfg["nice"]["api_key"])
            await proxy.run(ws)
        else:
            # 自编排模式：本机跑 ASR→LLM→TTS 流水线
            await FirmwareSession(ws, cfg).run()
    except WebSocketDisconnect:
        pass
    except Exception as exc:
        log.exception("会话异常退出")
        try:
            await ws.send_text(json.dumps({"type": "error", "message": str(exc)}, ensure_ascii=False))
            await ws.close()
        except Exception:
            pass
