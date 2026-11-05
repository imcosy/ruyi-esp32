#!/usr/bin/env python3
"""
LLM 提供商：OpenAI 兼容流式接口（一份实现覆盖四家厂商）
=======================================================
豆包火山方舟 / 阿里通义 DashScope compatible-mode / DeepSeek / 智谱 GLM
四家的 chat.completions 接口均为 OpenAI 兼容协议，仅 base_url 与 model 不同，
统一用 httpx 流式请求 + SSE 解析。
"""

from __future__ import annotations

import json
from typing import AsyncIterator

import httpx

from .base import BaseLLM

# SSE 流中单块最大行长（厂商单块可能带长思考文本，64KB 足够）
_MAX_LINE = 64 * 1024


class OpenAICompatLLM(BaseLLM):
    """OpenAI 兼容 /chat/completions 流式调用。

    config 结构：{api_key, model, base_url}
    base_url 形如 https://ark.cn-beijing.volces.com/api/v3（不含 /chat/completions）。
    """

    def __init__(self, vendor_cfg: dict):
        self._api_key = vendor_cfg["api_key"]
        self._model = vendor_cfg["model"]
        self._base_url = vendor_cfg["base_url"].rstrip("/")

    async def stream_chat(self, system_prompt: str,
                          messages: list[dict]) -> AsyncIterator[str]:
        payload = {
            "model": self._model,
            "messages": ([{"role": "system", "content": system_prompt}] if system_prompt else [])
                        + messages,
            "stream": True,
        }
        headers = {
            "Authorization": f"Bearer {self._api_key}",
            "Content-Type": "application/json",
        }
        async with httpx.AsyncClient(timeout=None) as client:
            async with client.stream(
                "POST", f"{self._base_url}/chat/completions",
                json=payload, headers=headers,
            ) as resp:
                if resp.status_code != 200:
                    body = (await resp.aread()).decode("utf-8", "replace")
                    raise RuntimeError(f"LLM HTTP {resp.status_code}: {body[:500]}")
                async for line in resp.aiter_lines(_MAX_LINE):
                    if not line.startswith("data:"):
                        continue
                    data = line[5:].strip()
                    if data == "[DONE]":
                        break
                    try:
                        chunk = json.loads(data)
                    except json.JSONDecodeError:
                        continue
                    choices = chunk.get("choices")
                    if not choices:
                        continue
                    delta = choices[0].get("delta") or {}
                    text = delta.get("content")
                    if text:
                        yield text
