#ifndef CUSTOM_WAKE_WORD_H
#define CUSTOM_WAKE_WORD_H

#include <esp_attr.h>
#include <esp_mn_iface.h>
#include <esp_mn_models.h>
#include <esp_timer.h>
#include <model_path.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <deque>
#include <string>
#include <vector>
#include <functional>
#include <mutex>

#include "audio_codec.h"
#include "wake_word.h"

class CustomWakeWord : public WakeWord {
public:
    CustomWakeWord();
    ~CustomWakeWord();

    bool Initialize(AudioCodec* codec, srmodel_list_t* models_list) override;
    bool SetWakeWords(const std::vector<std::pair<std::string, std::string>>& wake_words);
    void Feed(const std::vector<int16_t>& data) override;
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) override;
    void Start() override;
    void Stop() override;
    size_t GetFeedSize() override;
    void EncodeWakeWordData() override {}
    bool GetWakeWordOpus(std::vector<uint8_t>& opus) override { return false; }
    const std::string& GetLastDetectedWakeWord() const override { return last_detected_wake_word_; }
    std::string GetLastDetectedAction() const override { return last_detected_action_; }

private:
    struct Command {
        std::string command;
        std::string text;
        std::string action;
    };

    static const int DETECTION_RUNNING_EVENT = 1;

    esp_mn_iface_t* multinet_ = nullptr;
    model_iface_data_t* multinet_model_data_ = nullptr;
    srmodel_list_t *models_ = nullptr;
    char* mn_name_ = nullptr;
    std::string language_ = "cn";
    // ★ 15000ms（2026-09-14 唤醒失灵根因之二）：原 3000ms 窗口每 3s clean 重开一次，
    //   ~1.2s 的唤醒词有约 40% 概率跨窗口边界——前半句被 clean 丢弃，MultiNet 永远
    //   只听到半句话，必然失灵。加大到 15s 窗口后截断概率降到 ~8%，超时行为不变。
    int duration_ = 15000;
    float threshold_ = 0.2;
    std::deque<Command> commands_;

    std::function<void(const std::string& wake_word)> wake_word_detected_callback_;
    AudioCodec* codec_ = nullptr;
    std::string last_detected_wake_word_;
    std::string last_detected_action_;   // "wake" / "meeting"
    std::vector<int16_t> input_buffer_;
    std::mutex input_buffer_mutex_;
    EventGroupHandle_t event_group_ = nullptr;
    int chunksize_ = 0;

    // ★ 唤醒链路诊断统计（每500次feed=5s打印一次）
    // ★ 2026-09-18：计数器由 uint64_t 降为 uint32_t —— 仅诊断用（100/s 也要 497 天才回绕），
    //   而 sdkconfig 开着 CONFIG_NEWLIB_NANO_FORMAT（不支持 %llu）导致原日志参数错位。
    uint32_t feed_count_ = 0;        // feed 次数（应≈100/s）
    uint32_t feed_samples_ = 0;      // 喂入 mic 样本总数
    uint32_t drop_count_ = 0;        // 背压丢弃样本数（持续增长=消费不过来）
    uint32_t detect_count_ = 0;      // detect 调用次数
    uint32_t detect_us_max_ = 0;     // detect 单帧最大耗时（>32000us=实时率超1）
    float rms_max_ = 0;              // 5s 内 chunk RMS 峰值（远场语音应>300）

    void AudioDetectionTask();
};

#endif