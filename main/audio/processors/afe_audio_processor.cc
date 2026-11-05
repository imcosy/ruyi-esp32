#include "afe_audio_processor.h"
#include <esp_log.h>
#include <math.h>

#define PROCESSOR_RUNNING 0x01

#define TAG "AfeAudioProcessor"

AfeAudioProcessor::AfeAudioProcessor()
    : afe_data_(nullptr) {
    event_group_ = xEventGroupCreate();
}

void AfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;

    // Pre-allocate output buffer capacity
    output_buffer_.reserve(frame_samples_);

    int ref_num = codec_->input_reference() ? 1 : 0;

    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }

    srmodel_list_t *models;
    if (models_list == nullptr) {
        models = esp_srmodel_init("model");
    } else {
        models = models_list;
    }

    char* ns_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
    char* vad_model_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
    
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    // ★ VAD_MODE_1（2026-09-15 晚 华哥实测"超过1米就听不见"根修）：设备端说完判定/
    //   端点检测已于 2026-09-10 全部移交服务端 Silero VAD（HandleVadChangeEvent 空实现，
    //   设备持续上行），固件 VAD 现在唯一作用 = AFE 内部 AGC 抬音联动（人声帧才升增益）。
    //   旧 MODE_3（2026-09-07 为固件端静音判定调紧）对远场低电平人声判"非人声" →
    //   AGC 增益永不生效 → 上行保持原始低电平（实测 1 米 RMS 仅 300+）→ 服务端
    //   识别碎裂/听不见。放宽到 MODE_1 让远场人声触发抬音；环境音误抬由服务端
    //   Silero（特征级判定，不依赖电平）兜住，不再有旧"静音判定拖长"问题。
    afe_config->vad_mode = VAD_MODE_1;
    afe_config->vad_min_noise_ms = 100;
    if (vad_model_name != nullptr) {
        afe_config->vad_model_name = vad_model_name;
    }

    // ★ 2026-09-19 华哥根修：**启用 NSNet2 神经网络降噪**（原为弃用）。
    //   背景：华哥实测 sisi 板录音"呼呼"噪音大，且回忆去年开发时"加了本地降噪
    //   录音才清晰"（该代码已丢失）。实测证据（本日 anan vs sisi 32 段频谱）：
    //   sisi 低频 150-600Hz 底噪比 anan 高 +3~6dB、语音 2k-5k 低 -6.7~-7.4dB
    //   → 典型"噪声主导 + 高频被淹没"；开 NS 是源头治理（不是调增益掩盖）。
    //   历史弃用理由（2026-08 ①DNN 推理抢 CPU ②压制低电平人声）针对的是旧实现，
    //   现用 NSNet2（VAD 联动、对语音保护更好）且 sisi 固件已换 CONFIG_SR_NSN_NSNET2。
    // ★ 2026-09-20 修正为**按板型实际能力启用**（华哥"三个板子固件检查"）：
    //   ns_init 原为无条件 true，但 ns_model_name 只在 sdkconfig 选了
    //   CONFIG_SR_NSN_NSNET2 时才会被 esp_srmodel_filter 命中（模型才被打进 assets）。
    //   anan/meeting 用的是 CONFIG_SR_NSN_WEBRTC、不带 NSNet2 模型 → 若照原样重编，
    //   它们会启用**WebRTC NS**（正是 2026-08 因"抢 CPU / 压制低电平人声"被弃用的实现），
    //   把 anan 目前清晰的录音改坏。故改为"有神经降噪模型才开"：
    //     sisi（有 nsnet2）→ 开 NS；anan/meeting（无模型）→ 保持原状不开。
    afe_config->ns_init = (ns_model_name != nullptr);
    if (ns_model_name != nullptr) {
        afe_config->ns_model_name = ns_model_name;
    }

    // ★ AGC 自动增益（2026-09-07）：只在人声段抬音，静音段不抬——信噪比友好，
    //   替代固定软件增益的"信号噪声一起抬"。配合 sisi 板软件增益回调默认 2.5，
    //   AGC 把人声抬到 ASR 友好电平（上行 RMS 900+），环境音维持低位不触发 VAD。
    afe_config->agc_init = true;
    // ★ 2026-09-19 华哥根修：AGC 压缩增益 21 → **9dB（库默认值）**。
    //   华哥质疑："为什么这次会把 AGC 增益调这么高"——21dB 是 2026-09-16 为治
    //   "远场听不见"而抬的上限，当时**没有降噪**：AGC 抬人声的同时把环境噪声一起
    //   抬起来（实测上行底噪被推到 -28dBFS，"呼呼"声根源之一）。
    //   现在 NS（NSNet2）在 AGC 之前压制稳态噪声，人声本身的增益需求大幅下降，
    //   回到默认 9dB 即可；继续用 21dB 只会让残余噪声随人声一起被放大。
    //   （agc_mode 仍显式置 WEBRTC：0039 起本地 wakenet 已退役，默认 WAKENET 模式
    //     无模型时增益恒为零、AGC 形同虚设。）
    afe_config->agc_mode = AFE_AGC_MODE_WEBRTC;
    afe_config->agc_compression_gain_db = 9;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

#if CONFIG_MEETING_BOARD && CONFIG_USE_DEVICE_AEC
    // ★ meeting板（2026-09-14）：恢复硬件AEC。meeting板=ESP-BOX硬件架构，
    //   ES8311喇叭输出回采到ES7210 MIC3（TDM槽1），参考信号真实存在（RX-STAT/AEC_DEBUG
    //   可验证R_energy>0），AEC正常收敛。TTS期间上行=干净人声 → 服务端声纹打断可用。
    afe_config->aec_init = true;
#else
    // ★ sisi/chatbox 无参考通道（2026-08-18 sisi实测教训）：AEC自适应滤波器无参考会
    //   学习发散，TTS结束后残留滤波器状态把真人语音当回声消除。TTS回采防护改走：
    //   TTS期间关闭上行（app.cc）+ 服务端声纹判定（s2s_service.py）。
    afe_config->aec_init = false;
#endif
    // ★ 必须启用 vad_init：sisi_esp 的 VAD 静音判定逻辑（app.cc HandleVadChangeEvent）
    // 完全依赖 AFE VAD 的 on_vad_change 回调。vad_init=false 会导致 VAD 永不触发，
    // vad_has_spoken_ 永远为 false → 永远进不到静音判定 → "根本无法判断用户说完话"。
    // 之前误以为"VAD干扰AEC导致过度抑制"，实际 AEC 过度抑制的根因是参考信号未注入
    // （参考通道注入链路在 input_channels<2 时不生效），与 VAD 无关。
    afe_config->vad_init = true;

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);
    
    // ★ 2026-09-20 撤销 0073 的"提权绑核"改动（华哥拍板）：那是无实证的推测性
    //   加固，且本文件三板共用——FEED 满刷屏的实锤根因是 OTA 与会话并发（下载
    //   擦写禁 cache 冻结 PSRAM、检查 TLS 挤爆内部 SRAM），OTA 已全量避让
    //   （app.cc 0074），音频任务布局回归原生 xiaozhi 证明过的形态，一处未动。
    xTaskCreate([](void* arg) {
        auto this_ = (AfeAudioProcessor*)arg;
        this_->AudioProcessorTask();
        vTaskDelete(NULL);
    }, "audio_communication", 8192, this, 6, NULL);
}

AfeAudioProcessor::~AfeAudioProcessor() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    vEventGroupDelete(event_group_);
}

size_t AfeAudioProcessor::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (afe_data_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    if (!IsRunning()) {
        return;
    }

#ifdef CONFIG_USE_DEVICE_AEC
    static int debug_counter = 0;
    int channels = codec_->input_channels();
    if (channels >= 2 && ++debug_counter % 500 == 0) {
        int64_t mic_energy = 0, ref_energy = 0;
        int32_t mic_max = 0;
        size_t frames = data.size() / channels;
        for (size_t i = 0; i < frames; i++) {
            int32_t m = abs(data[i * channels]);
            mic_energy += m;
            if (m > mic_max) mic_max = m;
            ref_energy += abs(data[i * channels + 1]);
        }
        ESP_LOGI(TAG, "AEC_DEBUG: frames=%d M_energy=%d M_max=%d R_energy=%d ratio=%.2f",
                 (int)frames, (int)(mic_energy / frames), mic_max,
                 (int)(ref_energy / frames),
                 ref_energy > 0 ? (float)mic_energy / ref_energy : -1);
    }
#endif

    input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    size_t chunk_size = afe_iface_->get_feed_chunksize(afe_data_) * codec_->input_channels();

    while (input_buffer_.size() >= chunk_size) {
        afe_iface_->feed(afe_data_, input_buffer_.data());
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunk_size);

        size_t buf_size = afe_iface_->get_feed_chunksize(afe_data_) * 4;
        if (input_buffer_.size() > buf_size) {
            ESP_LOGW(TAG, "AFE input buffer overflow: %d > %d, dropping old data",
                     (int)input_buffer_.size(), (int)buf_size);
            size_t drop = input_buffer_.size() - buf_size;
            input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + drop);
        }
    }
}

void AfeAudioProcessor::Start() {
    xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
}

void AfeAudioProcessor::Stop() {
    xEventGroupClearBits(event_group_, PROCESSOR_RUNNING);

    // ★ 2026-09-18 anan 打断崩溃根修：绝不在调用者上下文调 afe_iface_->reset_buffer()
    //   ——此刻 fetch 任务仍阻塞在 fetch_with_delay() 内部，跨上下文重置 AFE 内部
    //   FEED ringbuffer 会让读写指针错位 → "AFE: Ringbuffer of AFE(FEED) is full"
    //   永久刷屏、fetch 永不返回（表面在聆听，实际已崩溃）。统一置标志，由 fetch
    //   任务在自身上下文串行执行（与 feed() 互斥），这是本文件唯一安全的复位路径。
    reset_pending_.store(true);
    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    input_buffer_.clear();
}

bool AfeAudioProcessor::IsRunning() {
    return xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING;
}

void AfeAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

void AfeAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

void AfeAudioProcessor::AudioProcessorTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio communication task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING, pdFALSE, pdTRUE, portMAX_DELAY);

        // ★ 打断后缓冲重置（2026-09-16 WDT 崩溃根修）：在 fetch 任务上下文执行，
        //   与 AFE 内部零竞态。打断瞬间 TTS 回采骤停 → AEC 远端参考突变失配 →
        //   滤波器病态计算（CPU1 占死）+ FEED ringbuffer 溢出刷屏。
        // ★ 2026-09-18 anan 打断崩溃根修：整块必须持 input_buffer_mutex_——Feed()
        //   正是在持该锁时调 feed() 的，唯有同锁才能保证 feed() 与 reset_buffer()
        //   永不并发（否则 FEED ringbuffer 读写指针错位 → "Ringbuffer of
        //   AFE(FEED) is full" 永久刷屏、fetch 永不返回）。is_speaking_/
        //   output_buffer_ 只归本任务所有，在此清理天然无竞态。
        if (reset_pending_.exchange(false)) {
            std::lock_guard<std::mutex> lock(input_buffer_mutex_);
            input_buffer_.clear();
            output_buffer_.clear();
            is_speaking_ = false;
            afe_iface_->reset_buffer(afe_data_);
            // ★ 不清底噪基线（2026-09-16 卡滞感根修）：打断后环境噪声并未改变，
            //   清零会导致基线重新自适应 → 端点检测抖动 → 对话卡滞感。
            //   只清缓冲对齐参考信号，基线沿用打断前的稳定值。
            ESP_LOGW(TAG, "AFE 缓冲已重置（打断后参考信号重新对齐，基线保留）");
        }

        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if ((xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) == 0) {
            continue;
        }
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            if (res != nullptr) {
                ESP_LOGI(TAG, "Error code: %d", res->ret_value);
            }
            // ★ fetch 失败必须退避（2026-09-16 WDT 崩溃实锤）：AFE 内部状态异常时
            //   fetch_with_delay 立即返回空，原 continue 零间隔重试 = CPU1 满载自旋
            //   → IDLE1 饿死 → task_wdt 触发系统崩。退避让出 CPU 等 AFE 状态恢复。
            // 限频计数：每 10 次失败打一条，用于诊断失败频率（频繁失败=上行卡滞信号）
            static uint32_t fetch_fail_cnt = 0;
            if (++fetch_fail_cnt % 10 == 1) {
                ESP_LOGW(TAG, "AFE fetch 失败累计: %lu（每次退避20ms，频繁出现需查AFE状态）",
                         (unsigned long)fetch_fail_cnt);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // VAD state change
        if (vad_state_change_callback_) {
            if (res->vad_state == VAD_SPEECH && !is_speaking_) {
                is_speaking_ = true;
                vad_state_change_callback_(true);
            } else if (res->vad_state == VAD_SILENCE && is_speaking_) {
                is_speaking_ = false;
                vad_state_change_callback_(false);
            }
        }

        // ★ 近场能量（RMS）+ 自适应底噪基线：能量端点检测数据源。
        //   【为什么必须相对底噪】AGC 会把持续环境音放大逼近人声电平（FETCH_DEBUG
        //   实证 avg=959），任何固定绝对阈值都会在某个场景失效。语音的物理特征是
        //   "能量显著高于当前底噪"，基线跟踪三规则：
        //   ① rms < baseline：安静期快速下探收敛（25%/帧，~300ms 贴住底噪）
        //   ② baseline ≤ rms < 2×baseline：环境音缓变区，微上浮跟随（环境变吵能跟上）
        //   ③ rms ≥ 2×baseline：疑似语音，基线冻结（人声绝不抬底噪）
        {
            size_t rms_samples = res->data_size / sizeof(int16_t);
            if (rms_samples > 0) {
                int64_t sq_sum = 0;
                for (size_t i = 0; i < rms_samples; i++) {
                    int32_t v = res->data[i];
                    sq_sum += (int64_t)v * v;
                }
                int rms = (int)sqrtf((float)(sq_sum / rms_samples));
                latest_rms_.store(rms);

                int baseline = noise_baseline_.load();
                if (baseline == 0) {
                    noise_baseline_.store(rms);  // 首帧初始化
                } else if (rms < baseline) {
                    noise_baseline_.store((baseline * 3 + rms) / 4);  // 安静期快速下探
                } else if (rms < baseline * 2) {
                    noise_baseline_.store(baseline + (rms - baseline) / 128);  // 缓变区微上浮
                }
                // rms ≥ 2×baseline：疑似语音，基线冻结不更新
            }
        }

        if (output_callback_) {
            size_t samples = res->data_size / sizeof(int16_t);
            
            // Add data to buffer
            output_buffer_.insert(output_buffer_.end(), res->data, res->data + samples);
            
            // Output complete frames when buffer has enough data
            while (output_buffer_.size() >= frame_samples_) {
                if (output_buffer_.size() == frame_samples_) {
                    // If buffer size equals frame size, move the entire buffer
                    output_callback_(std::move(output_buffer_));
                    output_buffer_.clear();
                    output_buffer_.reserve(frame_samples_);
                } else {
                    // If buffer size exceeds frame size, copy one frame and remove it
                    output_callback_(std::vector<int16_t>(output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
                    output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
                }
            }
        }
    }
}

void AfeAudioProcessor::EnableDeviceAec(bool enable) {
    if (enable) {
#if CONFIG_USE_DEVICE_AEC
        afe_iface_->disable_vad(afe_data_);
        afe_iface_->enable_aec(afe_data_);
#else
        ESP_LOGE(TAG, "Device AEC is not supported");
#endif
    } else {
        afe_iface_->disable_aec(afe_data_);
        afe_iface_->enable_vad(afe_data_);
    }
}

void AfeAudioProcessor::ResetVadState() {
    // ★ 2026-09-18 anan 打断崩溃根修：只置标志，一切清理交 fetch 任务在自身上下文做
    //   （见 AudioProcessorTask 的 reset_pending_ 块）。原实现在调用者上下文直接调
    //   reset_buffer() + 清 output_buffer_——前者与 fetch_with_delay 竞态损坏 AFE 内部
    //   ringbuffer，后者根本无锁（output_buffer_ 只属于 fetch 任务）。
    reset_pending_.store(true);
}
