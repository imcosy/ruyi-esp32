#include "websocket.h"
#include "board.h"
#include "system_info.h"
#include "app.h"
#include "settings.h"

#include <cstring>
#include <algorithm>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_app_format.h>
#include <esp_app_desc.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include "audio/assets/lang_config.h"

#define TAG "WS"

WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();
}

WebsocketProtocol::~WebsocketProtocol() {
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
    // S2S 协议：连接在 OpenAudioChannel 时建立
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    // ★ 协议锁：发送与重建/关闭互斥（崩溃根修，见 protocol_mutex_ 注释）
    std::lock_guard<std::mutex> lock(protocol_mutex_);
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }
    // S2S: 发送二进制帧（OGG/Opus 或 PCM 都通过此通道）
    return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
}

bool WebsocketProtocol::SendText(const std::string& text) {
    std::lock_guard<std::mutex> lock(protocol_mutex_);
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

void WebsocketProtocol::SendPingpong(int64_t timestamp) {
    std::lock_guard<std::mutex> lock(protocol_mutex_);
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return;
    }

    std::string json = "{\"type\":\"pingpong\",\"time\":" + std::to_string(timestamp) + "}";
    websocket_->Send(json);
}

void WebsocketProtocol::SendToolResponse(const std::string& payload) {
    std::lock_guard<std::mutex> lock(protocol_mutex_);
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return;
    }

    std::string json = "{\"type\":\"tool\",\"payload\":" + payload + "}";
    websocket_->Send(json);
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr &&
           websocket_->IsConnected() &&
           !error_occurred_;
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    // ★ 协议锁：关闭与发送互斥（崩溃根修）——reset() 期间其他任务的 Send 排队，
    //   发送中的帧完整落盘后才销毁传输层，杜绝发送中销毁的 UAF
    std::lock_guard<std::mutex> lock(protocol_mutex_);
    if (websocket_ != nullptr) {
        if (send_goodbye && websocket_->IsConnected()) {
            websocket_->Close();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        // ★ 主动关闭：reset() 析构内 Disconnect 等接收任务退出后再释放传输层；
        //   期间触发的断连回调被 closing_ 抑制（析构链不可重入应用回调）
        closing_ = true;
        websocket_.reset();
        closing_ = false;
    }
}

bool WebsocketProtocol::OpenAudioChannel() {
    // ★ 重建串行锁（2026-09-17 崩溃根修）：ActivationTask 重连与主循环按钮重连
    //   可能同时进入本函数，无互斥会双重建（对象泄漏+双接收任务）。持锁全程，
    //   另一重建方排队后经下方二次检查直接返回。
    std::lock_guard<std::mutex> rebuild_lock(rebuild_mutex_);

    if (websocket_ != nullptr && websocket_->IsConnected()) {
        return true;
    }

    error_occurred_ = false;

    // ★ 三段式重建（2026-09-17 崩溃根修）：清旧（锁内）→ 建新连接（锁外）→ 启用（锁内）。
    //   原实现整段与主循环上行音频并发：ActivationTask 重连 reset() 旧对象时，
    //   主循环 SendS2SAudio 正在发送 → 传输层发送中被销毁 → tcpip 线程 do_writemore
    //   访问已释放的 netconn → LoadStoreAlignment panic（tts_done 后实测崩溃）。
    //   锁内清旧：reset 期间其他任务 Send 排队（断连后快速失败），发送中的帧
    //   先完整落盘；Connect 握手（最长10s）放锁外，避免主循环 Send 长时间排队。
    {
        std::lock_guard<std::mutex> lock(protocol_mutex_);
        if (websocket_) {
            ESP_LOGI(TAG, "OpenAudioChannel: 清理旧连接后重建");
            closing_ = true;
            websocket_.reset();
            closing_ = false;
        }
    }

    auto network = Board::GetInstance().GetNetwork();
    auto ws = network->CreateWebSocket(1);
    if (ws == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    ws->SetHeader("User-Agent", SystemInfo::GetUserAgent().c_str());
    ws->SetHeader("Accept-Language", Lang::CODE);
    ws->SetHeader("Content-Type", "application/json");

    // Handle incoming data
    ws->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            // ── S2S 二进制帧：按 tts_start 通告的 format 分流 ──
            // format=opus_raw：opus 裸流 [2字节小端长度][opus帧]...（与上行 opus_raw 对称），
            //                  每解出一帧（20ms@48kHz）→ packet(codec=1) → 解码队列（Opus 解码路径）
            // format=int16   ：int16 PCM 直通（codec=0），重采样到输出采样率
            // 服务端协议：tts_start(format) → tts_sentence → Binary → tts_sentence_end → tts_done
            if (on_incoming_audio_ != nullptr && len > 0) {
                if (downlink_format_ == "opus_raw") {
                    downlink_opus_buf_.insert(downlink_opus_buf_.end(),
                                              reinterpret_cast<const uint8_t*>(data),
                                              reinterpret_cast<const uint8_t*>(data) + len);
                    size_t pos = 0;
                    while (downlink_opus_buf_.size() - pos >= 2) {
                        uint16_t flen = downlink_opus_buf_[pos] |
                                        (uint16_t)(downlink_opus_buf_[pos + 1] << 8);
                        if (downlink_opus_buf_.size() - pos - 2 < flen) {
                            break;  // 帧未收齐，等下一个分片
                        }
                        if (flen > 0) {
                            const uint8_t* frame_ptr = downlink_opus_buf_.data() + pos + 2;
                            AudioStreamPacket packet{
                                .sample_rate = downlink_sample_rate_,
                                .frame_duration = 20,  // libopus 20ms 帧
                                .codec = 1,            // Opus
                                .timestamp = 0,
                                .payload = PsramBytes(frame_ptr, frame_ptr + flen)
                            };
                            on_incoming_audio_(std::make_unique<AudioStreamPacket>(std::move(packet)));
                        }
                        pos += 2 + flen;
                    }
                    if (pos > 0) {
                        downlink_opus_buf_.erase(downlink_opus_buf_.begin(),
                                                 downlink_opus_buf_.begin() + pos);
                    }
                } else {
                    AudioStreamPacket packet{
                        .sample_rate = downlink_sample_rate_,
                        .frame_duration = 0,
                        .codec = 0,            // PCM 直通
                        .timestamp = 0,
                        .payload = PsramBytes(data, data + len)
                    };
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(std::move(packet)));
                }
            }
        } else {
            // ── S2S JSON 文本消息 ──
            ESP_LOGI(TAG, ">>> 服务器JSON: %s", std::string(data, len).c_str());
            auto root = cJSON_ParseWithLength(data, len);
            if (root == nullptr) {
                ESP_LOGE(TAG, "解析JSON失败: %s", std::string(data, len).c_str());
                return;
            }

            // 提取 tts_start 的 sample_rate / format 用于下行音频分流
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type) && strcmp(type->valuestring, "tts_start") == 0) {
                auto sr = cJSON_GetObjectItem(root, "sample_rate");
                if (cJSON_IsNumber(sr) && sr->valueint > 0) {
                    downlink_sample_rate_ = sr->valueint;
                }
                auto fmt = cJSON_GetObjectItem(root, "format");
                if (cJSON_IsString(fmt) && fmt->valuestring != nullptr) {
                    downlink_format_ = fmt->valuestring;
                    // 新一轮 TTS 开始：清掉上轮打断后残留的半帧，避免旧数据先播
                    downlink_opus_buf_.clear();
                    ESP_LOGI(TAG, "tts_start: sample_rate=%d format=%s",
                             downlink_sample_rate_, downlink_format_.c_str());
                }
            }

            // S2S 协议所有 JSON 消息都转发给 application 处理
            // 包括：asr_result / llm_start / llm_chunk / tts_start / tts_sentence /
            //       tts_sentence_end / tts_done / done / aborted / closed / error /
            //       echo_filtered / image_recognition / music / singing / warning
            if (on_incoming_json_ != nullptr) {
                on_incoming_json_(root);
            }

            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    ws->OnDisconnected([this]() {
        ESP_LOGI(TAG, "WebSocket已断开");
        error_occurred_ = true;
        session_id_.clear();
        // ★ 主动重建连接时 reset() 会触发 Disconnect()→ssl_receive 任务退出→
        //   tcp_->OnDisconnected 回调——此时对象正在销毁流程中，若把此事件再
        //   当作"被动断连"上报 → on_audio_channel_closed_ → app 切Idle/播提示音，
        //   不仅语义错位且重入析构链。closing_ 标志在 reset() 前后维护窗口。
        if (!closing_ && on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    // ── 构造 S2S 连接 URL：wss://dev.nice-ai.cn/s2s/ws/?api_key=xxx&firmware_version=xxx ──
    // 鉴权走 api_key（s2s_api_keys 表）；设备登录态由首帧 user_token（服务端签发、NVS 存储）承载，
    // 不使用 device_id / mdn 接口
    std::string base_url = CONFIG_OTA_URL;
    auto app_desc = esp_app_get_description();
    std::string fw_version = app_desc->version;
    std::string api_key = CONFIG_S2S_API_KEY;

    std::string url = base_url;
    char sep = (url.find('?') == std::string::npos) ? '?' : '&';
    url += sep;
    url += "api_key=";
    url += api_key;
    url += "&firmware_version=";
    url += fw_version;

    ESP_LOGI(TAG, "正在连接 s2s_server: %s (api_key=%s)", url.c_str(), api_key.c_str());
    if (!ws->Connect(url.c_str())) {
        ESP_LOGE(TAG, "连接 s2s_server 失败, 错误码=%d", ws->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // S2S 协议：服务端连接后不主动推送任何消息（等待客户端首帧 JSON）
    // 因此这里不再等待 connected 消息，连接成功即视为通道开启
    // 首帧 JSON 由 app.cc 在唤醒时通过 SendS2SFirstFrame 发送

    // ★ 锁内启用新连接（2026-09-17 崩溃根修）：此后所有 Send 才能看到本连接
    {
        std::lock_guard<std::mutex> lock(protocol_mutex_);
        websocket_ = std::move(ws);
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}
