# ruyi-esp32 · 如意 ESP32 语音助手

基于 ESP32-S3 的开源 AI 语音对话设备：**离线唤醒 + 端侧 VAD + 全链路流式 S2S**
（流式语音识别 → 大模型对话 → 流式语音合成，边说边识别、边生成边播放，全程内存调度不落盘）。

本仓库包含两部分：

```
├── main/ components/       ESP32-S3 固件（ESP-IDF 5.x）
└── server/                 配套开源服务端（Python，见 server/README.md）
```

## 服务端两种用法

| 模式 | 说明 |
|------|------|
| **如意云 API（默认）** | 服务端只做透明转发，ASR/LLM/TTS 编排全部在如意云完成，填一把 api_key 即用 |
| **自编排** | 在自己的服务器上对接厂商 API：ASR（字节豆包 / 阿里 Paraformer）、LLM（字节豆包 / 阿里通义 / DeepSeek / 智谱 GLM）、TTS（字节豆包 / 阿里 CosyVoice） |

管理后台 `/admin` 可视化切换模式与配置密钥，改完自动保存。
服务端还自带一个浏览器对话页 `/demo`：点话筒即可连续对话，开摄像头后每句话都会带上一帧画面给 AI，不接硬件也能试通整条链路。

## API Key 申请

固件与服务端默认对接如意云（`wss://dev.nice-ai.cn/s2s/`），免费申请 S2S Key：
**https://dev.nice-ai.cn/podcast/api-docs**（「申请 S2S Key」接口）。

## 固件编译

```bash
# ESP-IDF 5.x（推荐 5.3+）
idf.py set-target esp32s3
cp sdkconfig.defaults sdkconfig     # 首次：复制默认配置（API Key 为空）
idf.py menuconfig                   # NiceAI Configuration：填 API Key / 服务器地址
idf.py build flash monitor
```

## 服务端部署

```bash
cd server
pip install -r requirements.txt     # 需要 libopus：apt install libopus0
cp config.example.yaml config.yaml
python run.py                       # 默认 0.0.0.0:8892
```

固件 `OTA/S2S URL` 配置为 `wss://你的服务器:8892/s2s/ws` 即可接入。
详细协议与二次开发见 [server/README.md](server/README.md)。

## 硬件

- 主控：ESP32-S3（N16R8）
- 音频：麦克风 + 功放（板型配置见 `sdkconfig.sisi` / `sdkconfig.meeting`）
- 端侧能力：唤醒词检测、VAD、AEC、Opus 编解码

## License

见 [LICENSE](LICENSE)。
