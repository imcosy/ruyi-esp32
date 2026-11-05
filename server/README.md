# sisi-esp-server

基于 [sisi_esp](../)（xiaozhi-esp32 深度改造）固件的 **S2S（Speech-to-Speech）开源服务端**。

一条 WebSocket 承载「流式语音识别 → 大模型对话 → 流式语音合成」全链路：
固件端 VAD 说话 → opus 上行 → ASR → LLM 流式分句 → TTS 边合成边下发 opus → 固件边收边播。
全程内存调度、不落盘。

## 两种工作模式

| 模式 | 说明 | 配置 |
|------|------|------|
| **如意云 API（默认）** | 本服务只做固件协议 ↔ 云端的透明转发，ASR/LLM/TTS 编排全部在如意云完成，一行代码不用改 | 只填一把 `nice.api_key` |
| **自编排（对接厂商）** | 本服务在本机串接 ASR / LLM / TTS 三段流水线，密钥全在自己手里 | ASR：字节豆包 / 阿里 Paraformer；LLM：字节豆包 / 阿里通义 / DeepSeek / 智谱 GLM；TTS：字节豆包 / 阿里 CosyVoice |

切换与密钥配置全部在 **管理后台 `/admin`** 完成，改完自动保存、即时生效（下一条连接用新配置）。

## 快速开始

```bash
# 1. 系统依赖：libopus（opus 编解码）
sudo apt install -y libopus0

# 2. 安装（Python 3.10+）
cd server
pip install -r requirements.txt

# 3. 配置
cp config.example.yaml config.yaml
# 编辑 config.yaml：填入 nice.api_key（或切换自编排并填厂商密钥）

# 4. 启动
python run.py
# 默认 0.0.0.0:8892，浏览器打开 http://<host>:8892/admin 管理配置
```

## 固件接入

固件侧 `CONFIG_OTA_URL` 指向本服务即可（同一地址同时是 OTA 与 S2S 入口）：

```
idf.py menuconfig  →  NiceAI Configuration  →  OTA/S2S URL
    wss://你的服务器:端口/s2s/ws
```

**API Key**：`menuconfig → NiceAI Configuration → S2S API Key` 填入你的密钥。
默认走如意云时需要一把如意云 S2S Key，免费申请：**https://dev.nice-ai.cn/podcast/api-docs**（「申请 S2S Key」接口）。
仓库不携带任何密钥：`sdkconfig.defaults` 里 `CONFIG_S2S_API_KEY` 为空，首次编译前请复制为 `sdkconfig` 或直接在 menuconfig 填写。

固件连接后会自动带上 `?api_key=<编译值>&firmware_version=<版本>`。
服务端 `server.api_keys` 白名单非空时校验首帧 `api_key`，为空则不校验（内网自用）。

## 协议速查

固件与本服务、以及如意云 S2S API 使用同一套协议：

```
固件 → 服务端
  首帧 JSON   {api_key, format:"opus_raw", sample_rate:16000, language, voice,
               ai_character_id, output_format:"opus", stream_text, stream_upload,
               client_type:"esp32", board, user_token, [noaudio, text]}
  hello       {type:"hello", board:"sisi", has_aec:true}
  Binary      opus 裸流：[2字节小端长度][opus帧(20ms@16k)]×N（边说边传）
  listen_stop {type:"listen_stop"}        ← VAD 静音，触发 ASR→LLM→TTS
  abort       {type:"abort"}              ← 打断当前生成
  tts_playback_done {type:"tts_playback_done"}

服务端 → 固件
  asr_result      {type, round, text}
  llm_start       {type, round}
  llm_chunk       {type, round, text}          （stream_text=true 时）
  tts_start       {type, round, sample_rate:48000, format:"opus_raw", channels:1}
  tts_sentence    {type, round, index, text}   ← 每句合成前推文本
  Binary          opus 裸流（48k/64kbps，帧结构同上行）
  tts_sentence_end{type, round, index}
  tts_done        {type, round}
  done            {type, round, asr_text, llm_text, total_ms}
  aborted         {type, round, asr_text, llm_text}
  error           {type, [round], message}
```

## 目录结构

```
server/
├── run.py                  启动脚本
├── config.yaml             运行配置（不入 git，/admin 可视化编辑）
├── requirements.txt
└── s2s/
    ├── app.py              FastAPI：固件 WS 端点 + 管理后台 API
    ├── config.py           配置加载/原子保存/就绪校验
    ├── session.py          固件会话：S2S 协议状态机 + ASR→LLM→TTS 流水线
    ├── audio.py            opus 编解码（上下行裸流）+ 重采样
    ├── static/admin.html   管理后台配置页
    └── providers/
        ├── base.py         ASR/LLM/TTS 统一接口（新增厂商实现这三个类即可）
        ├── nice.py         默认模式：如意云 S2S 透明代理
        ├── llm_openai.py   OpenAI 兼容流式（豆方舟/通义/DeepSeek/智谱 四家通用）
        ├── asr_doubao.py   字节火山流式语音识别
        ├── asr_aliyun.py   阿里百炼 Paraformer 实时识别
        ├── tts_doubao.py   字节豆包流式语音合成
        └── tts_aliyun.py   阿里百炼 CosyVoice 流式合成
```

## 新增一家厂商

实现 `providers/base.py` 三个基类之一，在 `providers/__init__.py` 注册，`config.py`
补默认配置，`admin.html` 加一个 chip —— 完事。会话编排层不感知厂商差异。
