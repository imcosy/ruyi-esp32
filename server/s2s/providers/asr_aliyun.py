#!/usr/bin/env python3
"""
阿里云百炼 · Paraformer 实时语音识别
====================================
文档：https://help.aliyun.com/zh/model-studio/real-time-speech-recognition（Paraformer 实时语音识别 WebSocket）
endpoint：wss://dashscope.aliyuncs.com/api-ws/v1/inference
鉴权：HTTP 头 Authorization: bearer <api-key>

协议：JSON 文本帧（任务控制）+ Binary 帧（音频）。
  控制帧：{"header":{"action":"run-task|continue-task|finish-task|...","task_id":uuid,
           "streaming":"out"},"payload":{...}}
  服务端事件：task-started / result-generated / task-finished / task-failed
  音频帧：4 字节头 + 原始 PCM：
    byte0 = 0x11（version=1, header size=1）
    byte1 = 0x20（message type=audio-only request, flags=无序列号）
    byte2 = 0x00（无序列化、无压缩）
    byte3 = 0x00
识别结果按句增量返回（payload.output.sentence），同 begin_time 的句子是同一句的
中间结果——以 begin_time 为键收集，最后一版文本按时间顺序拼接即为全文。
"""

from __future__ import annotations

import asyncio
import json
import struct
import uuid

import websockets

from .base import BaseASR

_URL = "wss://dashscope.aliyuncs.com/api-ws/v1/inference"
# 300ms 一包
_CHUNK_BYTES = 9600
_AUDIO_HEADER = bytes([0x11, 0x20, 0x00, 0x00])


class AliyunASR(BaseASR):
    """config：{api_key, model}"""

    def __init__(self, cfg: dict):
        self._key = cfg["api_key"]
        self._model = cfg.get("model", "paraformer-realtime-v2")

    async def transcribe(self, pcm16k: bytes) -> str:
        task_id = str(uuid.uuid4())
        sentences: dict[int, str] = {}  # begin_time → 最新句子文本
        finished = asyncio.Event()

        async with websockets.connect(
            _URL, additional_headers={"Authorization": f"bearer {self._key}"},
            max_size=None,
        ) as ws:
            await ws.send(json.dumps({
                "header": {"action": "run-task", "task_id": task_id, "streaming": "out"},
                "payload": {
                    "model": self._model,
                    "task_group": "asr", "task": "asr", "function": "recognition",
                    "parameters": {"format": "pcm", "sample_rate": 16000,
                                   "language_hints": ["zh", "en"]},
                    "input": {},
                },
            }, ensure_ascii=False))

            async def feeder():
                # 等 task-started 再喂音频
                while True:
                    ev = json.loads(await ws.recv())
                    if ev["header"]["event"] == "task-started":
                        break
                    if ev["header"]["event"] == "task-failed":
                        raise RuntimeError(f"Paraformer task-failed: {ev['payload']}")
                for pos in range(0, len(pcm16k), _CHUNK_BYTES):
                    await ws.send(_AUDIO_HEADER + pcm16k[pos:pos + _CHUNK_BYTES])
                await ws.send(json.dumps({
                    "header": {"action": "finish-task", "task_id": task_id,
                               "streaming": "out"},
                    "payload": {"input": {}},
                }, ensure_ascii=False))

            feeder_task = asyncio.create_task(feeder())
            try:
                async for raw in ws:
                    if isinstance(raw, bytes):
                        continue  # 本任务无音频下行
                    ev = json.loads(raw)
                    event = ev["header"]["event"]
                    if event == "result-generated":
                        sent = ev["payload"].get("output", {}).get("sentence")
                        if sent and sent.get("begin_time") is not None:
                            sentences[sent["begin_time"]] = sent.get("text", "")
                    elif event == "task-failed":
                        raise RuntimeError(f"Paraformer task-failed: {ev['payload']}")
                    elif event == "task-finished":
                        finished.set()
                        break
            finally:
                feeder_task.cancel()
        return "".join(sentences[k] for k in sorted(sentences)).strip()
