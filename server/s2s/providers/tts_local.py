#!/usr/bin/env python3
"""
自建零样本 TTS（local）
=======================
把「音色工坊」里录制的参考音，交给任意支持零样本克隆的本地/自建 TTS 服务去合成。
服务只需要实现一个 HTTP 接口：

  POST <url>
  Content-Type: application/json
  {
    "text": "要合成的文字",
    "format": "pcm",              # 固定：返回裸 PCM，便于服务端直接编码下发
    "sample_rate": 24000,         # 本 provider 声明的采样率，服务端按它建 opus 编码器
    "ref_audio": "<base64 wav>",  # 可选：参考音（16k 单声道 wav），不带则按默认音色
    "ref_text":  "参考音对应的文字"  # 可选：零样本模型用它提升相似度
  }

  200 响应体：PCM int16 单声道裸流（可流式 chunked 返回，服务端边收边编码）

可对接：CosyVoice2 / IndexTTS / VoxCPM / GPT-SoVITS 等自建推理服务，
外面套一层上面这个协议即可；不需要改本项目的任何代码。
"""

from __future__ import annotations

import base64
import os

import httpx

from .base import BaseTTS


class LocalTTS(BaseTTS):
    """config：{url, sample_rate}"""

    supports_ref = True

    def __init__(self, cfg: dict):
        self._url = (cfg.get("url") or "").strip()
        self.sample_rate = int(cfg.get("sample_rate") or 24000)
        if not self._url:
            raise ValueError("tts.local.url 未配置")

    async def synthesize(self, text: str, ref: dict | None = None):
        if not text.strip():
            return
        payload = {"text": text, "format": "pcm", "sample_rate": self.sample_rate}
        if ref and ref.get("path") and os.path.isfile(ref["path"]):
            with open(ref["path"], "rb") as f:
                payload["ref_audio"] = base64.b64encode(f.read()).decode()
            payload["ref_text"] = ref.get("text") or ""

        async with httpx.AsyncClient(timeout=None) as client:
            async with client.stream("POST", self._url, json=payload) as resp:
                if resp.status_code != 200:
                    body = (await resp.aread()).decode("utf-8", "replace")
                    raise RuntimeError(f"TTS HTTP {resp.status_code}: {body[:300]}")
                async for chunk in resp.aiter_bytes(4096):
                    if chunk:
                        yield chunk
