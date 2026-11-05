#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_

#include "protocol.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>
#include <functional>
#include <mutex>

// OTA信息结构体（保留接口兼容，S2S 协议不推送 OTA 信息，故始终为空）
struct OtaInfo {
    std::string firmware_url;
    std::string firmware_version;
};

// 资源信息结构体（同上，S2S 不推送 assets_info，始终为空）
struct AssetsInfo {
    std::string version;
    std::string url;
    std::string boot_audio;
};

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

    // 检查底层WebSocket连接状态（不依赖session_id，用于判断是否需要重连）
    bool IsWebSocketConnected() const {
        return websocket_ != nullptr && websocket_->IsConnected();
    }

    // 获取OTA信息（S2S 协议不推送，始终返回空）
    const OtaInfo& GetOtaInfo() const { return ota_info_; }

    // 获取资源信息（S2S 协议不推送，始终返回空）
    const AssetsInfo& GetAssetsInfo() const { return assets_info_; }

private:
    EventGroupHandle_t event_group_handle_;
    std::unique_ptr<WebSocket> websocket_;

    // OTA和资源信息（S2S 不推送，始终为空，保留字段避免 app.cc 编译错误）
    OtaInfo ota_info_;
    AssetsInfo assets_info_;

    // 下行音频采样率（从 tts_start JSON 提取，默认 48000；PCM 直通重采样/opus 解码均用）
    int downlink_sample_rate_ = 48000;

    // 下行编码格式（从 tts_start JSON 的 format 提取）："int16"=PCM 帧直入解码队列，
    // "opus_raw"=opus 裸流（[2字节小端长度][opus帧]...）解析出 20ms 帧后入解码队列
    std::string downlink_format_ = "int16";

    // 下行 opus_raw 裸流跨 TCP 分片的累积缓冲（帧边界解析用）
    std::vector<uint8_t> downlink_opus_buf_;

    // ★ 主动关闭/重建连接期间置位（2026-09-17）：抑制旧对象析构时触发的
    //   on_audio_channel_closed_（防重建连接误播"断连提示音"/误切Idle）
    bool closing_ = false;

    // ★ 协议锁（2026-09-17 崩溃根修，主链路层）：所有 WS 对象的发送/重建/关闭
    //   收敛到一把锁下。原多任务并发：ActivationTask 重建连接 reset() 旧对象时，
    //   主循环可能正在 SendS2SAudio 上行音频（TTS 播报期持续）→ 传输层在发送中
    //   被销毁 → tcpip 线程 do_writemore 访问被释放的 netconn → LoadStoreAlignment
    //   panic（实测 tts_done 后 40ms 崩，EXCVADDR=0x4037c871）。组件保持原版，
    //   由主链路协议层保证发送与销毁互斥。
    std::mutex protocol_mutex_;

    // ★ 重建串行锁（2026-09-17）：ActivationTask 重连与主循环按钮重连可能同时
    //   进入 OpenAudioChannel——无此锁会产生双连接对象（旧对象泄漏+双接收任务）。
    //   只互斥重建流程本身，不阻塞发送；持锁期间 Connect 最长10s，另一重建方
    //   排队后经二次检查直接返回（对方已连上）。
    std::mutex rebuild_mutex_;

    bool SendText(const std::string& text) override;
    void SendPingpong(int64_t timestamp) override;
    void SendToolResponse(const std::string& payload) override;
public:
    void ClearSessionForReauth() { session_id_.clear(); }
};

#endif
