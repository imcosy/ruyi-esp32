#!/usr/bin/env python3
"""
字节火山引擎 · 豆包语音合成大模型（TTS，单向流式）
==================================================
文档：https://www.volcengine.com/docs/6561/1329555（大模型语音合成单向流式 WebSocket）
endpoint：wss://openspeech.bytedance.com/api/v3/tts/unidirectional/stream

二进制协议与 SAUC 相同（v4 4 字节头，见 asr_doubao.py 顶部说明）：
  请求：full client request，JSON {"user":..., "req_params":{text/speaker/audio_params}}
  响应：0b0100 audio-only（载荷 = PCM 音频），0b1001 full server response（合成结束），
        0b1011 error response
音频帧是否 gzip 由帧头 compression 半字节决定（0b0001 = gzip）。
"""

from __future__ import annotations

import gzip
import json
import struct
import uuid

import websockets

from .base import BaseTTS

_URL = "wss://openspeech.bytedance.com/api/v3/tts/unidirectional/stream"
_RESOURCE_ID = "volc.service_type.10029"
SAMPLE_RATE = 24000


class DoubaoTTS(BaseTTS):
    """config：{app_id, access_token, voice, speed_ratio}"""

    sample_rate = SAMPLE_RATE

    def __init__(self, cfg: dict):
        self._app_id = cfg["app_id"]
        self._token = cfg["access_token"]
        self._voice = cfg.get("voice", "zh_female_ruyi")
        self._speed = float(cfg.get("speed_ratio", 1.0))

    async def synthesize(self, text: str, ref: dict | None = None):
        if not text.strip():
            return
        async with websockets.connect(
            _URL,
            additional_headers={
                "X-Api-App-Key": self._app_id,
                "X-Api-Access-Key": self._token,
                "X-Api-Resource-Id": _RESOURCE_ID,
                "X-Api-Request-Id": str(uuid.uuid4()),
            },
            max_size=None,
        ) as ws:
            req = {
                "user": {"uid": "sisi-esp"},
                "req_params": {
                    "text": text,
                    "speaker": self._voice,
                    "audio_params": {"format": "pcm", "sample_rate": SAMPLE_RATE,
                                     "channel": 1, "bits": 16},
                    "additions": {"speed_ratio": self._speed},
                },
            }
            payload = gzip.compress(json.dumps(req).encode())
            header = bytes([0x41, (0b0001 << 4) | 0b0000, (0b0001 << 4) | 0b0001, 0x00])
            await ws.send(header + struct.pack(">I", len(payload)) + payload)

            async for msg in ws:
                if isinstance(msg, str):
                    raise RuntimeError(f"TTS 意外文本帧：{msg[:200]}")
                if len(msg) < 4:
                    raise RuntimeError("TTS 响应帧过短")
                msg_type = msg[1] >> 4
                flags = msg[1] & 0x0F
                compression = msg[2] & 0x0F
                if msg_type == 0b1011:
                    raise RuntimeError(f"豆包 TTS 错误响应：{_payload(msg, flags, compression)[:300]}")
                if msg_type == 0b0100:  # audio-only：产出 PCM
                    audio = _payload(msg, flags, compression)
                    if compression == 0b0001:
                        audio = gzip.decompress(audio)
                    if audio:
                        yield audio
                # 0b1001 = 合成结束的 full server response，循环自然结束（服务端随后关流）
        return


def _payload(msg: bytes, flags: int, compression: int) -> bytes:
    offset = 4
    if flags & 0b0001 or flags & 0b0010:
        offset += 4
    size = struct.unpack(">I", msg[offset:offset + 4])[0]
    return msg[offset + 4: offset + 4 + size]
