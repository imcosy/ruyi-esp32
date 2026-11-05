#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <memory>
#include <atomic>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <mutex>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>
#include <model_path.h>
#include "esp_audio_enc.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_types.h"
#include "../protocol.h"  // AudioStreamPacket / PsramAllocator（显式包含，消除对TU包含顺序的隐式依赖）

#include "audio_codec.h"
#include "audio_processor.h"
#include "wake_word.h"
#include "protocol.h"
#include "ogg_demuxer.h"
// 上行已改流式 opus_raw 直发（无 OGG 容器），ogg_muxer.h 不再引入

// ★ 能量端点检测的语音信噪比阈值：近场 RMS ≥ 底噪基线×3 即判定语音。
//   相对底噪而非绝对阈值——AGC 会把持续环境音放大逼近人声电平，固定阈值必然失效。
//   人声比底噪高 10-20dB（3-10倍），取 3 倍（+9.5dB）两侧均有余量。
static constexpr int VAD_VOICE_SNR_RATIO = 3;

/*
 * There are two types of audio data flow:
 * 1. (MIC) -> [Processors] -> {Encode Queue} -> [Opus Encoder] -> {Send Queue} -> (Server)
 * 2. (Server) -> {Decode Queue} -> [Opus Decoder] -> {Playback Queue} -> (Speaker)
 *
 * We use one task for MIC / Speaker / Processors, and one task for Opus Encoder / Opus Decoder.
 * 
 * Decode Queue and Send Queue are the main queues, because Opus packets are quite smaller than PCM packets.
 * 
 */

#define OPUS_FRAME_DURATION_MS 20
#define MAX_ENCODE_TASKS_IN_QUEUE 2
// ★ 播放/解码队列加大：音频缓冲(PCM帧)已改为PSRAM分配(见protocol.h PsramAllocator)，
//   内部SRAM不再被音频包挤占，队列可深度缓冲(数秒)以平滑网络抖动/服务端突发，
//   解决TTS因缓冲深度不足(~280ms)导致的下溢卡顿。
#define MAX_PLAYBACK_TASKS_IN_QUEUE 200
// ★ decode 队列上限：deque 元素+payload 都在 PSRAM，超长 TTS（2分钟会议总结≈6000包）
//   若无界堆积会把 PSRAM 打到分配失败。1200 包(24s) 远超任何合法流式提前量
//   （服务端长播报已按实时速率整形下发，见 s2s_meeting pace），仅在异常狂发时触顶丢最旧。
#define MAX_DECODE_PACKETS_IN_QUEUE 1200
// ★ 上行发送队列上限（2.4s 实时音频）：队列仅由 App 主循环消费，主循环一旦停摆
//   就会无界增长吃光 PSRAM（见 audio_service.cc OpusEncodeTask 注释），必须硬上限。
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define MAX_TIMESTAMPS_IN_QUEUE 3

#define AUDIO_POWER_TIMEOUT_MS 15000
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000

#define AS_EVENT_WAKE_WORD_RUNNING          (1 << 0)
#define AS_EVENT_AUDIO_PROCESSOR_RUNNING    (1 << 2)
#define AS_EVENT_PLAYBACK_NOT_EMPTY         (1 << 3)

#define AS_OPUS_GET_FRAME_DRU_ENUM(duration_ms)                   \
    ((duration_ms) == 5 ? ESP_OPUS_ENC_FRAME_DURATION_5_MS :      \
     (duration_ms) == 10 ? ESP_OPUS_ENC_FRAME_DURATION_10_MS :    \
     (duration_ms) == 20 ? ESP_OPUS_ENC_FRAME_DURATION_20_MS :    \
     (duration_ms) == 40 ? ESP_OPUS_ENC_FRAME_DURATION_40_MS :    \
     (duration_ms) == 60 ? ESP_OPUS_ENC_FRAME_DURATION_60_MS :    \
     (duration_ms) == 80 ? ESP_OPUS_ENC_FRAME_DURATION_80_MS :    \
     (duration_ms) == 100 ? ESP_OPUS_ENC_FRAME_DURATION_100_MS :  \
     (duration_ms) == 120 ? ESP_OPUS_ENC_FRAME_DURATION_120_MS : -1)

#define AS_OPUS_ENC_CONFIG() {                                                                                    \
        .sample_rate        = 16000,                                                                              \
        .channel            = ESP_AUDIO_MONO,                                                                     \
        .bits_per_sample    = ESP_AUDIO_BIT16,                                                                    \
        /* ★ 码率必须显式指定：AUTO 实测只给 48字节/20ms≈19.2kbps，opus 自动窄带化    */\
        /*   （4-8kHz 高频全丢），而声母 m/h/p/s 的区分全靠高频摩擦音 → "茂哥"识别成  */\
        /*   "浩哥/炮哥"、音节切分错乱（"贵一是"）。32kbps=完整宽带，20ms帧80字节，    */\
        /*   上行带宽完全无压力。对照 esp_opus_enc.h 官方默认 bitrate=90000/VOIP/CBR   */\
        .bitrate            = 32000,                                                                              \
        .frame_duration     = (esp_opus_enc_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(OPUS_FRAME_DURATION_MS),  \
        /* ★ VOIP 语音模式：低码率下为语音优化频带分配；AUDIO 音乐模式会把码率浪费在  */\
        /*   低频谐波上，语音可懂度反而更差                                            */\
        .application_mode   = ESP_OPUS_ENC_APPLICATION_VOIP,                                                        \
        .complexity         = 0,                                                                                  \
        .enable_fec         = false,                                                                              \
        .enable_dtx         = false,                                                                              \
        /* ★ DTX必须关闭：DTX会把静音帧编码成0字节，主循环跳过空payload后            */\
        /* listening_audio_bytes_永不增长 → "VAD误触发但本轮无有效音频"整轮丢弃。   */\
        /* NSNet2把低电平输入压成纯数字静音时必触发此路径。                          */\
        .enable_vbr         = false,                                                                              \
    }

struct AudioServiceCallbacks {
    std::function<void(void)> on_send_queue_available;
    std::function<void(const std::string&)> on_wake_word_detected;
    std::function<void(bool)> on_vad_change;           // AFE VAD
};


enum AudioTaskType {
    kAudioTaskTypeEncodeToSendQueue,
    kAudioTaskTypeDecodeToPlaybackQueue,
};

struct AudioTask {
    AudioTaskType type;
    // ★ PCM 数据放在 PSRAM（PsramAllocator），避免耗尽内部 SRAM、支持深度缓冲
    std::vector<int16_t, PsramAllocator<int16_t>> pcm;
    uint32_t timestamp;
};

struct DebugStatistics {
    uint32_t input_count = 0;
    uint32_t decode_count = 0;
    uint32_t encode_count = 0;
    uint32_t playback_count = 0;
};

class AudioService {
public:
    AudioService();
    ~AudioService();

    void Initialize(AudioCodec* codec);
    void Start();
    void Stop();
    void EncodeWakeWord();
    std::unique_ptr<AudioStreamPacket> PopWakeWordPacket();
    const std::string& GetLastWakeWord() const;
    // 最近一次检测唤醒词的 action 类型（"wake"普通 / "meeting"会议直达）
    std::string GetLastWakeWordAction() const;
    bool IsVoiceDetected() const { return voice_detected_; }
    // ★ 最近一帧 AFE 输出（AEC/AGC后真实上行音频）的近场能量 RMS + 自适应底噪基线。
    //   语音判定 = rms >= baseline × VAD_VOICE_SNR_RATIO（相对底噪，任何环境自适应）
    int GetLatestInputRms() const { return audio_processor_ ? audio_processor_->GetLatestRms() : 0; }
    int GetNoiseBaseline() const { return audio_processor_ ? audio_processor_->GetNoiseBaseline() : 0; }
    bool IsIdle();
    void WaitForPlaybackQueueEmpty();
    bool IsWakeWordRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_WAKE_WORD_RUNNING; }
    bool IsAudioProcessorRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING; }

    void EnableWakeWordDetection(bool enable);
    void EnableVoiceProcessing(bool enable);
    void ResetVoiceProcessing();
    void EnableAudioSending(bool enable);
    bool IsAudioSendingEnabled() const { return send_audio_enabled_; }
    // ★ 是否正在向硬件输出音频（AudioOutputTask 阻塞写 I2S 期间 + DMA 尾音播完前为 true）。
    // OutputData 返回后 DMA 缓冲里还有 160ms 音频在播，若此时切 Listening 开麦，
    // 这段无 AEC 参考信号的尾音会被麦克风回采触发 VAD → 误发空轮(ASR空文本/0字节) → 卡死。
    // 因此 "真正播完" = 阻塞写 I2S 完成 && 距完成 >= AUDIO_OUTPUT_DMA_TAIL_US。
    bool IsAudioOutputActive() const {
        if (is_audio_output_active_) return true;
        int64_t elapsed = esp_timer_get_time() - last_output_finish_us_.load();
        return elapsed >= 0 && elapsed < AUDIO_OUTPUT_DMA_TAIL_US;
    }
    void EnableDeviceAec(bool enable);

    void SetCallbacks(AudioServiceCallbacks& callbacks);

    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);
    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();
    void PlaySound(const std::string_view& sound);
    bool ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples);
    void ResetDecoder();
    void SetModelsList(srmodel_list_t* models_list);
    
    // 设置本地唤醒词列表（从服务器获取）
    void SetLocalWakeWords(const std::vector<std::pair<std::string, std::string>>& wake_words);

    void SetPcmMode(bool pcm_mode);
    void StopPlayback();

private:
    AudioCodec* codec_ = nullptr;
    AudioServiceCallbacks callbacks_;
    std::unique_ptr<AudioProcessor> audio_processor_;
    std::unique_ptr<WakeWord> wake_word_;
    void* opus_encoder_ = nullptr;
    void* opus_decoder_ = nullptr;
    std::mutex decoder_mutex_;
    std::mutex input_resampler_mutex_;
    esp_ae_rate_cvt_handle_t input_resampler_ = nullptr;
    esp_ae_rate_cvt_handle_t output_resampler_ = nullptr;
    
    // Encoder/Decoder state
    int encoder_sample_rate_ = 48000;
    int encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
    int encoder_frame_size_ = 0;
    int encoder_outbuf_size_ = 0;
    int decoder_sample_rate_ = 0;
    int decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
    int decoder_frame_size_ = 0;
    bool use_pcm_mode_ = false;
    esp_ae_rate_cvt_handle_t pcm_resampler_ = nullptr;
    int pcm_resampler_src_rate_ = 0;

    std::atomic<bool> is_audio_output_active_{false};  // AudioOutputTask 正在写 I2S
    std::atomic<int64_t> last_output_finish_us_{0};    // 最近一次 OutputData 完成的时刻(us)
    // I2S DMA 缓冲时长：8 描述符 × 480 帧 = 3840 帧 @24000Hz = 160ms。
    // OutputData(Write) 返回时 DMA 里还有这段音频在播，切 Listening 前必须等它播完，
    // 否则这段无 AEC 参考信号的尾音会被麦克风回采触发 VAD。
    static constexpr int64_t AUDIO_OUTPUT_DMA_TAIL_US = 160000;
    DebugStatistics debug_statistics_;
    srmodel_list_t* models_list_ = nullptr;
    
    // 本地唤醒词列表（从服务器获取）
    std::vector<std::pair<std::string, std::string>> local_wake_words_;

    EventGroupHandle_t event_group_;

    // Audio encode / decode
    TaskHandle_t audio_input_task_handle_ = nullptr;
    TaskHandle_t audio_output_task_handle_ = nullptr;
    // ★ encode/decode 拆双任务（2026-09-16 meeting 下行断流根修）：
    //   原单 opus_codec 任务串行处理上行编码(20ms/帧持续)+下行解码，下行 burst 时
    //   播放队列被输出任务清空后靠"每轮单包投喂"续命，编码/调度一抖 DMA 即空转断流
    TaskHandle_t opus_encode_task_handle_ = nullptr;
    TaskHandle_t opus_decode_task_handle_ = nullptr;
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    // ★ 全部值语义+PSRAM节点（20260915 0030）：元素对象不再 new 到内部 SRAM，
    //   TTS 长播报队列堆积只耗 PSRAM，SRAM 不再被音频缓冲打穿 → LoadProhibited 崩溃根除
    PsramDeque<AudioStreamPacket> audio_decode_queue_;
    PsramDeque<AudioStreamPacket> audio_send_queue_;
    PsramDeque<AudioTask> audio_encode_queue_;
    PsramDeque<AudioTask> audio_playback_queue_;
    // For server AEC
    // ★ 2026-09-18：timestamp_queue_ 死代码删除——全工程无任何 push（只在 ResetDecoder 里 clear），
    //   取时间戳分支永不成立，留着误导排障。

    bool wake_word_initialized_ = false;
    bool audio_processor_initialized_ = false;
    // ★ 2026-09-18 跨任务共享标量改原子（AFE fetch 任务读/写、主循环写、LED 回调读）：
    //   原裸 bool 存在"主循环已置 true、AFE 任务仍读到旧 false 丢帧"的窗口。
    std::atomic<bool> voice_detected_{false};        // AFE VAD状态
    std::atomic<bool> send_audio_enabled_{false};
    std::atomic<bool> audio_input_need_warmup_{false};
    bool service_stopped_ = true;

    // ★ 上行健康监视（2026-09-18 现场排障埋点）：mic 原始 RMS（ReadAudioData 采集）
    //   与 AFE 输出 RMS（上行实际内容），每 2s 在"发送使能中"打一行。
    //   用途：区分"麦克风没声"（micRMS≈0）与"AFE 抑制/静音输出"（micRMS正常、outRMS≈0）
    //   ——这正是"聆听中用户说话没反应"两类根因的分水岭。
    std::atomic<uint32_t> mic_rms_peak_{0};
    std::atomic<uint32_t> afe_out_rms_peak_{0};
    std::atomic<uint32_t> afe_out_frames_{0};
    int64_t last_upstream_log_us_ = 0;
    int64_t monitor_skip_until_us_ = 0;   // 上行监视跳过段（AFE 启动 warmup 垃圾帧）

    std::chrono::steady_clock::time_point last_input_time_;
    std::chrono::steady_clock::time_point last_output_time_;

    void AudioInputTask();
    void AudioOutputTask();
    void OpusEncodeTask();
    void OpusDecodeTask();
    void AudioPowerTask();
    void PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm);
    void SetDecodeSampleRate(int sample_rate, int frame_duration);
    void CheckAndUpdateAudioPowerState();
};

#endif