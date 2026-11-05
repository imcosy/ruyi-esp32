#!/usr/bin/env python3
"""提供商注册表：按配置实例化 ASR / LLM / TTS。"""

from __future__ import annotations

from .base import BaseASR, BaseLLM, BaseTTS
from .llm_openai import OpenAICompatLLM


def build_asr(cfg: dict) -> BaseASR:
    provider = cfg["asr"]["provider"]
    if provider == "doubao":
        from .asr_doubao import DoubaoASR
        return DoubaoASR(cfg["asr"]["doubao"])
    if provider == "aliyun":
        from .asr_aliyun import AliyunASR
        return AliyunASR(cfg["asr"]["aliyun"])
    raise ValueError(f"ASR 提供商无效：{provider}（自编排模式只支持 doubao/aliyun）")


def build_llm(cfg: dict) -> BaseLLM:
    provider = cfg["llm"]["provider"]
    if provider == "nice":
        raise ValueError("LLM=nice 仅在默认（整体转发）模式下可用")
    # 四家均为 OpenAI 兼容接口，一份实现按 base_url/model 区分
    return OpenAICompatLLM(cfg["llm"][provider])


def build_tts(cfg: dict) -> BaseTTS:
    provider = cfg["tts"]["provider"]
    if provider == "doubao":
        from .tts_doubao import DoubaoTTS
        return DoubaoTTS(cfg["tts"]["doubao"])
    if provider == "aliyun":
        from .tts_aliyun import AliyunTTS
        return AliyunTTS(cfg["tts"]["aliyun"])
    raise ValueError(f"TTS 提供商无效：{provider}（自编排模式只支持 doubao/aliyun）")
