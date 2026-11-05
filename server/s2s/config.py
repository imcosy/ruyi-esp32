#!/usr/bin/env python3
"""
配置管理
========
单文件 YAML（config.yaml，不入 git）承载全部运行配置：
  - server   ：监听地址/端口、允许接入的固件 api_key 白名单、admin 密码
  - nice     ：默认模式——如意云 S2S API 地址与 api_key（本服务只做透明转发）
  - asr/llm/tts：自编排模式——各能力选择的提供商 + 各厂商密钥

管理后台（/admin）读写的就是这份配置；save_config() 原子写盘。
"""

from __future__ import annotations

import copy
import threading
from pathlib import Path

import yaml

CONFIG_PATH = Path(__file__).resolve().parent.parent / "config.yaml"

_lock = threading.Lock()

DEFAULT_CONFIG: dict = {
    "server": {
        "host": "0.0.0.0",
        "port": 8892,
        # 固件首帧 api_key 白名单；留空 [] 表示不校验（自用）。上线建议填上。
        "api_keys": [],
        # 管理后台访问密码；留空表示不做鉴权（仅限本机/内网使用）
        "admin_password": "",
    },
    # 默认模式：全部能力走如意云 S2S（本服务只做固件协议 ↔ 云端协议的透明转发）
    "nice": {
        "url": "wss://dev.nice-ai.cn/s2s/",
        "api_key": "",
    },
    # ── 自编排模式：三个能力各自选择提供商，密钥在各厂商小节 ──
    # asr: nice | doubao | aliyun
    "asr": {
        "provider": "nice",
        "doubao": {"app_id": "", "access_token": "", "model": "bigmodel"},
        "aliyun": {"api_key": "", "model": "paraformer-realtime-v2"},
    },
    # llm: nice | doubao | aliyun | deepseek | zhipu（四家均为 OpenAI 兼容接口）
    "llm": {
        "provider": "nice",
        "doubao": {
            "api_key": "", "model": "doubao-seed-1.6-flash-250715",
            "base_url": "https://ark.cn-beijing.volces.com/api/v3",
        },
        "aliyun": {
            "api_key": "", "model": "qwen-plus",
            "base_url": "https://dashscope.aliyuncs.com/compatible-mode/v1",
        },
        "deepseek": {
            "api_key": "", "model": "deepseek-chat",
            "base_url": "https://api.deepseek.com/v1",
        },
        "zhipu": {
            "api_key": "", "model": "glm-4-flash",
            "base_url": "https://open.bigmodel.cn/api/paas/v4",
        },
    },
    # tts: nice | doubao | aliyun | local
    # local = 自建零样本 TTS（音色工坊录的参考音才有意义），按 HTTP 流式 PCM 对接
    "tts": {
        "provider": "nice",
        "doubao": {"app_id": "", "access_token": "", "voice": "zh_female_ruyi", "speed_ratio": 1.0},
        "aliyun": {"api_key": "", "model": "cosyvoice-v2", "voice": "longxiaochun", "speed_level": 1},
        "local": {"url": "", "sample_rate": 24000},
    },
}


def load_config() -> dict:
    """读配置并补齐 DEFAULT_CONFIG 中新增的键（旧配置文件平滑升级，不丢用户密钥）。"""
    with _lock:
        merged = copy.deepcopy(DEFAULT_CONFIG)
        if CONFIG_PATH.exists():
            with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                user = yaml.safe_load(f) or {}
            _deep_merge(merged, user)
        return merged


def save_config(cfg: dict) -> None:
    """原子写盘：先写临时文件再 rename，避免半截 YAML。"""
    with _lock:
        tmp = CONFIG_PATH.with_suffix(".yaml.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            yaml.safe_dump(cfg, f, allow_unicode=True, sort_keys=False)
        tmp.replace(CONFIG_PATH)


def _deep_merge(base: dict, patch: dict) -> None:
    for k, v in patch.items():
        if isinstance(v, dict) and isinstance(base.get(k), dict):
            _deep_merge(base[k], v)
        else:
            base[k] = v


def all_providers_ready(cfg: dict) -> tuple[bool, str]:
    """自编排模式就绪检查：返回 (是否就绪, 缺什么)。"""
    asr_p = cfg["asr"]["provider"]
    llm_p = cfg["llm"]["provider"]
    tts_p = cfg["tts"]["provider"]
    if asr_p == "nice" and llm_p == "nice" and tts_p == "nice":
        if not cfg["nice"]["api_key"]:
            return False, "如意云 api_key 未配置"
        return True, ""
    if asr_p != "nice":
        ok, miss = _asr_ready(cfg, asr_p)
        if not ok:
            return False, f"ASR({asr_p}) 缺少配置：{miss}"
    if llm_p != "nice":
        vendor = cfg["llm"][llm_p]
        if not vendor.get("api_key"):
            return False, f"LLM({llm_p}) 缺少 api_key"
    if tts_p != "nice":
        ok, miss = _tts_ready(cfg, tts_p)
        if not ok:
            return False, f"TTS({tts_p}) 缺少配置：{miss}"
    return True, ""


def _asr_ready(cfg: dict, provider: str) -> tuple[bool, str]:
    if provider == "doubao":
        d = cfg["asr"]["doubao"]
        missing = [k for k in ("app_id", "access_token") if not d.get(k)]
        return (not missing, "/".join(missing))
    if provider == "aliyun":
        a = cfg["asr"]["aliyun"]
        return (bool(a.get("api_key")), "api_key")
    return False, f"未知提供商 {provider}"


def _tts_ready(cfg: dict, provider: str) -> tuple[bool, str]:
    if provider == "doubao":
        d = cfg["tts"]["doubao"]
        missing = [k for k in ("app_id", "access_token") if not d.get(k)]
        return (not missing, "/".join(missing))
    if provider == "aliyun":
        a = cfg["tts"]["aliyun"]
        return (bool(a.get("api_key")), "api_key")
    if provider == "local":
        loc = cfg["tts"].get("local", {})
        return (bool(loc.get("url")), "url（自建 TTS 服务地址）")
    return False, f"未知提供商 {provider}"
