#include "audio_service.h"
#include <esp_log.h>
#include <cstring>
#include <cmath>
#include <algorithm>

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)        \
    (esp_ae_rate_cvt_cfg_t)                                  \
    {                                                        \
        .src_rate        = (uint32_t)(_src_rate),            \
        .dest_rate       = (uint32_t)(_dest_rate),           \
        .channel         = (uint8_t)(_channel),              \
        .bits_per_sample = ESP_AUDIO_BIT16,                  \
        .complexity      = 2,                                \
        .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,  \
    }

#define OPUS_DEC_CFG(_sample_rate, _frame_duration_ms)                                                    \
    (esp_opus_dec_cfg_t)                                                                                  \
    {                                                                                                     \
        .sample_rate    = (uint32_t)(_sample_rate),                                                       \
        .channel        = ESP_AUDIO_MONO,                                                                 \
        .frame_duration = (esp_opus_dec_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(_frame_duration_ms),  \
        .self_delimited = false,                                                                          \
    }

#if CONFIG_USE_AUDIO_PROCESSOR
#include "processors/afe_audio_processor.h"
#else
#include "processors/no_audio_processor.h"
#endif

#if CONFIG_IDF_TARGET_ESP32S3
#include "wake_words/custom_wake_word.h"
#endif

#define TAG "AudioService"

AudioService::AudioService() {
    event_group_ = xEventGroupCreate();
}

AudioService::~AudioService() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
    if (opus_encoder_ != nullptr) {
        esp_opus_enc_close(opus_encoder_);
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
    }
    if (input_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(input_resampler_);
    }
    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
    }
    if (pcm_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(pcm_resampler_);
    }
}

void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(codec->output_sample_rate(), OPUS_FRAME_DURATION_MS);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "创建音频解码器失败，错误代码：%d", ret);
    } else {
        decoder_sample_rate_ = codec->output_sample_rate();
        decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        decoder_frame_size_ = decoder_sample_rate_ / 1000 * OPUS_FRAME_DURATION_MS;
    }
    esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
    ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &opus_encoder_);
    if (opus_encoder_ == nullptr) {
        ESP_LOGE(TAG, "创建音频编码器失败，错误代码：%d", ret);
    } else {
        encoder_sample_rate_ = 16000;  // AFE 工作在 16kHz，编码器必须匹配
        encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_size_, &encoder_outbuf_size_);
        encoder_frame_size_ = encoder_frame_size_ / sizeof(int16_t);
    }

    if (codec->input_sample_rate() != 16000) {
        esp_ae_rate_cvt_cfg_t input_resampler_cfg = RATE_CVT_CFG(
            codec->input_sample_rate(), 16000, codec->input_channels());
        auto resampler_ret = esp_ae_rate_cvt_open(&input_resampler_cfg, &input_resampler_);
        if (input_resampler_ == nullptr) {
            ESP_LOGE(TAG, "创建输入重采样器失败，错误代码：%d", resampler_ret);
        }
    }

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        // ★ 2026-09-10 最终架构：去掉 AFE VAD gate 门控，上行无条件持续！
        //   设备端已不做任何 VAD 判定（服务端 Silero VAD + 声纹全权判定），
        //   VAD gate 门控（voice_detected_/vad_gate_open_）在 TTS 播放期间会把
        //   回采/插话误拦进回溯缓冲不上行 → 服务端收不到用户插话 → 无法打断。
        //   现在 send_audio_enabled_=true 即持续上行（含静音/回采帧），
        //   服务端声纹判定：用户声纹 sim≥0.4 → 打断/进入链路；否则丢弃。
        if (send_audio_enabled_) {
            // ★ 上行健康监视（2026-09-18）：AFE 输出 RMS 峰值（上行实际内容）。
            //   与 mic 原始 RMS 对照即可判定"麦克风没声"还是"AFE 抑制成静音"。
            if (esp_timer_get_time() >= monitor_skip_until_us_) {
                int64_t sq = 0;
                for (int16_t s : data) sq += (int64_t)s * s;
                uint32_t rms = data.empty() ? 0
                    : (uint32_t)sqrtf((float)(sq / (int64_t)data.size()));
                if (rms > afe_out_rms_peak_.load()) afe_out_rms_peak_.store(rms);
                afe_out_frames_.fetch_add(1);
                int64_t now_us = esp_timer_get_time();
                if (now_us - last_upstream_log_us_ >= 2000000) {
                    last_upstream_log_us_ = now_us;
                    ESP_LOGI(TAG, "上行监视: micRMS=%lu 输出RMS=%lu 输出帧=%lu（mic≈0=麦克风没声；输出≈0且mic正常=AFE抑制成静音）",
                             (unsigned long)mic_rms_peak_.exchange(0),
                             (unsigned long)afe_out_rms_peak_.exchange(0),
                             (unsigned long)afe_out_frames_.exchange(0));
                }
            }
            // 无条件上行（服务端判定一切）
            PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
        } else {
            // 上行关闭（Idle/断连等）：帧丢弃（服务端无需回溯缓冲，设备端不再打断）
            mic_rms_peak_.store(0);
            afe_out_rms_peak_.store(0);
            afe_out_frames_.store(0);
            last_upstream_log_us_ = esp_timer_get_time();
        }
    });

    audio_processor_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });
}

void AudioService::Start() {
    service_stopped_ = false;
    xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    // ★ 2026-09-18 根修（esp_timer 任务禁止阻塞）：音频电源巡检原为 esp_timer 回调，
    //   它要 enable/disable I2S 通道（阻塞调用）+ 取 codec 锁；而 esp_timer 任务同时
    //   承载 App 的 200ms 时钟 tick——该任务一旦被 I2S 阻塞，主循环就再也收不到
    //   CLOCK_TICK，永久阻塞在 xEventGroupWaitBits（整机卡死：无心跳/无重连/卡聆听中）。
    //   改为独立任务每 1s 巡检，与时钟彻底解耦。
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioPowerTask();
        vTaskDelete(NULL);
    }, "audio_power", 2048 * 2, this, 3, nullptr);

#if CONFIG_USE_AUDIO_PROCESSOR
    /* Start the audio input task */
    /* ★ 2026-09-19 栈扩容根修（0059 一唤醒即崩：***ERROR*** A stack overflow in task
     *   audio_input）：本任务除读 I2S 外还同步调用 wake_word_->Feed（MultiNet 推理）与
     *   audio_processor_->Feed（AFE feed 路径），AFE 启用 NS（降噪）后该路径栈需求显著
     *   增加——6144B 不够，实测唤醒进入 listening 后立即溢出崩溃。扩到 16KB。 */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 8, this, 8, &audio_input_task_handle_, 0);

    /* Start the audio output task */
    /* ★ 绑核1+优先级6→7（2026-09-16 meeting TTS 播报卡根修）：audio_output 与 AFE
     *   任务同为优先级6且都不绑核——meeting 板 AFE 带 AEC HIGH_PERF（CPU 大户），
     *   与 LVGL 渲染、modem_event 风暴竞争时 audio_output 频繁让出 CPU，喂 DMA 的
     *   240ms 批量一旦迟到即断流出声（实测 sisi 无 AEC 反而流畅 = 调度竞争证据）。
     *   提到 7 并绑核1：优先级高于 AFE(6)，抢占不再发生；绑核1与 audio_input(核0)
     *   分离，输入输出互不挤占。OutputData 大部分时间阻塞在 DMA 等待，核心让出，
     *   不会饿死同核其他任务。 */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048 * 2, this, 7, &audio_output_task_handle_, 1);
#else
    /* Start the audio input task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 2, this, 8, &audio_input_task_handle_);

    /* Start the audio output task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048, this, 6, &audio_output_task_handle_);
#endif

    /* Start the opus decode task */
    /* ★ encode/decode 拆双任务（2026-09-16 meeting 下行断流根修）：原单 opus_codec
     *   任务串行处理上行编码(20ms/帧持续)+下行解码且每轮只搬1包——下行 burst 时
     *   播放队列被 audio_output 清空后靠"单包投喂"续命，编码/调度一抖 DMA 即空转
     *   断流（实测 decode 队列 96→285 积压、播放队列=0、gap 40-65ms 反复）。
     *   decode 独立成任务绑核0（输入核，优先级6低于 audio_input(8)），并批量回填
     *   播放队列喂饱 audio_output（核1优先级7）的 240ms 深批量写入。 */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->OpusDecodeTask();
        vTaskDelete(NULL);
    }, "opus_decode", 2048 * 10, this, 6, &opus_decode_task_handle_, 0);

    /* Start the opus encode task */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->OpusEncodeTask();
        vTaskDelete(NULL);
    }, "opus_encode", 2048 * 12, this, 5, &opus_encode_task_handle_, 0);
}

void AudioService::Stop() {
    service_stopped_ = true;
    xEventGroupSetBits(event_group_,
        AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    audio_encode_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_queue_cv_.notify_all();
}

void AudioService::StopPlayback() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    // ★ 只清空队列，不关闭I2S输出通道！
    // 关闭I2S(EnableOutput(false))是昂贵操作（重新配置GPIO+DMA+时钟），
    // 且与AudioOutputTask的OutputData()/EnableOutput(true)存在竞态条件导致崩溃。
    // I2S的开关交给CheckAndUpdateAudioPowerState()超时机制管理。
    audio_playback_queue_.clear();
    audio_decode_queue_.clear();
    audio_queue_cv_.notify_all();
    // ★ AFE 输入缓冲重置（2026-09-16 WDT 崩溃根修）：打断瞬间 TTS 回采骤停 →
    //   AEC 远端参考突变失配 → 滤波器病态计算占死 CPU1 + FEED ringbuffer 溢出。
    //   置重置标志，由 fetch 任务在自身上下文自执行（零竞态）。
    if (audio_processor_) {
        audio_processor_->RequestReset();
    }
}

bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (input_resampler_ != nullptr) {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            uint32_t in_sample_num = data.size() / codec_->input_channels();
            uint32_t output_samples = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(input_resampler_, in_sample_num, &output_samples);
            auto resampled = std::vector<int16_t>(output_samples * codec_->input_channels());
            uint32_t actual_output = output_samples;
            esp_ae_rate_cvt_process(input_resampler_, (esp_ae_sample_t)data.data(), in_sample_num,
                                   (esp_ae_sample_t)resampled.data(), &actual_output);
            resampled.resize(actual_output * codec_->input_channels());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;

    // ★ 上行健康监视（2026-09-18）：mic 原始信号 RMS 峰值（重采样前的 codec 直读数据，
    //   与 AFE 内部 AGC/NS 无关）。与 AFE 输出 RMS 对照即可分辨链路故障位置。
    if (esp_timer_get_time() >= monitor_skip_until_us_) {
        int64_t sq = 0;
        for (int16_t s : data) sq += (int64_t)s * s;
        if (!data.empty()) {
            uint32_t rms = (uint32_t)sqrtf((float)(sq / (int64_t)data.size()));
            if (rms > mic_rms_peak_.load()) mic_rms_peak_.store(rms);
        }
    }

    return true;
}

void AudioService::AudioInputTask() {
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_,
            AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING,
            pdFALSE, pdFALSE, portMAX_DELAY);

        if (service_stopped_) {
            break;
        }
        if (audio_input_need_warmup_) {
            audio_input_need_warmup_ = false;
            // ★ I2S刚启动/AFE刚初始化后，前几帧数据不稳定，丢弃
            // 50ms ≈ 5帧@10ms/帧，足够让I2S DMA和AFE pipeline稳定
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Feed the wake word and/or audio processor */
        if (bits & (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
            int samples = codec_->input_sample_rate() / 100; // 10ms
            std::vector<int16_t> data;
            if (ReadAudioData(data, codec_->input_sample_rate(), samples)) {
                if (bits & AS_EVENT_WAKE_WORD_RUNNING) {
                    wake_word_->Feed(data);
                }
                if (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) {
                    // ★ 用户决策：禁用软件AEC参考注入。本板（BoxAudioCodec/ES7210）
                    //   ch1 是硬件 loopback 参考（与原生小智 ESP-Box 同设计，天然时间对齐），
                    //   软件注入会用延迟靠猜的缓冲覆写真实参考 → AEC 发散误消用户语音。
                    audio_processor_->Feed(std::move(data));
                }
                // ★ 不再 vTaskDelay(1)（2026-09-14 唤醒失灵根因之一）：读10ms+delay1ms
                //   =11ms周期，产率10ms → I2S RX DMA 每秒积压~90ms音频，缓冲满后丢弃
                //   → MultiNet/AFE 收到的音频带周期性空洞。i2s_channel_read 本身阻塞到
                //   下一帧就绪，无需人为延时，删除后周期=10ms 对齐 DMA 产率，零空洞。
                continue;
            }
        }

        // Read timeout/error should not terminate the input task.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void AudioService::AudioOutputTask() {
    // ★ 用户决策：收到即播、只排队、不人为等待（撤掉原2s起播预缓冲/3s硬等逻辑）。
    //   队列空则阻塞等待，有数据立即批量写出，首音延迟=链路真实延迟。
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() { return !audio_playback_queue_.empty() || service_stopped_; });
        if (service_stopped_) {
            break;
        }

        /* ★ 批量取出任务合并为一大块（上限240ms）一次写入codec：
         *    服务端下行为20ms帧，若每20ms唤醒一次写I2S，modem_event(优先级22)的
         *    AT解析/HEX解码/SAUD解封风暴会抢占audio_output(优先级6)造成DMA断流→TTS卡顿。
         *    合并成大块后，单次写入的数据足够DMA播放较长时间，任务唤醒频率大幅下降，
         *    对调度的容忍度大幅提高。上限从120ms提到240ms：SAUD opus 帧解封入队更快，
         *    深批量进一步对抗接收风暴期间的调度饥饿。
         *    （不足240ms时有多少写多少立即出声，不凑批、不增加首音延时） */
        static std::vector<int16_t, PsramAllocator<int16_t>> batch_buffer;
        batch_buffer.clear();
        size_t batch_samples_cap = (size_t)codec_->output_sample_rate() / 1000 * 240; // ~240ms
        while (!audio_playback_queue_.empty() && batch_buffer.size() < batch_samples_cap) {
            auto& task = audio_playback_queue_.front();
            batch_buffer.insert(batch_buffer.end(), task.pcm.begin(), task.pcm.end());
            audio_playback_queue_.pop_front();
        }
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (batch_buffer.empty()) {
            continue;
        }

        // ★ 断流诊断（2026-09-16 TTS卡顿取证）：DMA 深度240ms。若本次写入距上次
        //   写入的间隔超过上一批时长，说明上一批播完后 DMA 空转（可闻断音）。
        //   短断流(<1.5s)=调度/下行抖动饿死输出路径（配合 audio_output 提优先级
        //   观察是否归零）；句间自然停顿(>1.5s)不记录，避免日志噪音。
        static int64_t s_last_write_us = 0;
        static size_t s_last_batch_ms = 0;
        int64_t now_us = esp_timer_get_time();
        size_t batch_ms = batch_buffer.size() * 1000 / codec_->output_sample_rate();
        if (s_last_write_us != 0) {
            int64_t gap_ms = (now_us - s_last_write_us) / 1000;
            if (gap_ms > (int64_t)s_last_batch_ms + 20 && gap_ms < 1500) {
                ESP_LOGW(TAG, "TTS输出断流: gap=%dms > 上批=%dms（DMA已空转，decode队列=%d 播放队列=%d）",
                         (int)gap_ms, (int)s_last_batch_ms,
                         (int)audio_decode_queue_.size(), (int)audio_playback_queue_.size());
            }
        }
        s_last_write_us = now_us;
        s_last_batch_ms = batch_ms;

        if (!codec_->output_enabled()) {
            codec_->EnableOutput(true);
        }

        // ★ 标记正在向硬件输出音频。OutputData 是阻塞写 I2S（可能持续到"块时长-160ms"),
        //   必须在整块真正播完(incl. DMA)后才清除，否则 IsIdle() 会提前返回 true。
        is_audio_output_active_ = true;
        codec_->OutputData(batch_buffer.data(), batch_buffer.size());
        is_audio_output_active_ = false;
        // ★ 记录本次输出完成时刻。IsAudioOutputActive() 会在之后的
        // AUDIO_OUTPUT_DMA_TAIL_US(160ms) 内仍返回 true（DMA 尾音还在播），
        //   保证切 Listening 一定发生在整段音频（含 DMA 尾音）出完之后，
        //   避免无 AEC 参考信号的尾音被麦克风回采触发 VAD 误发空轮。
        last_output_finish_us_.store(esp_timer_get_time());

        /* Update the last output time */
        last_output_time_ = std::chrono::steady_clock::now();
        debug_statistics_.playback_count++;
    }
}

void AudioService::OpusDecodeTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_ ||
                (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE);
        });
        if (service_stopped_) {
            break;
        }

        /* ★ 批量搬运（2026-09-16 下行断流根修）：循环搬满播放队列（上限200包）才睡，
         *   一次性喂饱 audio_output 的 240ms 深批量写 DMA。原单任务串行 encode+decode
         *   且每轮只搬1包，播放队列被输出任务清空后靠投喂续命，上行编码一抖 DMA 即空转。 */
        while (!audio_decode_queue_.empty() &&
               audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE &&
               !service_stopped_) {
            // ★ 值语义（20260915 0030）：元素从 PSRAM deque 移出到任务栈，栈上仅~64B对象头
            AudioStreamPacket packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            AudioTask task;
            task.type = kAudioTaskTypeDecodeToPlaybackQueue;
            task.timestamp = packet.timestamp;

            if (packet.codec == 0) {
                // PCM payload: directly interpret as int16_t samples, resample if needed
                size_t sample_count = packet.payload.size() / sizeof(int16_t);
                task.pcm.resize(sample_count);
                std::memcpy(task.pcm.data(), packet.payload.data(), packet.payload.size());

                int pcm_sample_rate = packet.sample_rate;
                if (pcm_sample_rate != codec_->output_sample_rate()) {
                    if (pcm_resampler_ == nullptr || pcm_resampler_src_rate_ != pcm_sample_rate) {
                        if (pcm_resampler_ != nullptr) {
                            esp_ae_rate_cvt_close(pcm_resampler_);
                            pcm_resampler_ = nullptr;
                        }
                        esp_ae_rate_cvt_cfg_t resampler_cfg = RATE_CVT_CFG(
                            pcm_sample_rate, codec_->output_sample_rate(), ESP_AUDIO_MONO);
                        auto ret = esp_ae_rate_cvt_open(&resampler_cfg, &pcm_resampler_);
                        if (ret != ESP_AE_ERR_OK || pcm_resampler_ == nullptr) {
                            ESP_LOGE(TAG, "创建PCM重采样器失败，错误代码：%d", ret);
                            pcm_resampler_ = nullptr;
                        } else {
                            pcm_resampler_src_rate_ = pcm_sample_rate;
                        }
                    }
                    if (pcm_resampler_ != nullptr) {
                        uint32_t target_size = 0;
                        esp_ae_rate_cvt_get_max_out_sample_num(pcm_resampler_, task.pcm.size(), &target_size);
                        // ★ PSRAM 化：重采样临时缓冲最大~17KB，此前 malloc 在内部 SRAM，
                        //   每60ms一次分配/释放，是 SRAM min 187B 的另一个推手
                        std::vector<int16_t, PsramAllocator<int16_t>> resampled(target_size);
                        uint32_t actual_output = target_size;
                        esp_ae_rate_cvt_process(pcm_resampler_, (esp_ae_sample_t)task.pcm.data(), task.pcm.size(),
                                                (esp_ae_sample_t)resampled.data(), &actual_output);
                        resampled.resize(actual_output);
                        task.pcm.assign(resampled.begin(), resampled.end());
                    }
                }

                lock.lock();
                audio_playback_queue_.push_back(std::move(task));
                audio_queue_cv_.notify_all();
                debug_statistics_.decode_count++;
            } else {
                // Opus payload: decode then resample
                SetDecodeSampleRate(packet.sample_rate, packet.frame_duration);
                if (opus_decoder_ != nullptr) {
                    // ★ 6 倍余量：SAUD opus 帧时长可能为 20/40/60ms，decoder_frame_size_
                    //   按 frame_duration 分配只够单帧，帧时长不匹配时 esp_opus_dec_decode
                    //   输出超出缓冲 → 解码失败。解码后按实际 decoded_size 收缩。
                    task.pcm.resize(decoder_frame_size_ * 6);
                    esp_audio_dec_in_raw_t raw = {
                        .buffer = (uint8_t *)(packet.payload.data()),
                        .len = (uint32_t)(packet.payload.size()),
                        .consumed = 0,
                        .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                    };
                    esp_audio_dec_out_frame_t out_frame = {
                        .buffer = (uint8_t *)(task.pcm.data()),
                        .len = (uint32_t)(task.pcm.size() * sizeof(int16_t)),
                        .decoded_size = 0,
                    };
                    esp_audio_dec_info_t dec_info = {};
                    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
                    auto ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &dec_info);
                    decoder_lock.unlock();
                    if (ret == ESP_AUDIO_ERR_OK) {
                        task.pcm.resize(out_frame.decoded_size / sizeof(int16_t));
                        if (decoder_sample_rate_ != codec_->output_sample_rate() && output_resampler_ != nullptr) {
                            uint32_t target_size = 0;
                            esp_ae_rate_cvt_get_max_out_sample_num(output_resampler_, task.pcm.size(), &target_size);
                            std::vector<int16_t, PsramAllocator<int16_t>> resampled(target_size);
                            uint32_t actual_output = target_size;
                            esp_ae_rate_cvt_process(output_resampler_, (esp_ae_sample_t)task.pcm.data(), task.pcm.size(),
                                                    (esp_ae_sample_t)resampled.data(), &actual_output);
                            resampled.resize(actual_output);
                            task.pcm.assign(resampled.begin(), resampled.end());
                        }
                        lock.lock();
                        audio_playback_queue_.push_back(std::move(task));
                        audio_queue_cv_.notify_all();
                        debug_statistics_.decode_count++;
                    } else {
                        ESP_LOGE(TAG, "音频解码失败，错误代码：%d", ret);
                        lock.lock();
                    }
                } else {
                    ESP_LOGE(TAG, "未配置音频解码器");
                    lock.lock();
                }
            }
        }
    }
}

void AudioService::OpusEncodeTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            /* ★ 对齐原版：encode不再受send队列深度门限限制（满时在push处丢弃最旧包），
             *    防止send队列积压→编码任务停转→encode队列填满→AFE fetch卡死 */
            return service_stopped_ || !audio_encode_queue_.empty();
        });
        if (service_stopped_) {
            break;
        }

        /* Encode the audio to send queue */
        if (!audio_encode_queue_.empty()) {
            // ★ 值语义（20260915 0030）：从 PSRAM deque 移出到任务栈
            AudioTask task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();
            // （2026-09-14 撤销上行 ×3 数字增益：上行电平由 AFE AGC 在人声段抬音负责，
            //   数字域把底噪同步放大 3 倍既伤 SNR 又有削波风险，与原生链路设计冲突）

            AudioStreamPacket packet;
            packet.frame_duration = OPUS_FRAME_DURATION_MS;
            packet.sample_rate = encoder_sample_rate_;
            packet.timestamp = task.timestamp;

            if (use_pcm_mode_) {
                // PCM mode: send raw PCM data directly without Opus encoding
                packet.codec = 0;
                packet.payload.assign(
                    reinterpret_cast<uint8_t*>(task.pcm.data()),
                    reinterpret_cast<uint8_t*>(task.pcm.data()) + task.pcm.size() * sizeof(int16_t));

                if (task.type == kAudioTaskTypeEncodeToSendQueue) {
                    {
                        std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                        /* ★ 对齐原版：实时音频过期的包无价值，send队列满时丢最旧，绝不积压 */
                        if (audio_send_queue_.size() >= MAX_SEND_PACKETS_IN_QUEUE) {
                            audio_send_queue_.pop_front();
                        }
                        audio_send_queue_.push_back(std::move(packet));
                    }
                    if (callbacks_.on_send_queue_available) {
                        callbacks_.on_send_queue_available();
                    }
                }
                debug_statistics_.encode_count++;
            } else if (opus_encoder_ != nullptr && task.pcm.size() == encoder_frame_size_) {
                std::vector<uint8_t> buf(encoder_outbuf_size_);
                esp_audio_enc_in_frame_t in = {
                    .buffer = (uint8_t *)(task.pcm.data()),
                    .len = (uint32_t)(encoder_frame_size_ * sizeof(int16_t)),
                };
                esp_audio_enc_out_frame_t out = {
                    .buffer = buf.data(),
                    .len = (uint32_t)encoder_outbuf_size_,
                    .encoded_bytes = 0,
                };
                auto ret = esp_opus_enc_process(opus_encoder_, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    packet.codec = 1;
                    packet.payload.assign(buf.data(), buf.data() + out.encoded_bytes);

                    // ★ 2026-09-18：0 字节帧（DTX 静音抑制）只在出现时告警——
                    //   正常恒为 0，原实现每 100 帧（2s）打印一条把串口刷满，
                    //   真正需要看日志时全被 ENC_DEBUG 噪音淹没。
                    if (out.encoded_bytes == 0) {
                        ESP_LOGW(TAG, "上行编码出现 0 字节帧（DTX 异常）");
                    }

                    if (task.type == kAudioTaskTypeEncodeToSendQueue) {
                        {
                            std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                            /* ★ 2026-09-18 根修（设备卡死后 PSRAM 耗尽 abort）：Opus 分支
                             *   此前**没有上限**，只有 PCM 分支有丢最旧逻辑。发送队列仅由
                             *   App 主循环消费，一旦主循环停摆（时钟停跳），本队列会按
                             *   ~250B/20ms 无界增长——实测 15 分钟吃光 PSRAM，随后
                             *   PsramAllocator 分配 640B 失败直接 abort 整机。
                             *   实时音频过期即废：与 PCM 分支同规则，满即丢最旧，绝不积压。 */
                            if (audio_send_queue_.size() >= MAX_SEND_PACKETS_IN_QUEUE) {
                                audio_send_queue_.pop_front();
                            }
                            audio_send_queue_.push_back(std::move(packet));
                        }
                        if (callbacks_.on_send_queue_available) {
                            callbacks_.on_send_queue_available();
                        }
                    }
                    debug_statistics_.encode_count++;
                } else {
                    ESP_LOGE(TAG, "音频编码失败，错误代码：%d", ret);
                }
            } else {
                ESP_LOGE(TAG, "音频编码失败：未配置编码器或帧大小无效（期望 %u，实际 %u）",
                         encoder_frame_size_, task.pcm.size());
            }
            lock.lock();
        }
    }
}

void AudioService::SetPcmMode(bool pcm_mode) {
    use_pcm_mode_ = pcm_mode;
    ESP_LOGI(TAG, "音频模式设置: %s", pcm_mode ? "PCM" : "Opus");
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (decoder_sample_rate_ == sample_rate && decoder_duration_ms_ == frame_duration) {
        return;
    }
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
    }
    decoder_lock.unlock();
    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(sample_rate, frame_duration);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "创建音频解码器失败，错误代码：%d", ret);
        return;
    }
    decoder_sample_rate_ = sample_rate;
    decoder_duration_ms_ = frame_duration;
    decoder_frame_size_ = decoder_sample_rate_ / 1000 * frame_duration;

    auto codec = Board::GetInstance().GetAudioCodec();
    if (decoder_sample_rate_ != codec->output_sample_rate()) {
        if (output_resampler_ != nullptr) {
            esp_ae_rate_cvt_close(output_resampler_);
            output_resampler_ = nullptr;
        }
        esp_ae_rate_cvt_cfg_t output_resampler_cfg = RATE_CVT_CFG(
            decoder_sample_rate_, codec->output_sample_rate(), ESP_AUDIO_MONO);
        auto resampler_ret = esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
        if (output_resampler_ == nullptr) {
            ESP_LOGE(TAG, "创建输出重采样器失败，错误代码：%d", resampler_ret);
        }
    }
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    // ★ 值语义（20260915 0030）：task 对象随 PSRAM deque 节点分配，不占内部 SRAM
    AudioTask task;
    task.type = type;
    task.pcm.assign(pcm.begin(), pcm.end());  // 复制进PSRAM缓冲
    /* Push the task to the encode queue */
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);

    /* ★ 2026-09-18：原 timestamp_queue_ 取时间戳分支删除——该队列全工程无 push
     *   （只在 ResetDecoder 里 clear），条件永不成立，纯死代码（排障时误导）。 */

    /* ★ 对齐xiaozhi原版：麦克风音频是实时的，encode队列满时丢弃最旧帧，绝不阻塞。
     *    阻塞此处会卡死AFE fetch任务→VAD失效→麦克风无声（网络拥塞时send队列
     *    停止排空→OpusCodecTask停转→encode队列(仅2深)填满→AFE任务死锁）。
     *    原版注释原话："Blocking here would stall the audio engine task (AFE fetch)
     *    and deadlock the whole input pipeline when the send queue stops being drained." */
    if (audio_encode_queue_.size() >= MAX_ENCODE_TASKS_IN_QUEUE) {
        audio_encode_queue_.pop_front();
    }
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    // ★ 按需求移除客户端控流：不做任何阻塞/背压，收到即入队，依次解码播放。
    //   （原阻塞式背压会卡住WS接收线程→TCP停读→到达突发→播放断续）
    //   ★ 20260915 0030：元素对象+deque节点全部落PSRAM（PsramDeque 值语义），
    //   另加硬上限丢最旧——此前队列完全无界，超长TTS（2分钟会议总结≈6000包）
    //   直接把内部SRAM堆打穿到187B → LoadProhibited 崩溃（本次修复的根因）。
    (void)wait;
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        audio_decode_queue_.pop_front();
        ESP_LOGW(TAG, "decode队列已满(%u)，丢弃最旧包", (unsigned)audio_decode_queue_.size());
    }
    audio_decode_queue_.push_back(std::move(*packet));
    audio_queue_cv_.notify_all();
    return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    // ★ 值出队后包回 unique_ptr（仅瞬态单对象在内部堆，不随队列深度堆积）
    auto packet = std::make_unique<AudioStreamPacket>(std::move(audio_send_queue_.front()));
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

void AudioService::EncodeWakeWord() {
    if (wake_word_) {
        wake_word_->EncodeWakeWordData();
    }
}

const std::string& AudioService::GetLastWakeWord() const {
    return wake_word_->GetLastDetectedWakeWord();
}

std::string AudioService::GetLastWakeWordAction() const {
    return wake_word_ ? wake_word_->GetLastDetectedAction() : "";
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    std::vector<uint8_t> opus;  // 唤醒词数据量小，临时默认分配器缓冲即可
    if (wake_word_->GetWakeWordOpus(opus)) {
        packet->payload.assign(opus.begin(), opus.end());
        return packet;
    }
    return nullptr;
}

void AudioService::EnableWakeWordDetection(bool enable) {
    if (!wake_word_) {
        return;
    }

    ESP_LOGD(TAG, "%s唤醒词检测", enable ? "启用" : "禁用");
    if (enable) {
        if (!wake_word_initialized_) {
            if (!wake_word_->Initialize(codec_, models_list_)) {
                ESP_LOGE(TAG, "唤醒词初始化失败");
                return;
            }
            wake_word_initialized_ = true;
        }
        // Reset input resampler to clear cached data from previous mode (e.g. AudioProcessor)
        // This prevents buffer overflow when switching between different feed sizes
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        wake_word_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        wake_word_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    }
}

void AudioService::EnableVoiceProcessing(bool enable) {
    if (enable) {
        // ★ 在AFE初始化之前先开启I2S输入通道，让硬件稳定运行
        // 避免AFE Initialize时I2S重新配置导致前几百毫秒采样数据是垃圾
        if (!codec_->input_enabled()) {
            codec_->EnableInput(true);
            vTaskDelay(pdMS_TO_TICKS(50));  // 等I2S DMA buffer填满稳定
        }

        if (!audio_processor_initialized_) {
            audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
            audio_processor_initialized_ = true;
        }

        /* We should make sure no audio is playing */
        ResetDecoder();
        audio_input_need_warmup_ = true;
        // Reset input resampler to clear cached data from previous mode (e.g. WakeWord)
        // This prevents buffer overflow when switching between different feed sizes
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        audio_processor_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
        // ★ 上行监视统计窗随 AFE 启动重置：AFE 刚启动的前几帧是 warmup 垃圾数据
        //   （实测 micRMS 会瞬时打到 32767 饱和值），不重置会让首行监视日志失真误导排障。
        mic_rms_peak_.store(0);
        afe_out_rms_peak_.store(0);
        afe_out_frames_.store(0);
        last_upstream_log_us_ = esp_timer_get_time();
        // ★ 2026-09-18⑨：AFE 启动后前 300ms 是 warmup 垃圾帧（实测 micRMS 会瞬时顶到
        //   32767 饱和值），监视埋点跳过这段再统计，避免首行日志误导排障。
        monitor_skip_until_us_ = esp_timer_get_time() + 300000;
    } else {
        audio_processor_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
}

void AudioService::ResetVoiceProcessing() {
    if (audio_processor_initialized_) {
        audio_processor_->ResetVadState();
    }
}

void AudioService::EnableAudioSending(bool enable) {
    // ★ 2026-09-18 不变式根修（"假聆听：状态机在聆听、子上行却 0 字节"）：
    //   上行使能与 AFE 处理器运行是两个独立开关，历史上出现过"处理器被停掉
    //   （WifiConfig/切模式）后某条恢复路径只 EnableAudioSending(true)"的组合——
    //   此时 AFE 不产帧（Feed 在 IsRunning 早退），设备静静聆听、服务端零音频。
    //   上行使能 = 处理器必须在跑：在这里一并拉起，两条开关不再可能脱钩。
    if (enable && !IsAudioProcessorRunning()) {
        ESP_LOGW(TAG, "EnableAudioSending(true) 时 processor 未运行 → 一并启动处理器");
        EnableVoiceProcessing(true);
    }
    // ★ 诊断：记录发送开关切换（排查"恢复聆听后音频0字节"）
    if (enable != send_audio_enabled_) {
        ESP_LOGI(TAG, "SendAudio: %s→%s",
                 send_audio_enabled_ ? "on" : "off", enable ? "on" : "off");
    }
    send_audio_enabled_ = enable;
    if (!enable) {
        // ★ 关闭发送：清发送/编码队列（2026-09-10 起不再有 VAD gate/pre_roll 门控）
        {
            std::lock_guard<std::mutex> lock(audio_queue_mutex_);
            audio_send_queue_.clear();
            audio_encode_queue_.erase(
                std::remove_if(audio_encode_queue_.begin(), audio_encode_queue_.end(),
                    [](const AudioTask& task) {
                        return task.type == kAudioTaskTypeEncodeToSendQueue;
                    }),
                audio_encode_queue_.end());
            audio_queue_cv_.notify_all();
        }
    }
}

void AudioService::EnableDeviceAec(bool enable) {
    if (!audio_processor_initialized_) {
        audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
        audio_processor_initialized_ = true;
    }

    audio_processor_->EnableDeviceAec(enable);
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
}

void AudioService::PlaySound(const std::string_view& ogg) {
    if (!codec_->output_enabled()) {
        codec_->EnableOutput(true);
    }

    const auto* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();

    auto demuxer = std::make_unique<OggDemuxer>();
    
    // Use output device sample rate for decoding to avoid resampling artifacts
    // OGG files are typically 16kHz but output is now 24kHz - Opus decoder handles rate conversion natively
    int target_sample_rate = codec_->output_sample_rate();
    
    demuxer->OnDemuxerFinished([this, target_sample_rate](const uint8_t* data, int ogg_sample_rate, size_t size){
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = target_sample_rate;
        packet->frame_duration = 60;
        packet->codec = 1;
        packet->payload.resize(size);
        std::memcpy(packet->payload.data(), data, size);
        PushPacketToDecodeQueue(std::move(packet), true);
    });
    demuxer->Reset();
    demuxer->Process(buf, size);
}

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_encode_queue_.empty() && audio_decode_queue_.empty() && audio_playback_queue_.empty();
}

void AudioService::WaitForPlaybackQueueEmpty() {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    audio_queue_cv_.wait(lock, [this]() { 
        return service_stopped_ || (audio_decode_queue_.empty() && audio_playback_queue_.empty()); 
    });
}

void AudioService::ResetDecoder() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_reset(opus_decoder_);
    }
    decoder_lock.unlock();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_queue_cv_.notify_all();
}

void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        codec_->EnableInput(false);
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        // Keep TX clock when duplex RX is active; otherwise RX may stall on some boards.
        if (!(codec_->duplex() && codec_->input_enabled())) {
            codec_->EnableOutput(false);
        }
    }
}

// ★ 音频电源巡检任务（原 esp_timer 回调改造，缘由见 Start() 注释）：
//   1s 周期检查录音/放音通道空闲超时并关闭；service_stopped_ 期间不做任何事。
void AudioService::AudioPowerTask() {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(AUDIO_POWER_CHECK_INTERVAL_MS));
        if (service_stopped_) {
            continue;
        }
        CheckAndUpdateAudioPowerState();
    }
}

void AudioService::SetModelsList(srmodel_list_t* models_list) {
    if (models_list_ == models_list) {
        return;
    }

    models_list_ = models_list;
    ESP_LOGI(TAG, "SetModelsList: models_list=%p", (void*)models_list_);

#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_USE_CUSTOM_WAKE_WORD
    // ★ USE_CUSTOM_WAKE_WORD 总门控（2026-09-16 卡滞感根修）：关闭时不初始化
    //   CustomWakeWord 检测任务（唤醒走服务端 anyWordWakeup），模型也不加载
    //   （assets.cc 源头门控），双保险。
    auto* mn_model = esp_srmodel_filter(models_list_, ESP_MN_PREFIX, NULL);
    ESP_LOGI(TAG, "SetModelsList: esp_srmodel_filter(MN)=%p", (void*)mn_model);
    if (mn_model != nullptr) {
        auto* cw = dynamic_cast<CustomWakeWord*>(wake_word_.get());
        if (cw == nullptr) {
            auto custom_wake_word = std::make_unique<CustomWakeWord>();
            if (custom_wake_word->Initialize(codec_, models_list_)) {
                cw = custom_wake_word.get();
                wake_word_ = std::move(custom_wake_word);
                wake_word_initialized_ = true;
                ESP_LOGI(TAG, "SetModelsList: CustomWakeWord initialized OK");
                if (!local_wake_words_.empty()) {
                    cw->SetWakeWords(local_wake_words_);
                }
            } else {
                ESP_LOGE(TAG, "SetModelsList: CustomWakeWord::Initialize FAILED");
            }
        }
    } else {
        ESP_LOGW(TAG, "SetModelsList: no MN model in srmodels, wake_word_=nullptr");
        wake_word_ = nullptr;
    }

    ESP_LOGI(TAG, "SetModelsList: done, wake_word_=%p", (void*)wake_word_.get());

    if (wake_word_) {
        wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            if (callbacks_.on_wake_word_detected) {
                callbacks_.on_wake_word_detected(wake_word);
            }
        });
    }
#else
    wake_word_ = nullptr;
#endif
}

void AudioService::SetLocalWakeWords(const std::vector<std::pair<std::string, std::string>>& wake_words) {
    local_wake_words_ = wake_words;
    auto* cw = dynamic_cast<CustomWakeWord*>(wake_word_.get());
    if (cw) {
        cw->SetWakeWords(local_wake_words_);
    }
}
