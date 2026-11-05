#!/usr/bin/env python3
"""
固件会话（自编排模式核心）
==========================
一条 WebSocket 连接 = 一个 FirmwareSession。实现固件侧 S2S 协议：

  首帧 JSON（认证 + 默认参数）→ Binary opus 上行 → listen_stop 触发本轮
  ASR → LLM 流式 → 分句 → TTS 流式 → opus 下行 → done/aborted

消息协议（与如意云 S2S 一致，固件零改动适配）：
  上行：首帧 JSON / hello / Binary(opus_raw 16k) / listen_stop / abort / tts_playback_done
  下行：asr_result / llm_start / llm_chunk / tts_start / tts_sentence / Binary /
        tts_sentence_end / tts_done / done / aborted / error

轮次模型：Binary 音频到达即开启新轮次并累积 PCM；listen_stop 结束本轮输入。
唤醒词（首帧 noaudio+text）直接跳过 ASR 进入 LLM。
"""

from __future__ import annotations

import asyncio
import base64
import json
import re
import time

from .audio import DOWN_SAMPLE_RATE, OpusStreamDecoder, OpusStreamEncoder
from .config import all_providers_ready
from .providers import build_asr, build_llm, build_tts
from .store import get_ai, get_voice

# 对话历史上限（含 system 之外的消息条数）
_MAX_HISTORY = 20
# LLM 文本分句：强标点即断句（逗号不断，避免碎句拖慢首包）
_SENT_SPLIT = re.compile(r"(?<=[。！？!?；;\n])")
# 单句超长强制截断长度（LLM 不吐标点时兜不了底就按长度切，保证 TTS 起播节奏）
_MAX_SENT_CHARS = 60


class FirmwareSession:
    def __init__(self, ws, cfg: dict):
        self._ws = ws
        self._cfg = cfg
        self._round = 0                 # 当前轮次（从 1 开始）
        self._round_open = False        # 是否正在收集本轮上行音频（首个 binary 帧开轮，listen_stop 关轮）
        self._running: asyncio.Task | None = None   # 本轮 ASR→LLM→TTS 流水线任务
        self._abort_evt = asyncio.Event()
        self._asr_pcm = bytearray()     # 本轮累积的 PCM 16k
        self._upstream_format = "opus_raw"  # opus_raw=固件（需解码）；pcm=浏览器 demo（直用）
        self._decoder = OpusStreamDecoder(16000)
        self._messages: list[dict] = []  # 对话历史（OpenAI 格式）
        self._system_prompt = ""
        self._language = "zh"
        self._voice = ""                # 固件首帧带的音色偏好（厂商侧映射见各 provider）
        self._voice_ref: dict | None = None  # 音色工坊的参考音（零样本 TTS 用）
        self._stream_text = True
        self._client_type = ""
        self._board = ""
        self._has_aec = None
        # 会话内独立的三个提供商实例（密钥来自当前配置）
        self._asr = None
        self._llm = None
        self._tts = None

    # ── 连接主循环 ──────────────────────────────────────────────

    async def run(self) -> None:
        first_frame_processed = False
        async for msg in self._ws:
            if isinstance(msg, bytes):
                if not self._round_open and (self._running is None or self._running.done()):
                    self._round += 1
                    self._round_open = True
                if self._upstream_format == "pcm":
                    # 浏览器 demo 客户端：AudioWorklet 采出的 PCM int16 16k 直接送 ASR
                    self._asr_pcm += msg
                else:
                    self._asr_pcm += self._decoder.feed(msg)
                continue
            try:
                frame = json.loads(msg)
            except json.JSONDecodeError:
                await self._send({"type": "error", "message": f"JSON 解析失败: {msg[:120]}"})
                continue

            mtype = frame.get("type", "")
            if not first_frame_processed:
                first_frame_processed = True
                await self._handle_first_frame(frame)  # 认证 + 提供商初始化 + 参数
            await self._dispatch(frame)

        # 连接关闭：收掉在跑的流水线
        if self._running and not self._running.done():
            self._running.cancel()

    # ── 首帧 ────────────────────────────────────────────────────

    async def _handle_first_frame(self, frame: dict) -> None:
        api_keys = self._cfg["server"]["api_keys"]
        if api_keys and frame.get("api_key") not in api_keys:
            await self._send({"type": "error", "message": "api_key 校验失败"})
            raise ConnectionAbortedError("api_key 校验失败")

        ok, missing = all_providers_ready(self._cfg)
        if not ok:
            await self._send({"type": "error", "message": f"服务端配置不完整：{missing}（请在 /admin 补全）"})
            raise ConnectionAbortedError(f"配置不完整：{missing}")

        self._asr = build_asr(self._cfg)
        self._llm = build_llm(self._cfg)
        self._tts = build_tts(self._cfg)
        self._apply_overrides(frame)
        self._client_type = frame.get("client_type", "")
        # AI 角色（音色工坊里创建）：人设提示词 + 绑定音色，一并在首帧生效
        ai_id = int(frame.get("ai_id") or 0)
        if ai_id:
            ai = get_ai(ai_id)
            if ai:
                if ai.get("prompt"):
                    self._system_prompt = ai["prompt"]
                self._bind_studio_voice(ai.get("voice_id") or 0)
        if not self._system_prompt:
            self._system_prompt = self._default_system_prompt()

    def _bind_studio_voice(self, voice_id: int) -> None:
        """把音色工坊的参考音绑到本会话（provider 不支持参考音时自动忽略）。"""
        if not voice_id:
            return
        v = get_voice(int(voice_id))
        if v:
            self._voice_ref = {"path": v["ref_path"], "text": v.get("ref_text") or ""}

    # ── 消息分发（首帧与非首帧共用同一张路由表） ────────────────

    async def _dispatch(self, frame: dict) -> None:
        mtype = frame.get("type", "")
        if mtype == "hello":
            self._board = frame.get("board", self._board)
            self._has_aec = frame.get("has_aec")
        elif mtype == "listen_stop":
            await self._on_listen_stop()
        elif mtype == "abort":
            await self._on_abort()
        elif mtype == "tts_playback_done":
            pass  # 设备端播完通知；自编排模式按 abort 打断，无需额外处理
        elif frame.get("audio") or frame.get("image"):
            # 单帧一轮（浏览器/手机端）：整段音频 base64 + 可选图片放同一个 JSON 帧，
            # 与固件的 binary + listen_stop 等价，多模态图片随本轮一起送 LLM
            await self._on_round_frame(frame)
        elif frame.get("noaudio") and frame.get("text"):
            # 唤醒词：固件在连接中途重发首帧（noaudio+text，app.cc 唤醒路径），
            # 或连接首帧直接带 noaudio+text——都跳过 ASR 直接 LLM
            self._apply_overrides(frame)
            if self._running and not self._running.done():
                await self._on_abort()
            self._round += 1
            self._running = asyncio.create_task(self._run_pipeline(frame["text"]))
        elif "voice" in frame or "system_prompt" in frame or "messages" in frame:
            # 纯参数帧（固件按键触发等场景会重发首帧配置，无 noaudio）
            self._apply_overrides(frame)
        # 其余消息忽略

    def _apply_overrides(self, frame: dict) -> None:
        self._language = frame.get("language", self._language)
        # format: opus_raw（固件默认）| pcm（浏览器 demo，上行二进制为 PCM int16 16k）
        if frame.get("format") in ("opus_raw", "pcm"):
            self._upstream_format = frame["format"]
        if frame.get("voice"):
            self._voice = str(frame["voice"])
            # 显式引用音色工坊的音色：voice="studio:3"
            m = re.match(r"^studio:(\d+)$", self._voice)
            if m:
                self._bind_studio_voice(int(m.group(1)))
        self._stream_text = bool(frame.get("stream_text", self._stream_text))
        if frame.get("system_prompt"):
            self._system_prompt = frame["system_prompt"]
        if isinstance(frame.get("messages"), list) and frame["messages"]:
            self._messages = list(frame["messages"])

    def _default_system_prompt(self) -> str:
        return (
            "你是如意，一个聪明友善的中文语音助手。回答必须口语化、简短（一般不超过两句话），"
            "适合直接朗读给用户听，不要用 Markdown、不要用列表符号。"
        )

    # ── 轮次触发 / 打断 ─────────────────────────────────────────

    async def _on_listen_stop(self) -> None:
        if self._running and not self._running.done():
            # 上一轮还在跑：设备端应先 abort 再继续说话，这里直接打断旧轮
            await self._on_abort()
        pcm = bytes(self._asr_pcm)
        self._asr_pcm.clear()
        if not pcm:
            self._round_open = False
            return  # 空轮（误触）不消耗轮次
        # 轮次号已由首个 binary 帧开启；若从未收到音频直接 listen_stop，则不开新轮
        self._round_open = False
        self._running = asyncio.create_task(self._run_pipeline(pcm))

    async def _on_round_frame(self, frame: dict) -> None:
        """单帧一轮：audio=base64 PCM int16（sample_rate 由首帧声明，默认 16k），image=base64 JPEG。"""
        if self._running and not self._running.done():
            await self._on_abort()
        self._apply_overrides(frame)
        audio_b64 = frame.get("audio") or ""
        pcm = base64.b64decode(audio_b64) if audio_b64 else b""
        text = (frame.get("text") or "").strip()
        if not pcm and not text:
            return  # 空轮（既无音频也无文本）
        self._round += 1
        self._running = asyncio.create_task(
            self._run_pipeline(pcm if pcm else text, frame.get("image") or ""))

    async def _on_abort(self) -> None:
        if self._running and not self._running.done():
            self._abort_evt.set()
            self._running.cancel()
            try:
                await self._running
            except (asyncio.CancelledError, Exception):
                pass
            self._running = None
            self._abort_evt.clear()

    # ── 流水线：ASR → LLM → 分句 → TTS → 下发 ──────────────────

    async def _run_pipeline(self, asr_input, image_b64: str = "") -> None:
        """asr_input: str（noaudio 文本）或 bytes（PCM 16k）；image_b64: 本轮随行的图片（可选）。"""
        rnd = self._round
        started = time.monotonic()
        asr_text = asr_input if isinstance(asr_input, str) else ""
        llm_text = ""
        try:
            if not isinstance(asr_input, str):
                asr_text = await self._asr.transcribe(asr_input)
                if self._abort_evt.is_set():
                    raise asyncio.CancelledError
            if not asr_text.strip() and not image_b64:
                await self._send({"type": "done", "round": rnd, "asr_text": "",
                                  "llm_text": "", "total_ms": self._ms(started)})
                return
            await self._send({"type": "asr_result", "round": rnd, "text": asr_text})

            await self._send({"type": "llm_start", "round": rnd})
            history = self._messages[-_MAX_HISTORY:]
            if image_b64:
                # 多模态：图片只挂在本轮 user 消息上，不写进对话历史（避免历史膨胀）
                history = history + [{"role": "user", "content": [
                    {"type": "text", "text": asr_text or "（用户发来一张图片，请看看）"},
                    {"type": "image_url",
                     "image_url": {"url": f"data:image/jpeg;base64,{image_b64}"}},
                ]}]
            full_text, sentence_iter = "", self._stream_llm_sentences(history)
            sent_index = 0
            encoder: OpusStreamEncoder | None = None
            tts_started = False

            async for sentence in sentence_iter:
                if self._abort_evt.is_set():
                    raise asyncio.CancelledError
                full_text += sentence
                llm_text += sentence
                if not tts_started:
                    await self._send({"type": "tts_start", "round": rnd,
                                      "sample_rate": DOWN_SAMPLE_RATE,
                                      "format": "opus_raw", "channels": 1})
                    tts_started = True
                await self._send({"type": "tts_sentence", "round": rnd,
                                  "index": sent_index, "text": sentence})
                if encoder is None:
                    encoder = OpusStreamEncoder(self._tts.sample_rate)
                async for pcm in self._tts.synthesize(sentence, ref=self._voice_ref):
                    if self._abort_evt.is_set():
                        raise asyncio.CancelledError
                    opus = encoder.feed(pcm)
                    if opus:
                        await self._ws.send_bytes(opus)
                await self._send({"type": "tts_sentence_end", "round": rnd, "index": sent_index})
                sent_index += 1

            if tts_started and encoder is not None:
                tail = encoder.flush()
                if tail:
                    await self._ws.send_bytes(tail)
                await self._send({"type": "tts_done", "round": rnd})

            # 写入历史
            self._messages.append({"role": "user", "content": asr_text})
            self._messages.append({"role": "assistant", "content": full_text.strip()})
            if len(self._messages) > _MAX_HISTORY * 2:
                self._messages = self._messages[-_MAX_HISTORY * 2:]

            await self._send({"type": "done", "round": rnd, "asr_text": asr_text,
                              "llm_text": full_text.strip(), "total_ms": self._ms(started)})
        except asyncio.CancelledError:
            await self._send({"type": "aborted", "round": rnd, "asr_text": asr_text,
                              "llm_text": llm_text})
            raise
        except Exception as exc:  # 厂商接口/网络错误：原样透传给固件显示
            await self._send({"type": "error", "round": rnd, "message": str(exc)})

    async def _stream_llm_sentences(self, history: list[dict]):
        """LLM 流式增量 → 按标点切句 → 逐句 yield（打断时抛 CancelledError）。"""
        buf = ""

        def split_buf(text: str) -> list[str]:
            parts = [p for p in _SENT_SPLIT.split(text) if p]
            # 超长无标点块按 _MAX_SENT_CHARS 硬切
            out = []
            for p in parts:
                while len(p) > _MAX_SENT_CHARS:
                    out.append(p[:_MAX_SENT_CHARS])
                    p = p[_MAX_SENT_CHARS:]
                if p:
                    out.append(p)
            return out

        async for chunk in self._llm.stream_chat(self._system_prompt, history):
            if self._abort_evt.is_set():
                raise asyncio.CancelledError
            if self._stream_text:
                await self._send({"type": "llm_chunk", "text": chunk})
            buf += chunk
            if _SENT_SPLIT.search(buf) or len(buf) > _MAX_SENT_CHARS:
                sentences = split_buf(buf)
                # 最后一段可能是不完整句（还没遇到标点），留在缓冲
                tail = sentences.pop() if sentences and not _ends_with_break(sentences[-1]) else None
                for s in sentences:
                    yield s
                buf = tail or ""

        if buf.strip():
            yield buf

    # ── 杂项 ────────────────────────────────────────────────────

    async def _send(self, obj: dict) -> None:
        await self._ws.send_text(json.dumps(obj, ensure_ascii=False))

    @staticmethod
    def _ms(started: float) -> int:
        return int((time.monotonic() - started) * 1000)


def _ends_with_break(sentence: str) -> bool:
    return bool(sentence) and sentence[-1] in "。！？!?；;\n"
