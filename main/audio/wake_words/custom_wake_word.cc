#include "custom_wake_word.h"
#include "audio_service.h"

#include <cmath>
#include <esp_log.h>
#include <esp_mn_iface.h>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>

#define TAG "CustomWakeWord"

CustomWakeWord::CustomWakeWord() {
    event_group_ = xEventGroupCreate();
}

CustomWakeWord::~CustomWakeWord() {
    if (multinet_model_data_ != nullptr && multinet_ != nullptr) {
        multinet_->destroy(multinet_model_data_);
        multinet_model_data_ = nullptr;
    }

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
}

bool CustomWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    codec_ = codec;
    commands_.clear();

    if (models_list == nullptr) {
        language_ = "cn";
        models_ = esp_srmodel_init("model");
    } else {
        models_ = models_list;
        language_ = "cn";
    }

    if (models_ == nullptr || models_->num == -1) {
        ESP_LOGE(TAG, "初始化 wakenet 模型失败");
        return false;
    }

    mn_name_ = esp_srmodel_filter(models_, ESP_MN_PREFIX, language_.c_str());
    if (mn_name_ == nullptr) {
        mn_name_ = esp_srmodel_filter(models_, ESP_MN_PREFIX, NULL);
    }
    if (mn_name_ == nullptr) {
        ESP_LOGE(TAG, "初始化 multinet 失败，mn_name 为空指针");
        return false;
    }

    multinet_ = esp_mn_handle_from_name(mn_name_);
    multinet_model_data_ = multinet_->create(mn_name_, duration_);
    multinet_->set_det_threshold(multinet_model_data_, threshold_);
    esp_mn_commands_clear();

    int chunksize = multinet_->get_samp_chunksize(multinet_model_data_);
    chunksize_ = chunksize;
    ESP_LOGI(TAG, "CustomWakeWord initialized, chunksize=%d, channels=%d", chunksize, codec_->input_channels());

    // ★ 唤醒词硬编码到固件：Initialize 阶段立即写入命令词表，
    //   不依赖外部 SetWakeWords/SetLocalWakeWords 调用（S2S 协议下服务端不再下发）。
    //   SetWakeWords 内部会忽略入参，使用硬编码的"你好如意"等唤醒词。
    SetWakeWords({});

    xTaskCreate([](void* arg) {
        auto this_ = (CustomWakeWord*)arg;
        this_->AudioDetectionTask();
        vTaskDelete(NULL);
        // ★ 优先级 3→5（2026-09-14）：唤醒是关键路径，此前优先级低于 opus_codec(5)，
        //   与显示/网络任务竞争时 detect 消费速率掉到实时率以下 → input_buffer_ 撞
        //   上限 → 音频空洞，MultiNet 必然失灵。
    }, "mn_detection", 8192, this, 5, nullptr);

    return true;
}

void CustomWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

bool CustomWakeWord::SetWakeWords(const std::vector<std::pair<std::string, std::string>>& wake_words) {
    commands_.clear();
    esp_mn_commands_clear();

    // ★ ESP32端唤醒词彻底硬编码到固件，不依赖也不接受服务端下发
    // 入参 wake_words 一律忽略，避免服务端 local_detect_words 覆盖导致唤醒失效
    (void)wake_words;
    std::vector<std::pair<std::string, std::string>> final_wake_words;
    final_wake_words.push_back({"ni hao ru yi", "你好如意"});
    final_wake_words.push_back({"ru yi ni hao", "如意你好"});
    final_wake_words.push_back({"zai gan ma", "在干嘛"});
    final_wake_words.push_back({"zai bu zai", "在不在"});
    final_wake_words.push_back({"ru yi ru yi", "如意如意"});

    for (const auto& word : final_wake_words) {
        commands_.push_back({word.first, word.second, "wake"});
    }

    // ★ 会议唤醒词（action="meeting"）：设备端直达会议模式，
    //   SendWakeupMessage 检测到 meeting action 后发 text="会议开始"，
    //   服务端 s2s_service._handle_chat_round 命中 s2s_meeting.START_WORDS 拦截进会议
    std::vector<std::pair<std::string, std::string>> meeting_wake_words;
    meeting_wake_words.push_back({"hui yi kai shi", "会议开始"});
    meeting_wake_words.push_back({"kai shi hui yi", "开始会议"});
    meeting_wake_words.push_back({"hui yi mo shi", "会议模式"});

    for (const auto& word : meeting_wake_words) {
        commands_.push_back({word.first, word.second, "meeting"});
    }

    // ★ 汇报唤醒词（action="report"，2026-09-16 汇报总结模式）：设备端直达汇报模式，
    //   SendWakeupMessage 检测到 report action 后发 text="开始汇报"，
    //   服务端命中 s2s_meeting.REPORT_START_WORDS 直达一对一工作汇报总结场景
    std::vector<std::pair<std::string, std::string>> report_wake_words;
    report_wake_words.push_back({"kai shi hui bao", "开始汇报"});
    report_wake_words.push_back({"hui bao kai shi", "汇报开始"});
    report_wake_words.push_back({"hui bao mo shi", "汇报模式"});

    for (const auto& word : report_wake_words) {
        commands_.push_back({word.first, word.second, "report"});
    }

    for (int i = 0; i < commands_.size(); i++) {
        esp_mn_commands_add(i + 1, commands_[i].command.c_str());
    }
    esp_mn_commands_update();

    ESP_LOGI(TAG, "唤醒词已设置，共 %d 个", commands_.size());
    for (const auto& cmd : commands_) {
        ESP_LOGI(TAG, "  - %s (%s)", cmd.command.c_str(), cmd.text.c_str());
    }

    return true;
}

void CustomWakeWord::Start() {
    xEventGroupSetBits(event_group_, DETECTION_RUNNING_EVENT);
}

void CustomWakeWord::Stop() {
    xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    input_buffer_.clear();
}

void CustomWakeWord::Feed(const std::vector<int16_t>& data) {
    if (multinet_model_data_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    if (!(xEventGroupGetBits(event_group_) & DETECTION_RUNNING_EVENT)) {
        return;
    }

    // ★ 背压改"丢旧保新"（2026-09-14）：此前超限丢新数据 → 最新语音（唤醒词尾部）
    //   进不来，MultiNet 永远听到的是越来越旧的音频；改为从头部丢弃最旧数据，
    //   保证 input_buffer_ 尾部始终是连续的最新语音。
    const size_t MAX_BUF = chunksize_ > 0 ? (size_t)chunksize_ * 5 : 2560;
    if (input_buffer_.size() > MAX_BUF) {
        size_t drop = input_buffer_.size() - MAX_BUF;
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + drop);
        drop_count_ += drop;
    }

    // ★ RMS 统计（诊断电平：远场语音应 >300，静音 <100）——单/双声道都要算：
    //   2026-09-18 根修：原实现只在 input_channels()==2 时统计，anan（单麦
    //   NoAudioCodecSimplex，input_channels=1）永远走 else 分支 → chunkRMSmax 恒 0，
    //   麦克风是否活着这条最关键的排障信号在 anan 上完全失效。
    if (codec_->input_channels() == 2) {
        // ★ 取 mic 通道（TDM槽0=MIC1），丢弃回采参考（槽1=MIC3）——MultiNet 单通道模型。
        //   （2026-09-14 撤销 ×3 数字增益：底噪同步放大喂 MultiNet 是误检源，
        //   回原生直通电平，灵敏度问题交由 PGA/AGC 域解决）
        int64_t sum_sq = 0;
        for (size_t i = 0; i < data.size(); i += 2) {
            int32_t s = data[i];
            sum_sq += (int64_t)s * s;
            input_buffer_.push_back(data[i]);
        }
        float rms = sqrtf((float)(sum_sq / (data.size() / 2)));
        if (rms > rms_max_) rms_max_ = rms;
    } else {
        int64_t sum_sq = 0;
        for (size_t i = 0; i < data.size(); i++) {
            int64_t s = data[i];
            sum_sq += s * s;
        }
        if (!data.empty()) {
            float rms = sqrtf((float)(sum_sq / data.size()));
            if (rms > rms_max_) rms_max_ = rms;
        }
        input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    }

    feed_count_++;
    feed_samples_ += data.size() / 2;
    if (feed_count_ % 500 == 0) {
        // 每 500 次 feed（约 5 秒）打印唤醒链路诊断：喂入速率应≈100/s，
        // detect 耗时反映 MultiNet 实时率（>32000us 即消费不过来）
        // ★ 2026-09-18 根修：原格式串用 %llu，而 CONFIG_NEWLIB_NANO_FORMAT=y
        //   （nano 格式化不支持 64 位整型格式）→ "%llu" 中多出的 l/u 被当字面量输出
        //   （日志里的 "feed=lu次"），且 8 字节参数按 4 字节取用 → 后续参数全体错位
        //   （水位显示 1070493076 这类垃圾值、chunkRMSmax 读数不可信）。
        //   计数器已统一为 uint32_t（见头文件），%lu/%u 在 nano 格式化下安全。
        ESP_LOGI(TAG, "[诊断] feed=%lu次/样本=%lu 丢弃样本=%lu 水位=%u/%u detect=%lu次 耗时max=%luus chunkRMSmax=%.0f",
                 (unsigned long)feed_count_, (unsigned long)feed_samples_,
                 (unsigned long)drop_count_, (unsigned)input_buffer_.size(),
                 (unsigned)(chunksize_ * 5), (unsigned long)detect_count_,
                 (unsigned long)detect_us_max_, rms_max_);
        detect_us_max_ = 0;
        rms_max_ = 0;
    }
}

void CustomWakeWord::AudioDetectionTask() {
    int chunksize = chunksize_ > 0 ? chunksize_ : multinet_->get_samp_chunksize(multinet_model_data_);
    ESP_LOGI(TAG, "Audio detection task started, chunksize=%d", chunksize);

    while (true) {
        xEventGroupWaitBits(event_group_, DETECTION_RUNNING_EVENT, pdFALSE, pdTRUE, portMAX_DELAY);

        std::vector<int16_t> chunk;
        {
            std::lock_guard<std::mutex> lock(input_buffer_mutex_);
            if (input_buffer_.size() < chunksize) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            chunk.assign(input_buffer_.begin(), input_buffer_.begin() + chunksize);
            input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunksize);
            // erase 后 capacity 不缩，定期回收防止 SRAM 慢慢泄漏
            if (input_buffer_.empty()) {
                input_buffer_.shrink_to_fit();
            }
        }

        // ★ detect 耗时统计：MultiNet7 单帧推理若 >32000us（实时率>1）即消费不过来
        int64_t t0 = esp_timer_get_time();
        esp_mn_state_t mn_state = multinet_->detect(multinet_model_data_, chunk.data());
        int64_t dt = esp_timer_get_time() - t0;
        detect_count_++;
        if (dt > detect_us_max_) detect_us_max_ = dt;

        if (mn_state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *mn_result = multinet_->get_results(multinet_model_data_);
            for (int i = 0; i < mn_result->num; i++) {
                if (mn_result->command_id[i] >= 1 && mn_result->command_id[i] <= (int)commands_.size()) {
                    auto& command = commands_[mn_result->command_id[i] - 1];
                    last_detected_wake_word_ = command.text;
                    last_detected_action_ = command.action;  // "wake" / "meeting"

                    xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);
                    {
                        std::lock_guard<std::mutex> lock(input_buffer_mutex_);
                        input_buffer_.clear();
                    }

                    ESP_LOGI(TAG, "唤醒词检测到: %s", last_detected_wake_word_.c_str());

                    if (wake_word_detected_callback_) {
                        wake_word_detected_callback_(last_detected_wake_word_);
                    }
                    break;
                }
            }
            multinet_->clean(multinet_model_data_);
        } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
            multinet_->clean(multinet_model_data_);
        }
    }
}

size_t CustomWakeWord::GetFeedSize() {
    if (chunksize_ > 0) {
        return chunksize_;
    }
    if (multinet_model_data_ == nullptr) {
        return 0;
    }
    return multinet_->get_samp_chunksize(multinet_model_data_);
}