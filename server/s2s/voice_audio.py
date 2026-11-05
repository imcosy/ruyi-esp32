#!/usr/bin/env python3
"""
参考音频处理（纯标准库 + 可选 ffmpeg）
======================================
用户上传或现场录的那段话，直接拿去做零样本 TTS 的参考音通常不够干净：
底噪、直流偏移、首尾静音都会影响克隆效果。这里做三件事：

  1. 解码：wav 用标准库，其它格式（mp3/m4a/webm）交给 ffmpeg 转 16k 单声道
  2. 降噪：80Hz 高通 + 分帧噪声门（噪声底取帧能量 20 分位数，平滑增益）
  3. 规整：裁掉首尾静音 + 峰值归一化

全部内存处理，产出 16kHz / 单声道 / int16 的 WAV 字节。
不引入 numpy，开源用户装好依赖就能跑。
"""

from __future__ import annotations

import array
import io
import math
import subprocess
import wave

SAMPLE_RATE = 16000
FRAME = 320          # 20ms @16k


class AudioError(ValueError):
    """输入音频无法解码 / 不适合作为参考音。"""


# ─── 解码 ──────────────────────────────────────────────────────

def decode_to_pcm(raw: bytes) -> bytes:
    """任意常见音频 → 16kHz / 单声道 / int16 PCM。"""
    if not raw:
        raise AudioError("音频为空")
    if raw[:4] == b"RIFF" and raw[8:12] == b"WAVE":
        return _decode_wav(raw)
    return _decode_with_ffmpeg(raw)


def _decode_wav(raw: bytes) -> bytes:
    try:
        with wave.open(io.BytesIO(raw), "rb") as w:
            channels, width, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
            frames = w.readframes(w.getnframes())
    except (wave.Error, EOFError) as exc:
        raise AudioError(f"WAV 解析失败：{exc}") from exc

    if width == 1:                       # 8bit 无符号 → 16bit
        data = array.array("h", [(b - 128) << 8 for b in frames])
    elif width == 2:
        data = array.array("h")
        data.frombytes(frames[: len(frames) // 2 * 2])
    elif width == 4:                     # 32bit → 取高 16 位
        src = array.array("i")
        src.frombytes(frames[: len(frames) // 4 * 4])
        data = array.array("h", [v >> 16 for v in src])
    else:
        raise AudioError(f"不支持的位深：{width * 8}bit，请上传 16bit 音频")

    if channels > 1:
        mono = array.array("h", [0]) * (len(data) // channels)
        for i in range(len(mono)):
            mono[i] = int(sum(data[i * channels:(i + 1) * channels]) / channels)
        data = mono
    if rate != SAMPLE_RATE:
        data = _resample(data, rate, SAMPLE_RATE)
    return data.tobytes()


def _decode_with_ffmpeg(raw: bytes) -> bytes:
    try:
        proc = subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", "pipe:0",
             "-f", "s16le", "-acodec", "pcm_s16le", "-ac", "1", "-ar", str(SAMPLE_RATE), "pipe:1"],
            input=raw, capture_output=True,
        )
    except FileNotFoundError as exc:
        raise AudioError("不是 WAV 且系统未安装 ffmpeg：请上传 wav，或先 apt install ffmpeg") from exc
    if proc.returncode != 0 or not proc.stdout:
        raise AudioError("音频无法解码，请换 wav / mp3 / m4a 再试")
    return proc.stdout


def _resample(data: array.array, src: int, dst: int) -> array.array:
    if not data or src == dst:
        return data
    n = max(1, int(round(len(data) * dst / src)))
    step = (len(data) - 1) / max(1, n - 1) if len(data) > 1 else 0
    out = array.array("h", [0]) * n
    for i in range(n):
        pos = i * step
        idx = int(pos)
        frac = pos - idx
        a = data[min(idx, len(data) - 1)]
        b = data[min(idx + 1, len(data) - 1)]
        out[i] = int(a + (b - a) * frac)
    return out


# ─── 降噪 / 规整 ────────────────────────────────────────────────

def denoise(pcm: bytes, sr: int = SAMPLE_RATE) -> bytes:
    """80Hz 高通 + 分帧噪声门。对「安静房间里的口述」效果最明显。"""
    data = array.array("h")
    data.frombytes(pcm[: len(pcm) // 2 * 2])
    if len(data) < FRAME * 2:
        return pcm

    # 1) 去直流 + 80Hz 一阶高通
    a = math.exp(-2 * math.pi * 80 / sr)
    hp = [0.0] * len(data)
    prev_in = prev_out = 0.0
    for i, x in enumerate(data):
        prev_out = a * (prev_out + x - prev_in)
        prev_in = x
        hp[i] = prev_out

    # 2) 分帧能量 → 噪声底（20 分位）
    nframes = len(hp) // FRAME
    rms = []
    for f in range(nframes):
        seg = hp[f * FRAME:(f + 1) * FRAME]
        rms.append(math.sqrt(sum(v * v for v in seg) / FRAME))
    ordered = sorted(rms)
    noise = ordered[max(0, int(len(ordered) * 0.2))] or 1e-6

    # 3) 噪声门增益（低于门限按平方衰减，最低保留 15%，避免完全静音产生"水声"）
    raw_gain = []
    for r in rms:
        ratio = r / (noise * 2.2 + 1e-9)
        raw_gain.append(1.0 if ratio >= 1 else max(0.15, ratio * ratio))
    gains = []
    for i in range(nframes):                       # 3 帧滑动平均，抹掉增益跳变
        lo, hi = max(0, i - 1), min(nframes, i + 2)
        gains.append(sum(raw_gain[lo:hi]) / (hi - lo))

    # 4) 逐帧应用（帧间线性过渡，不引入咔哒声）
    out = array.array("h", [0]) * len(data)
    for f in range(nframes):
        g = gains[f]
        base = f * FRAME
        for i in range(FRAME):
            out[base + i] = int(max(-32768, min(32767, hp[base + i] * g)))
    out[nframes * FRAME:] = data[nframes * FRAME:]     # 尾部不足一帧的原样保留
    return out.tobytes()


def trim_silence(pcm: bytes, sr: int = SAMPLE_RATE) -> bytes:
    """裁掉首尾静音，保留 120ms 呼吸感。"""
    data = array.array("h")
    data.frombytes(pcm[: len(pcm) // 2 * 2])
    nframes = len(data) // FRAME
    if nframes < 3:
        return pcm
    rms = []
    for f in range(nframes):
        seg = data[f * FRAME:(f + 1) * FRAME]
        rms.append(math.sqrt(sum(v * v for v in seg) / FRAME))
    peak = max(rms) or 1
    thresh = max(peak * 0.04, 60)
    voiced = [i for i, r in enumerate(rms) if r >= thresh]
    if not voiced:
        return pcm
    pad = int(0.12 * sr / FRAME)
    start = max(0, voiced[0] - pad) * FRAME
    end = min(nframes, voiced[-1] + 1 + pad) * FRAME
    return data[start:end].tobytes()


def normalize(pcm: bytes, peak_ratio: float = 0.9) -> bytes:
    data = array.array("h")
    data.frombytes(pcm[: len(pcm) // 2 * 2])
    peak = max((abs(v) for v in data), default=0)
    if peak == 0:
        return pcm
    k = (32767 * peak_ratio) / peak
    return array.array("h", [int(v * k) for v in data]).tobytes()


def prepare_reference(raw: bytes) -> bytes:
    """上传音频 → 可直接当 TTS 参考音的 16k 单声道 PCM。"""
    pcm = decode_to_pcm(raw)
    pcm = denoise(pcm)
    pcm = trim_silence(pcm)
    pcm = normalize(pcm)
    seconds = duration(pcm)
    if seconds < 1.0:
        raise AudioError(f"参考音太短（{seconds:.1f}s），建议录 3~10 秒")
    if seconds > 30:
        raise AudioError(f"参考音太长（{seconds:.1f}s），建议录 3~10 秒")
    return pcm


def duration(pcm: bytes, sr: int = SAMPLE_RATE) -> float:
    return len(pcm) / 2 / sr


def to_wav(pcm: bytes, sr: int = SAMPLE_RATE) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm)
    return buf.getvalue()


def write_wav(path: str, pcm: bytes, sr: int = SAMPLE_RATE) -> None:
    with open(path, "wb") as f:
        f.write(to_wav(pcm, sr))
