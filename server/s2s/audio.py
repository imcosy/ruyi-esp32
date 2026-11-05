#!/usr/bin/env python3
"""
音频编解码
==========
固件 ↔ 服务端 ↔ 厂商 API 三段音频格式转换的唯一实现处：

  上行：固件 → opus_raw 裸流（16kHz 单声道，[2字节小端长度][opus帧]×N）
        → OpusStreamDecoder 流式解码 → PCM int16 16k（送 ASR）
  下行：厂商 TTS PCM（各采样率）→ 重采样到 48k → OpusStreamEncoder
        → opus_raw 裸流 → Binary 帧下发固件

下行固定 48kHz/64kbps（与如意云 s2s_service 生产行为完全一致，
固件按 tts_start.sample_rate 配置解码器，opus 解码后由设备端重采样到喇叭速率）。
"""

from __future__ import annotations

import audioop
import struct

import opuslib

# 下行 TTS 采样率：opus 编码统一 48k（opuslib 支持的最低 8k~48k 任选，48k 与生产一致）
DOWN_SAMPLE_RATE = 48000
DOWN_BITRATE = 64000
# opus 帧长 20ms
FRAME_MS = 20


def unpack_opus_frames(payload: bytes) -> list[bytes]:
    """拆 opus_raw 裸流：[len_lo, len_hi][opus_frame]... → opus 帧列表。

    半帧（TCP 分片截断）留在缓冲，由调用方拼下一包后重拆。
    """
    frames = []
    pos = 0
    total = len(payload)
    while total - pos >= 2:
        flen = payload[pos] | (payload[pos + 1] << 8)
        if total - pos - 2 < flen:
            break
        if flen > 0:
            frames.append(payload[pos + 2: pos + 2 + flen])
        pos += 2 + flen
    return frames, payload[pos:]  # type: ignore[return-value]


class OpusStreamDecoder:
    """上行 opus_raw 流式解码器：喂 opus 裸流（含跨包半帧），产出 PCM int16 16k。"""

    def __init__(self, sample_rate: int = 16000):
        self._rate = sample_rate
        self._dec = opuslib.Decoder(sample_rate, 1)
        self._buf = b""

    def feed(self, payload: bytes) -> bytes:
        """喂一包 binary（可能含多个整帧+一个半帧），返回累计解码出的 PCM。"""
        self._buf += payload
        frames, self._buf = unpack_opus_frames(self._buf)
        if not frames:
            return b""
        chunks = []
        for frame in frames:
            chunks.append(self._dec.decode(frame, FRAME_MS * self._rate // 1000, False))
        return b"".join(chunks)


class OpusStreamEncoder:
    """下行 opus_raw 流式编码器：喂 PCM int16（任意采样率）→ 重采样 48k → 凑 20ms 帧 → opus 裸流。

    opuslib 编码器按帧走：内部维护 48k 下的残样缓冲，feed 任意长度 PCM 都能产出
    对齐 20ms 的裸流。末尾 flush() 补零凑出最后一帧。
    """

    def __init__(self, src_sample_rate: int):
        self._src_rate = src_sample_rate
        self._enc = opuslib.Encoder(DOWN_SAMPLE_RATE, 1, opuslib.APPLICATION_AUDIO)
        self._enc.bitrate = DOWN_BITRATE
        # 20ms @48k = 960 样本 = 1920 字节
        self._frame_bytes = DOWN_SAMPLE_RATE * 2 * FRAME_MS // 1000
        self._pcm_buf = b""
        # 重采样状态（audioop.ratecv 需要跨块维持 state）
        self._resample_state = None

    def feed(self, pcm: bytes) -> bytes:
        """喂一段源采样率 PCM int16，返回 opus_raw 裸流（0~N 个完整帧）。"""
        if not pcm:
            return b""
        converted, self._resample_state = audioop.ratecv(
            pcm, 2, 1, self._src_rate, DOWN_SAMPLE_RATE, self._resample_state)
        self._pcm_buf += converted
        n_frames = len(self._pcm_buf) // self._frame_bytes
        if n_frames == 0:
            return b""
        out = []
        for i in range(n_frames):
            frame = self._enc.encode(self._pcm_buf[i * self._frame_bytes:(i + 1) * self._frame_bytes],
                                     self._frame_bytes // 2)
            out.append(struct.pack("<H", len(frame)) + frame)
        self._pcm_buf = self._pcm_buf[n_frames * self._frame_bytes:]
        return b"".join(out)

    def flush(self) -> bytes:
        """收尾：残样补零凑最后一帧。"""
        if not self._pcm_buf:
            return b""
        self._pcm_buf += b"\x00" * (self._frame_bytes - len(self._pcm_buf))
        frame = self._enc.encode(self._pcm_buf, self._frame_bytes // 2)
        self._pcm_buf = b""
        return struct.pack("<H", len(frame)) + frame
