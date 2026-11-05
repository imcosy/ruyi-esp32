#ifndef AFE_AUDIO_PROCESSOR_H
#define AFE_AUDIO_PROCESSOR_H

#include <esp_afe_sr_models.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

#include "audio_processor.h"
#include "audio_codec.h"

class AfeAudioProcessor : public AudioProcessor {
public:
    AfeAudioProcessor();
    ~AfeAudioProcessor();

    void Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) override;
    void Feed(std::vector<int16_t>&& data) override;
    void Start() override;
    void Stop() override;
    bool IsRunning() override;
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override;
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;
    size_t GetFeedSize() override;
    void EnableDeviceAec(bool enable) override;
    void ResetVadState() override;
    // ★ 请求重置（2026-09-16 WDT 崩溃根修）：置标志，fetch 任务自执行缓冲重置（零竞态）
    void RequestReset() override { reset_pending_.store(true); }
    int GetLatestRms() const override { return latest_rms_.load(); }
    int GetNoiseBaseline() const override { return noise_baseline_.load(); }

private:
    EventGroupHandle_t event_group_ = nullptr;
    const esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    std::function<void(std::vector<int16_t>&& data)> output_callback_;
    std::function<void(bool speaking)> vad_state_change_callback_;
    AudioCodec* codec_ = nullptr;
    int afe_frame_samples_ = 0;
    int frame_samples_ = 0;
    bool is_speaking_ = false;
    std::vector<int16_t> input_buffer_;
    std::mutex input_buffer_mutex_;
    std::vector<int16_t> output_buffer_;
    TaskHandle_t task_handle_ = nullptr;
    volatile bool task_running_ = false;
    std::atomic<int> latest_rms_{0};  // 最近一帧AFE输出（AEC/AGC后真实上行音频）的近场能量RMS
    std::atomic<int> noise_baseline_{0};  // 自适应底噪基线（安静期快速下探/缓变区微上浮/疑似语音冻结）
    std::atomic<bool> reset_pending_{false};  // AFE 输入缓冲重置请求（打断后 AEC 参考失配根治）

    void AudioProcessorTask();
};

#endif 