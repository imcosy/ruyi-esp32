#ifndef AUDIO_PROCESSOR_H
#define AUDIO_PROCESSOR_H

#include <string>
#include <vector>
#include <functional>

#include <model_path.h>
#include "audio_codec.h"

class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    
    virtual void Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) = 0;
    virtual void Feed(std::vector<int16_t>&& data) = 0;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() = 0;
    virtual void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) = 0;
    virtual void OnVadStateChange(std::function<void(bool speaking)> callback) = 0;
    virtual size_t GetFeedSize() = 0;
    virtual void EnableDeviceAec(bool enable) = 0;
    virtual void ResetVadState() = 0;
    // ★ 请求重置 AFE 输入缓冲（2026-09-16 WDT 崩溃根修）：打断/停播后 TTS 回采
    //   骤停 → AEC 远端参考突变失配 → 滤波器病态计算 + FEED ringbuffer 溢出。
    //   重置动作在 fetch 任务上下文自执行（零竞态），NoAudioProcessor 默认空实现。
    virtual void RequestReset() {}
    // ★ 最近一帧 AFE 输出的近场能量 RMS（能量端点检测数据源，默认0=不支持）
    virtual int GetLatestRms() const { return 0; }
    // ★ 自适应底噪基线（语音判定=rms≥基线×SNR比值，默认0=不支持）
    virtual int GetNoiseBaseline() const { return 0; }
};

#endif
