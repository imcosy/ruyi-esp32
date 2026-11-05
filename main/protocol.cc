#include "protocol.h"

#include "settings.h"

#include <esp_log.h>
#include <esp_app_desc.h>
#include <cJSON.h>

#include "settings.h"

#define TAG "Protocol"

void Protocol::OnIncomingJson(std::function<void(const cJSON* root)> callback) {
    on_incoming_json_ = callback;
}

void Protocol::OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback) {
    on_incoming_audio_ = callback;
}

void Protocol::OnAudioChannelOpened(std::function<void()> callback) {
    on_audio_channel_opened_ = callback;
}

void Protocol::OnAudioChannelClosed(std::function<void()> callback) {
    on_audio_channel_closed_ = callback;
}

void Protocol::OnNetworkError(std::function<void(const std::string& message)> callback) {
    on_network_error_ = callback;
}

void Protocol::OnConnected(std::function<void()> callback) {
    on_connected_ = callback;
}

void Protocol::OnDisconnected(std::function<void()> callback) {
    on_disconnected_ = callback;
}

void Protocol::SetError(const std::string& message) {
    error_occurred_ = true;
    if (on_network_error_ != nullptr) {
        on_network_error_(message);
    }
}

// ── S2S 协议适配 ──────────────────────────────────────────────────
// S2S 协议核心：一连接多轮对话，每轮通过 Binary 或 JSON 发送音频，
// 服务端响应 asr_result → llm_chunk → tts_start → tts_sentence → Binary 音频 → tts_done → done。
// 没有 start_speech/end_speech 概念，音频本身就是一轮对话的开始与结束。

void Protocol::SendAbortSpeaking(AbortReason reason) {
    // S2S: 客户端随时可发 {"type":"abort"} 中断当前 LLM/TTS 生成
    SendText("{\"type\":\"abort\"}");
}

// 唤醒词不再走单独的 wakeup 消息（S2S 无此概念）。
// 唤醒后通过 SendS2SFirstFrame(noaudio=true + text="你好") 触发服务端 LLM 回应。
void Protocol::SendWakeWordDetected(const std::string& wake_word) {
    ESP_LOGI(TAG, "SendWakeWordDetected via S2S first frame: %s", wake_word.c_str());
    SendS2SFirstFrame("你好");
}

// S2S 没有独立的 start_speech 通知（音频本身就是开始信号）
// 客户端进入 Listening 状态后边说边传 Opus 裸流（带长度前缀），VAD 静音后发 listen_stop 触发 ASR
void Protocol::SendStartListening() {
    ESP_LOGI(TAG, "SendStartListening: S2S 协议无 start_speech（音频即开始信号）");
}

// S2S 流式上行：VAD 静音后发送 listen_stop JSON，通知服务端本轮音频结束触发 ASR
// 客户端边说边传 Opus 块（多个 binary 帧，每帧前 2 字节小端长度前缀），说完发 listen_stop 让服务端合并送 ASR
void Protocol::SendStopListening() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "listen_stop");
    char* json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    SendText(message);
    ESP_LOGI(TAG, ">>> S2S 发送 listen_stop");
}

// 发送 S2S 首帧 JSON：认证 + 默认参数 + 可选首轮文本
// wake_text 不为空时使用 noaudio 模式，跳过 ASR 直接走 LLM（用于唤醒词触发）
// wake_text 为空时仅发送配置参数，服务端等待后续二进制 PCM 音频帧（用于按键触发）
bool Protocol::SendS2SFirstFrame(const std::string& wake_text) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "api_key", CONFIG_S2S_API_KEY);
    cJSON_AddStringToObject(root, "format", "opus_raw");
    cJSON_AddNumberToObject(root, "sample_rate", 16000);
    cJSON_AddStringToObject(root, "language", "zh");
    cJSON_AddStringToObject(root, "voice", "ruyi");
    // ★ 角色持久化（2026-09-14）：NVS 存储上次切换的角色 ID，首帧带给服务端
    //   服务端用该角色构建身份+音色，实现"切换角色后重启设备角色不变"
    {
        Settings settings("sisi", false);
        int saved_aid = settings.GetInt("ai_character_id", 0);
        if (saved_aid > 0) {
            cJSON_AddNumberToObject(root, "ai_character_id", saved_aid);
        }
    }
    // ★ 下行 TTS 用 opus/OGG（64kbps，约 8KB/s），替代 48kHz 原始 PCM（96KB/s）：
    //   带宽降 12 倍，避免下行洪泛拖垮 ESP32 TLS/TCP 链路（上行同连接被阻塞→
    //   ASR partial 迟滞、listen_stop 发不出、发送队列丢帧没话头）
    cJSON_AddStringToObject(root, "output_format", "opus");
    cJSON_AddBoolToObject(root, "stream_text", 1);
    cJSON_AddBoolToObject(root, "stream_upload", 1);
    // 用户资料记忆：LLM 从对话中提取用户资料（姓名/称呼/性别/生日等）写入 xd_users 档案，
    // 连接建立时服务端自动把已有档案注入 system_prompt，AI 知道当前用户是谁
    cJSON_AddBoolToObject(root, "enable_user_profile", 1);
    // 设备身份：client_type 标识 ESP32 接入；user_token 为服务端签发的登录令牌
    // （首次语音登录成功后下发、NVS 持久化），令牌有效即免登录，不使用 device_id
    cJSON_AddStringToObject(root, "client_type", "esp32");
    // ★ 板型标识（2026-09-16 设备管理）：编译期 BOARD_TYPE（sisi/meeting/stick-s3/
    //   anan/chatbox…），服务端据此落设备注册表、按板应用唤醒/会议模式配置
    cJSON_AddStringToObject(root, "board", BOARD_TYPE);
    {
        Settings settings("sisi", false);
        std::string user_token = settings.GetString("user_token");
        if (!user_token.empty()) {
            cJSON_AddStringToObject(root, "user_token", user_token.c_str());
        }
    }
    if (!wake_text.empty()) {
        // noaudio 模式：跳过 ASR，直接用 text 触发 LLM
        cJSON_AddBoolToObject(root, "noaudio", 1);
        cJSON_AddStringToObject(root, "text", wake_text.c_str());
    }

    char* json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);

    ESP_LOGI(TAG, ">>> S2S 首帧发送: %s", message.c_str());
    return SendText(message);
}

// 发送 S2S JSON 控制消息（如 {"type":"tts_playback_done"}）
bool Protocol::SendS2SJson(const std::string& json_message) {
    if (json_message.empty()) {
        return false;
    }
    ESP_LOGI(TAG, ">>> S2S JSON 发送: %s", json_message.c_str());
    return SendText(json_message);
}

// 发送 S2S 一轮音频（二进制 Opus 裸流：每帧前 2 字节小端长度前缀）
// audio_data: 累积的 Opus 帧字节流（16kHz mono 20ms/帧，由 AudioService Opus 编码累积）
bool Protocol::SendS2SAudio(const std::vector<uint8_t>& audio_data) {
    if (audio_data.empty()) {
        ESP_LOGW(TAG, "SendS2SAudio: 音频数据为空，跳过");
        return false;
    }
    // ★ 2026-09-18 根修：先发再打日志——原实现无条件先打 ">>> S2S 发送音频"，
    //   断连/握手失败时（SendAudio 内部 !IsConnected 直接 return false）日志照样刷，
    //   排障时"日志在发、服务端 0 帧"互相矛盾。失败限频告警（每 10 次一条）。
    AudioStreamPacket packet;
    packet.payload.assign(audio_data.begin(), audio_data.end());
    bool ok = SendAudio(std::make_unique<AudioStreamPacket>(std::move(packet)));
    if (ok) {
        ESP_LOGI(TAG, ">>> S2S 发送音频: %u bytes opus_raw", (unsigned)audio_data.size());
    } else {
        static uint32_t s_audio_send_fail = 0;
        if (++s_audio_send_fail % 10 == 1) {
            ESP_LOGW(TAG, "S2S 发送音频失败累计: %lu（连接未就绪）",
                     (unsigned long)s_audio_send_fail);
        }
    }
    return ok;
}

bool Protocol::IsTimeout() const {
    const int kTimeoutSeconds = 120;
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming_time_);
    bool timeout = duration.count() > kTimeoutSeconds;
    if (timeout) {
        ESP_LOGE(TAG, "Channel timeout %ld seconds", (long)duration.count());
    }
    return timeout;
}
