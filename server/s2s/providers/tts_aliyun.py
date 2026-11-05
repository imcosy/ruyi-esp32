#!/usr/bin/env python3
"""
阿里云百炼 · CosyVoice 流式语音合成
====================================
文档：https://help.aliyun.com/zh/model-studio/cosyvoice-websocket-api（CosyVoice WebSocket API）
endpoint：wss://dashscope.aliyuncs.com/api-ws/v1/inference
鉴权：Authorization: bearer <api-key>

协议（与 asr_aliyun.py 同一套 DashScope WebSocket 协议）：
  run-task（parameters 带 voice/format/sample_rate）→ task-started
  → continue-task 喂文本 → finish-task
  → 音频以 Binary 帧返回（4 字节头 + 载荷长度 4B + PCM），task-finished 结束
"""

from __future__ import annotations

import json
import struct
import uuid

import websockets

from .base import BaseTTS

_URL = "wss://dashscope.aliyuncs.com/api-ws/v1/inference"
SAMPLE_RATE = 24000


class AliyunTTS(BaseTTS):
    """config：{api_key, model, voice, speed_level}"""

    sample_rate = SAMPLE_RATE

    def __init__(self, cfg: dict):
        self._key = cfg["api_key"]
        self._model = cfg.get("model", "cosyvoice-v2")
        self._voice = cfg.get("voice", "longxiaochun")
        self._speed = float(cfg.get("speed_level", 1))

    async def synthesize(self, text: str):
        if not text.strip():
            return
        task_id = str(uuid.uuid4())
        async with websockets.connect(
            _URL, additional_headers={"Authorization": f"bearer {self._key}"},
            max_size=None,
        ) as ws:
            await ws.send(json.dumps({
                "header": {"action": "run-task", "task_id": task_id, "streaming": "out"},
                "payload": {
                    "model": self._model,
                    "task_group": "tts", "task": "tts", "function": "SpeechSynthesizer",
                    "parameters": {
                        "text_type": "PlainText", "voice": self._voice,
                        "format": "pcm", "sample_rate": SAMPLE_RATE,
                        "volume": 50, "rate": self._speed, "pitch": 1,
                    },
                    "input": {},
                },
            }, ensure_ascii=False))

            async for raw in ws:
                if isinstance(raw, bytes):
                    # 音频帧：4B 头 + 4B 载荷长度 + PCM（本接口音频无压缩）
                    if len(raw) < 8:
                        raise RuntimeError(f"CosyVoice 音频帧过短：{len(raw)}B")
                    size = struct.unpack(">I", raw[4:8])[0]
                    audio = raw[8:8 + size]
                    if audio:
                        yield audio
                    continue
                ev = json.loads(raw)
                event = ev["header"]["event"]
                if event == "task-started":
                    await ws.send(json.dumps({
                        "header": {"action": "continue-task", "task_id": task_id,
                                   "streaming": "in"},
                        "payload": {"input": {"text": text}},
                    }, ensure_ascii=False))
                    await ws.send(json.dumps({
                        "header": {"action": "finish-task", "task_id": task_id,
                                   "streaming": "out"},
                        "payload": {"input": {}},
                    }, ensure_ascii=False))
                elif event == "task-failed":
                    raise RuntimeError(f"CosyVoice task-failed: {ev['payload']}")
                elif event == "task-finished":
                    return
        return
