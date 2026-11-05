#!/usr/bin/env python3
"""
默认模式：如意云 S2S 透明代理
=============================
固件协议与如意云 S2S API（wss://dev.nice-ai.cn/s2s/）是同一套协议——
本服务在中间做纯消息级转发，不做任何协议转换：

  固件首帧 JSON → 替换 api_key 为配置中的如意云密钥 → 原样转发云端
  固件后续 JSON / Binary → 原样转发云端
  云端 JSON / Binary（asr_result/llm_chunk/tts_start/...）→ 原样回传固件

链路延迟只多一跳本地转发，LLM/TTS 编排全部在云端完成。
"""

from __future__ import annotations

import asyncio
import json
import logging

from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosedError, ConnectionClosedOK

log = logging.getLogger("s2s.proxy")


class NiceS2SProxy:
    """单连接代理：run() 后双向泵，直到任一侧断开。"""

    def __init__(self, nice_url: str, nice_api_key: str,
                 history: list[dict] | None = None, on_done=None):
        """
        history：建连时注入首帧 messages，实现「跨连接」的多轮上下文；
        on_done(msg)：每轮结束（done）回调，用于把对话落到本地库。
        两者都可为空（固件接入时就不需要）。
        """
        # 云端地址：url 尾部统一补 ?api_key=xxx。固件 URL 里的 api_key 是设备侧编译值，
        # 云端认的是服务端配置的这把钥匙；首帧 JSON 的 api_key 同样在转发前替换。
        sep = "&" if "?" in nice_url else "?"
        self._upstream_url = f"{nice_url}{sep}api_key={nice_api_key}"
        self._api_key = nice_api_key
        self._history = history or []
        self._on_done = on_done

    async def run(self, client_ws) -> None:
        """接管客户端连接：连接云端 → 双向转发 → 任一方向结束即收尾。"""
        async with connect(self._upstream_url, max_size=None,
                           ping_interval=20, ping_timeout=20) as upstream:
            first_frame_rewritten = False

            async def client_to_cloud():
                """单循环消费客户端：Starlette 的 iter_text/iter_bytes 不能并发
                （两者抢同一个 receive 队列，文本帧落进 iter_bytes 会抛 KeyError('bytes')），
                必须自己收原始 ASGI 消息再按 text/bytes 分发。"""
                nonlocal first_frame_rewritten
                while True:
                    msg = await client_ws.receive()
                    if msg.get("type") == "websocket.disconnect":
                        return
                    text = msg.get("text")
                    if text is not None:
                        # 首帧 JSON 替换 api_key 后转发；后续 JSON（listen_stop/abort/...）原样透传
                        if not first_frame_rewritten:
                            first_frame_rewritten = True
                            try:
                                frame = json.loads(text)
                                frame["api_key"] = self._api_key
                                # 本地历史注入：云端没有这个账号的历史，靠首帧 messages 带过去
                                if self._history and not frame.get("messages"):
                                    frame["messages"] = self._history
                                text = json.dumps(frame, ensure_ascii=False)
                            except json.JSONDecodeError:
                                pass
                        await upstream.send(text)
                    elif msg.get("bytes") is not None:
                        await upstream.send(msg["bytes"])

            async def cloud_to_client():
                async for msg in upstream:
                    if isinstance(msg, bytes):
                        await client_ws.send_bytes(msg)
                        continue
                    await client_ws.send_text(msg)
                    if self._on_done:
                        try:
                            d = json.loads(msg)
                        except (ValueError, TypeError):
                            continue
                        if d.get("type") in ("done", "aborted"):
                            try:
                                self._on_done(d)
                            except Exception:
                                log.exception("记录对话历史失败")

            tasks = [
                asyncio.create_task(client_to_cloud()),
                asyncio.create_task(cloud_to_client()),
            ]
            done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for t in pending:
                t.cancel()
            # 抛出先完成方向的异常（正常断连是 CancelledError/ConnectionClosed，静默收尾即可）
            for t in done:
                exc = t.exception()
                if exc and not isinstance(exc, (ConnectionClosedOK, ConnectionClosedError)):
                    raise exc
