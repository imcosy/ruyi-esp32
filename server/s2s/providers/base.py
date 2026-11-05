#!/usr/bin/env python3
"""
提供商接口约定
==============
自编排模式下三类能力的统一接口。全部为 asyncio 异步生成器/协程，
不落盘、全链路流式：

  - ASR.transcribe(pcm16k) -> str                整段识别（listen_stop 后一次性调用）
  - LLM.stream_chat(system, messages) -> async iter[str]   流式产出文本增量
  - TTS.synthesize(text) -> async iter[bytes]    流式产出 PCM int16（类属性 sample_rate）

会话编排（session.py）只认这三个接口，新增厂商只需实现对应类并在
providers/__init__.py 注册。
"""

from __future__ import annotations

import abc


class BaseASR(abc.ABC):
    """流式语音识别。输入 PCM int16 16kHz 单声道，返回识别文本。"""

    @abc.abstractmethod
    async def transcribe(self, pcm16k: bytes) -> str:
        ...


class BaseLLM(abc.ABC):
    """对话大模型。流式产出文本增量。"""

    @abc.abstractmethod
    def stream_chat(self, system_prompt: str, messages: list[dict]) -> "asyncio_generator[str]":
        ...


class BaseTTS(abc.ABC):
    """语音合成。流式产出 PCM int16 原生采样率块（sample_rate 类属性声明采样率）。

    ref：可选的参考音（音色工坊里录制的）——
        {"path": "/abs/xxx.wav", "text": "参考音对应的文字"}
        支持零样本克隆的 provider 用它来"像谁"；厂商预置音色的 provider 忽略即可。
    """

    sample_rate: int = 24000
    supports_ref: bool = False

    @abc.abstractmethod
    def synthesize(self, text: str, ref: dict | None = None) -> "asyncio_generator[bytes]":
        ...
