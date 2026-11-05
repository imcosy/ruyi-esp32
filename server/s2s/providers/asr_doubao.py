#!/usr/bin/env python3
"""
字节火山引擎 · 流式语音识别大模型（SAUC bigmodel）
==================================================
文档：https://www.volcengine.com/docs/6561/1354829（大模型流式语音识别 WebSocket）
endpoint：wss://openspeech.bytedance.com/api/v3/sauc/bigmodel

二进制协议（v4，4 字节头，逐字段）：
  byte0 = (protocol version 4bit << 4) | header size 4bit（单位 4 字节）→ 0x41
  byte1 = (message type 4bit << 4) | flags 4bit
          message type：0b0001 full client request / 0b0010 audio-only request /
                        0b1001 full server response / 0b1011 error response
          flags：0b0000 无序列号 / 0b0001 正序列号 / 0b0010 末包无序列号 /
                 0b0011 末包带负序列号
  byte2 = (serialization 4bit << 4) | compression 4bit
          serialization：0b0001 JSON；compression：0b0001 gzip
  byte3 = 保留 0x00
载荷组织：header → [序列号 4B（flags 带序列号时）] → 载荷长度 4B(BE) → gzip(数据)
识别结果为**累计全文**：{"result":{"text":"目前识别到的全文"}}
"""

from __future__ import annotations

import asyncio
import gzip
import json
import struct
import uuid

import websockets

from .base import BaseASR

_URL = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel"
_RESOURCE_ID = "volc.bigasr.sauc.duration"  # 按时长计费的并发资源
# 300ms 一包（16k×2B×0.3s = 9600B），兼顾延迟与帧开销
_CHUNK_BYTES = 9600


def _header(msg_type: int, flags: int, compression: int = 0x1) -> bytes:
    return bytes([
        0x41,                          # version=4, header_size=1（4 字节）
        (msg_type << 4) | flags,
        (0x1 << 4) | compression,      # serialization=JSON
        0x00,
    ])


def _gzip(data: bytes) -> bytes:
    return gzip.compress(data)


class DoubaoASR(BaseASR):
    """config：{app_id, access_token, model}"""

    def __init__(self, cfg: dict):
        self._app_id = cfg["app_id"]
        self._token = cfg["access_token"]
        self._model = cfg.get("model", "bigmodel")

    async def transcribe(self, pcm16k: bytes) -> str:
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
            # ① full client request：任务参数
            req = {
                "user": {"uid": "sisi-esp"},
                "audio": {"format": "pcm", "codec": "raw", "rate": 16000,
                          "bits": 16, "channel": 1},
                "request": {"model_name": self._model, "enable_punc": True,
                            "enable_itn": True, "show_utterances": False},
            }
            payload = _gzip(json.dumps(req).encode())
            await ws.send(_header(0b0001, 0b0000) + struct.pack(">I", len(payload)) + payload)

            # ② audio-only request 流式喂音频（正序列号），末包负序列号
            seq = 1
            for pos in range(0, len(pcm16k), _CHUNK_BYTES):
                chunk = pcm16k[pos:pos + _CHUNK_BYTES]
                is_last = pos + _CHUNK_BYTES >= len(pcm16k)
                body = _gzip(chunk)
                if is_last:
                    frame = (_header(0b0010, 0b0011)
                             + struct.pack(">i", -seq)
                             + struct.pack(">I", len(body)) + body)
                else:
                    frame = (_header(0b0010, 0b0001)
                             + struct.pack(">i", seq)
                             + struct.pack(">I", len(body)) + body)
                await ws.send(frame)
                seq += 1

            # ③ 收响应直到连接关闭；result.text 是累计全文，取最后一次
            final_text = ""
            async for msg in ws:
                text = _parse_response(msg)
                if text:
                    final_text = text
        return final_text.strip()


def _parse_response(msg) -> str | None:
    """解析服务端响应帧，返回识别文本（无文本返回 None）。"""
    if isinstance(msg, str):
        raise RuntimeError(f"SAUC 意外文本帧：{msg[:200]}")
    if len(msg) < 4:
        raise RuntimeError("SAUC 响应帧过短")
    msg_type = msg[1] >> 4
    flags = msg[1] & 0x0F
    compression = msg[2] & 0x0F
    if msg_type == 0b1011:  # error response
        raise RuntimeError(f"SAUC 错误响应：{_extract_payload(msg, flags, compression)[:300]}")
    if msg_type != 0b1001:  # full server response 之外（ack 等）忽略
        return None
    body = _extract_payload(msg, flags, compression)
    data = json.loads(gzip.decompress(body) if compression == 0b0001 else body)
    return data.get("result", {}).get("text") or None


def _extract_payload(msg: bytes, flags: int, compression: int) -> bytes:
    """header → [序列号 4B] → 载荷长度 4B → 载荷。"""
    offset = 4
    if flags & 0b0001 or flags & 0b0010:  # 带序列号（含负序列号）
        offset += 4
    size = struct.unpack(">I", msg[offset:offset + 4])[0]
    return msg[offset + 4: offset + 4 + size]
