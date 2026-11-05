#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cJSON.h>
#include <string>
#include <functional>
#include <chrono>
#include <vector>
#include <deque>
#include <cstddef>
#include <new>
#include <esp_heap_caps.h>
#include <esp_log.h>

// ★ PSRAM 分配器：TTS 下行 / 上行音频数据缓冲放到 PSRAM（SRAM治理，MDN线验证版本移植）。
// 根因：设备内部 SRAM 空闲仅 ~14KB，音频包缓冲若落在内部 RAM，解码/播放队列最多只能
// 缓冲 ~280ms 音频，服务端突发/网络抖动超过该深度即播放队列下溢 → TTS 卡顿。
// 改为 PSRAM 后可深度缓冲(数秒)，平滑网络抖动。
template <typename T>
struct PsramAllocator {
    using value_type = T;

    PsramAllocator() noexcept = default;
    template <typename U>
    PsramAllocator(const PsramAllocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        if (n == 0) return nullptr;
        if (n > std::size_t(-1) / sizeof(T)) {
            ESP_LOGE("PsramAllocator", "分配超限: %u * %uB", (unsigned)n, (unsigned)sizeof(T));
            abort();
        }
        void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM);
        if (p == nullptr) {
            // ★ 不抛 std::bad_alloc：SRAM 近乎耗尽时抛异常本身会二次崩溃，直接 abort 精确定位
            ESP_LOGE("PsramAllocator", "PSRAM分配失败: %u * %uB（%uB）",
                     (unsigned)n, (unsigned)sizeof(T), (unsigned)(n * sizeof(T)));
            abort();
        }
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept {
        heap_caps_free(p);
    }
};

template <typename T, typename U>
bool operator==(const PsramAllocator<T>&, const PsramAllocator<U>&) noexcept { return true; }
template <typename T, typename U>
bool operator!=(const PsramAllocator<T>&, const PsramAllocator<U>&) noexcept { return false; }

using PsramBytes = std::vector<uint8_t, PsramAllocator<uint8_t>>;

// ★ PSRAM deque：音频队列的元素对象+deque节点整体放PSRAM。
// 根因（20260915 0030 崩溃修复）：此前队列为 deque<unique_ptr<T>>，元素对象全部
// new 在内部 SRAM——TTS 播报超长会议总结时 decode/playback 队列持续堆积数百个
// 包对象（~30KB+ 内部堆），SRAM 最低被打穿到 187B，后续任何一次内部堆分配
// （NVS Settings/网络缓冲）拿到 null 即 LoadProhibited 崩溃（EXCVADDR 0x43）。
// 值语义+PSRAM节点后，队列内存全部落在 PSRAM，SRAM 不再随音频缓冲深度波动。
template <typename T>
using PsramDeque = std::deque<T, PsramAllocator<T>>;

struct AudioStreamPacket {
    int sample_rate = 0;
    int frame_duration = 0;
    int codec = 0;          // 0=PCM, 1=Opus
    uint32_t timestamp = 0;
    PsramBytes payload;     // ★ PSRAM 缓冲，避免耗尽内部 SRAM
};

struct BinaryProtocol2 {
    uint16_t version;
    uint16_t type;          // Message type (0: OPUS, 1: JSON)
    uint32_t reserved;      // Reserved for future use
    uint32_t timestamp;     // Timestamp in milliseconds (used for server-side AEC)
    uint32_t payload_size;  // Payload size in bytes
    uint8_t payload[];      // Payload data
} __attribute__((packed));

struct BinaryProtocol3 {
    uint8_t type;
    uint8_t reserved;
    uint16_t payload_size;
    uint8_t payload[];
} __attribute__((packed));

struct AudioFrameHeaderV1 {
    char magic[4];               // "SAUD"
    uint8_t version;             // 1
    uint8_t codec;               // 0=PCM, 1=Opus
    uint8_t channels;            // 1=mono
    uint8_t reserved;            // reserved
    uint32_t sample_rate;        // network byte order
    uint16_t frame_duration_ms;  // network byte order
    uint16_t flags;              // network byte order
    uint32_t seq;                // network byte order
    uint32_t payload_len;        // network byte order
} __attribute__((packed));

enum AbortReason {
    kAbortReasonNone,
    kAbortReasonWakeUp,
    kAbortReasonNoSpeak
};

class Protocol {
public:
    virtual ~Protocol() = default;

    inline int server_sample_rate() const {
        return server_sample_rate_;
    }
    inline int server_frame_duration() const {
        return server_frame_duration_;
    }
    inline const std::string& session_id() const {
        return session_id_;
    }

    void OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback);
    void OnIncomingJson(std::function<void(const cJSON* root)> callback);
    void OnAudioChannelOpened(std::function<void()> callback);
    void OnAudioChannelClosed(std::function<void()> callback);
    void OnNetworkError(std::function<void(const std::string& message)> callback);
    void OnConnected(std::function<void()> callback);
    void OnDisconnected(std::function<void()> callback);

    virtual bool Start() = 0;
    virtual bool OpenAudioChannel() = 0;
    virtual void CloseAudioChannel(bool send_goodbye = true) = 0;
    virtual bool IsAudioChannelOpened() const = 0;
    virtual bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) = 0;
    virtual void SendWakeWordDetected(const std::string& wake_word);
    virtual void SendStartListening();
    virtual void SendStopListening();
    virtual void SendAbortSpeaking(AbortReason reason);
    virtual void SendPingpong(int64_t timestamp) = 0;
    virtual void SendToolResponse(const std::string& payload) = 0;

    // ── S2S 协议扩展方法 ─────────────────────────────────────
    /// 发送 S2S 首帧 JSON（认证 + 默认参数 + 可选首轮文本）
    /// wake_text: 唤醒文本（如 "你好"），不为空时使用 noaudio 模式直接走 LLM
    /// 返回 true 表示发送成功
    virtual bool SendS2SFirstFrame(const std::string& wake_text = "");

    /// 发送 S2S 一轮音频（二进制 PCM）
    /// audio_data: PCM 字节流（int16 16kHz mono）
    /// 返回 true 表示发送成功
    virtual bool SendS2SAudio(const std::vector<uint8_t>& audio_data);

    /// 发送 S2S JSON 控制消息（如 {"type":"tts_playback_done"}）
    /// 返回 true 表示发送成功
    virtual bool SendS2SJson(const std::string& json_message);

protected:
    std::function<void(const cJSON* root)> on_incoming_json_;
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> on_incoming_audio_;
    std::function<void()> on_audio_channel_opened_;
    std::function<void()> on_audio_channel_closed_;
    std::function<void(const std::string& message)> on_network_error_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;

    int server_sample_rate_ = 24000;
    int server_frame_duration_ = 60;
    bool error_occurred_ = false;
    std::string session_id_;
    std::chrono::time_point<std::chrono::steady_clock> last_incoming_time_;

    virtual bool SendText(const std::string& text) = 0;
    virtual void SetError(const std::string& message);
    virtual bool IsTimeout() const;
};

#endif // PROTOCOL_H

