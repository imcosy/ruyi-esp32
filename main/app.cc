#include "app.h"
#include "board.h"
#include "display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "protocol.h"
#include "system_info.h"
#include "audio_codec.h"
#include "websocket.h"
#include "audio/assets/lang_config.h"
#include "tool.h"
#include <esp_app_desc.h>
#include <esp_lvgl_port.h>
#include "assets.h"
#include "settings.h"

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "App"


App::App() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            App* app = (App*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

App::~App() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool App::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void App::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // ★ 2026-09-16 meeting板OTA开机崩溃根修（0048 LoadProhibited）：audio 初始化
    //   必须先于 assets.Apply——0047 起 assets.Apply 前移到 SetupUI 之前，其内部
    //   srmodels 加载会同步走 SetModelsList → CustomWakeWord::Initialize（要用
    //   codec）；客户端唤醒（USE_CUSTOM_WAKE_WORD=y）的板型在此处 codec_ 尚未
    //   赋值，codec_->input_channels() 空指针解引用，开机即崩循环。audio 服务
    //   不依赖 display/assets，置于最前无副作用。
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    // Setup the display
    auto display = board.GetDisplay();
    // ★ 2026-09-16 屏幕文字不全根修（anan/stick 等所有用 assets 全字库的板）：
    //   SetupUI 会把主题字体固化到 screen 样式（气泡/控件全部继承），必须先把
    //   assets 分区中的全字库 cbin 换进主题对象再 SetupUI。原先放在
    //   ActivationTask 里的 Apply(false) 只换主题对象，救不回已固化的 screen
    //   字体，导致屏幕只能显示内置 C 数组小字库（puhui-basic 仅 206 汉字）。
    auto& assets = Assets::GetInstance();
    if (assets.partition_valid()) {
        assets.Apply(false);
    }
    display->SetupUI();
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        previous_state_ = old_state;
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    // ★ 200ms tick（2026-09-09）：原 1s tick 导致 Speaking→Listening 切换检测平均
    //   延迟 0.5s（TTS 播完要多等半秒才恢复聆听）；改 200ms 后切换检测零延迟，
    //   原有按 1s 节奏的逻辑（心跳/超时计数）用 clock_ticks_ % 5 门控保持不变
    esp_timer_start_periodic(clock_timer_handle_, 200000);

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();

        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                break;
            case NetworkEvent::WifiConfigModeExit:
                break;
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();
    display->UpdateStatusBar(true);
}

void App::Run() {
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            // S2S 流式上传：Opus 帧追加到流式缓冲（每帧前 2 字节小端长度前缀）
            // 达阈值立即流式发送（边说边传）；VAD 静音判定后由 FlushStreamAudio() 发剩余 + listen_stop
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                // codec==1 = Opus 裸帧；每帧前追加 2 字节小端长度前缀
                if (packet->codec == 1 && !packet->payload.empty()) {
                    size_t frame_len = packet->payload.size();
                    if (frame_len > 0xFFFF) {
                        // 单帧不应超过 64KB（20ms opus 实际 ~80-200B），异常跳过
                        continue;
                    }
                    // 写入长度前缀（小端）
                    stream_send_buffer_.push_back(static_cast<uint8_t>(frame_len & 0xFF));
                    stream_send_buffer_.push_back(static_cast<uint8_t>((frame_len >> 8) & 0xFF));
                    // 写入 opus 帧数据
                    stream_send_buffer_.insert(stream_send_buffer_.end(),
                                               packet->payload.begin(),
                                               packet->payload.end());
                    // 字节计数只在真正发送成功时累加（见下方发送分支）——保证
                    // listening_audio_bytes_ 语义="实际发出去的上行字节"。
                    // 达阈值立即流式发送，不等说完
                    if (stream_send_buffer_.size() >= kStreamChunkSize) {
                        // ★ 2026-09-18 根修（日志必须说真话）：原实现不检查发送结果——
                        //   TCP 半开/握手失败期间 SendS2SAudio 返回 false，日志却继续
                        //   打"发送音频"、字节计数照涨，排障时完全被误导（"日志在发、
                        //   服务端 0 帧"）。改为只在成功时计数，失败按次数限频告警。
                        bool sent = protocol_ && protocol_->SendS2SAudio(stream_send_buffer_);
                        if (sent) {
                            listening_audio_bytes_ += (int)stream_send_buffer_.size();
                        } else {
                            static uint32_t s_send_fail_cnt = 0;
                            if (++s_send_fail_cnt % 10 == 1) {
                                ESP_LOGW(TAG, "上行发送失败累计: %lu（连接已断/未就绪，本段已丢弃）",
                                         (unsigned long)s_send_fail_cnt);
                            }
                        }
                        stream_send_buffer_.clear();
                    }
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            HandleVadChangeEvent();
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
            CheckIdleTimeout();

            // ★ 2026-09-18：堆统计从 2 秒一次改为 30 秒一次（原频率直接把串口刷满，
            //   排障时反而看不到关键日志；30s 足够观察内存趋势）
            if (clock_ticks_ % 150 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void App::HandleNetworkConnectedEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        SetDeviceState(kDeviceStateConnecting);
        // ★ 2026-09-18：与重连路径共用 reconnect_mutex_ 串行化任务创建，
        //   杜绝"网络事件与断连回调同时创建任务"的双任务竞态。
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        if (activation_task_handle_ != nullptr) {
            return;
        }
        xTaskCreate([](void* arg) {
            App* app = static_cast<App*>(arg);
            app->ActivationTask();
            {
                std::lock_guard<std::mutex> lk(app->reconnect_mutex_);
                app->activation_task_handle_ = nullptr;
            }
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void App::HandleNetworkDisconnectedEvent() {
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        protocol_->CloseAudioChannel();
        // ★ PlaySound已移至OnAudioChannelClosed回调中(在SetDeviceState Idle之前执行)
        // 此处不再重复播放，避免断连时播两遍
    }

    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void App::CheckIdleTimeout() {
    auto state = GetDeviceState();
    // ★ 200ms tick 节奏（2026-09-09）：Speaking→Listening 切换、S2S 静音兜底每 tick
    //   （200ms）执行，TTS 播完零延迟恢复聆听；原 1 秒节奏的计数逻辑（心跳/超时）
    //   用 clock_ticks_ % 5 门控，tick 数值语义不变

    // ★ 唤醒待命 pending（2026-09-16 修复"无法切回 idle"）：idle_voice_pending_ /
    //   idle_wake_pending_ 在状态机 idle 分支按唤醒模式置位（那里同步调会死等输出
    //   任务）。到这一拍时 bye 的 TTS 尾巴必然已播完（bye→idle 本来就等 IsIdle+输出
    //   空闲才切换），安全开启。
    if (idle_voice_pending_) {
        idle_voice_pending_ = false;
        audio_service_.EnableVoiceProcessing(true);
        ESP_LOGI(TAG, ">>> idle 待命：VoiceProcessing 已开启（服务端唤醒上行中）");
    }
    if (idle_wake_pending_) {
        idle_wake_pending_ = false;
        // ★ 客户端唤醒待命：开本地 MultiNet 检测（processor 保持关闭，省 CPU，
        //   唤醒检测由 AudioInputTask 独立喂流，不依赖 AFE）
        audio_service_.EnableWakeWordDetection(true);
        ESP_LOGI(TAG, ">>> idle 待命：本地唤醒检测已开启（客户端唤醒模式，idle 不上行）");
    }

    // ★ 会议总结二维码倒计时（2026-09-14）：显示期间独立于设备状态，到时销毁回Idle
    if (qr_countdown_ticks_ > 0) {
        qr_countdown_ticks_--;
        if (qr_countdown_ticks_ == 0) {
            ESP_LOGI(TAG, "二维码显示时间到 → 隐藏并回Idle待机");
            Board::GetInstance().GetDisplay()->HideMeetingQrcode();
            SetDeviceState(kDeviceStateIdle);
        }
        return;   // 二维码显示期间不执行其他状态逻辑（心跳由服务端会议上下文已结束兜底）
    }

    // ★ pingpong 心跳全状态运行（2026-09-16 断线自愈根修）：
    //   原实现在 Idle 分支内、Listening/Speaking 分支每秒清零计数——心跳在对话态
    //   完全停摆；且超时只重置重发（死循环发心跳永不重连）。底层自实现 WebSocket
    //   无主动保活（web_socket.cc 无 WS ping，断开仅靠 TCP RST/FIN 或服务端 close
    //   frame），TCP 半开（NAT 表项丢失/路由静默断链）时设备永远发现不了断线——
    //   实测 sisi 板断线 7 分钟零重连，音频上行进黑洞无法唤醒（服务端日志零帧）。
    //   服务端 2026-09-16 起回显 pingpong，闭环成立；超时即强制断开+手动重连
    //   （主动 Close 无服务端 close 回复，on_disconnected_ 不触发，必须手动建重连任务）。
    if (clock_ticks_ % 5 == 0) {
        if (pingpong_pending_) {
            pingpong_wait_ticks_++;
            if (pingpong_wait_ticks_ >= PINGPONG_TIMEOUT_TICKS) {
                pingpong_pending_ = false;
                pingpong_wait_ticks_ = 0;
                heartbeat_ticks_ = 0;
                ESP_LOGW(TAG, "心跳超时无响应(%ds)，强制断开重连",
                         PINGPONG_TIMEOUT_TICKS);
                if (protocol_) {
                    protocol_->CloseAudioChannel(false);
                }
                if (reconnect_retry_count_ < MAX_RECONNECT_RETRIES &&
                    activation_task_handle_ == nullptr) {
                    StartReconnectTask("heartbeat_reconnect");
                }
            }
        } else {
            heartbeat_ticks_++;
            if (heartbeat_ticks_ >= HEARTBEAT_INTERVAL_TICKS) {
                heartbeat_ticks_ = 0;
                pingpong_pending_ = true;
                pingpong_wait_ticks_ = 0;
                SendHeartbeat();
            }
        }
    }

    if (state == kDeviceStateIdle) {
        if (clock_ticks_ % 5 == 0) {
            idle_clock_ticks_++;
        }
    } else if (state == kDeviceStateListening) {
        // ★ 2026-09-10 架构升级：设备端不再做任何 VAD 静音判定/说完判定！
        //   设备端 Listening 期间持续上行 Opus，服务端 Silero VAD 流式判定
        //   语音开始/结束（静音≥500ms 触发 ASR），无需客户端发 listen_stop。
        //   此处仅保留兜底超时：等待服务端响应超时 / 长时间无语音回 Idle。

        // ★ 2026-09-17 会议自动恢复期间不回Idle：会议是持续录音场景（长时无人
        //   发言也要保持录音转写），等待响应/空闲超时回Idle会漏录。
        //   直到 bye（会议结束）或断连（OnAudioChannelClosed）复位本标志。
        if (meeting_auto_active_) {
            idle_clock_ticks_ = 0;
            waiting_server_ticks_ = 0;
            return;
        }

        // ★ 等待服务端TTS响应超时：10秒无响应→回Idle（1秒节奏：%5门控）
        if (waiting_for_server_ && clock_ticks_ % 5 == 0) {
            waiting_server_ticks_++;
            if (waiting_server_ticks_ >= WAITING_SERVER_TIMEOUT_TICKS) {
                ESP_LOGW(TAG, "等待服务端TTS响应超时(%ds)→回Idle", WAITING_SERVER_TIMEOUT_TICKS);
                waiting_for_server_ = false;
                waiting_server_ticks_ = 0;
                waiting_interrupt_speech_start_us_ = 0;
                s2s_waiting_tts_greeting_ = false;
                // 音频开关交给 idle 分支统一管理（服务端唤醒待命：processing+sending 常开）
                ClearStreamAudio();
                SetDeviceState(kDeviceStateIdle);
                return;
            }
        }

        // ★ 客户端空闲超时：10秒无语音→回Idle（1秒节奏：%5门控）
        if (clock_ticks_ % 5 == 0) {
            idle_clock_ticks_++;
            if (idle_clock_ticks_ >= LISTENING_IDLE_TIMEOUT_TICKS) {
                ESP_LOGW(TAG, "聆听空闲超时(%ds)→回Idle", LISTENING_IDLE_TIMEOUT_TICKS);
                idle_clock_ticks_ = 0;
                waiting_for_server_ = false;
                waiting_server_ticks_ = 0;
                waiting_interrupt_speech_start_us_ = 0;
                s2s_waiting_tts_greeting_ = false;
                // 音频开关交给 idle 分支统一管理（服务端唤醒待命：processing+sending 常开）
                ClearStreamAudio();
                SetDeviceState(kDeviceStateIdle);
                return;
            }
        }
    } else if (state == kDeviceStateSpeaking) {
        // ★ tts_done后等播放队列排空再切Listening，避免清空队列丢弃未播音频
        // 且必须等 AudioOutputTask 真正写完最后一块（IsAudioOutputActive()==false），
        // 否则 IsIdle() 在最后一块阻塞写I2S期间就返回true，导致提前切Listening开麦，
        // 无AEC下TTS回声触发VAD→把回声当用户语音→"卡在聆听/听不见用户/无法连续对话"
        if (tts_done_received_ && audio_service_.IsIdle() && !audio_service_.IsAudioOutputActive()) {
            // ★ 2026-09-12：服务端已发 bye（会话结束，如会议总结播完）→ 回 Idle 待机，
            //   不再进对话聆听（修复：会议结束后默认进入对话模式的问题）
            // ★ 2026-09-14：bye 带 qrcode 时 → 显示会议总结二维码 meeting_qr_seconds_
            //   秒（CheckIdleTimeout 顶部倒计时统一销毁并回 Idle），期间触摸被拦截
            if (meeting_bye_received_) {
                meeting_bye_received_ = false;
                meeting_auto_active_ = false;   // 会议结束，恢复超时机制
                tts_done_received_ = false;
                if (protocol_ && protocol_->IsAudioChannelOpened()) {
                    protocol_->SendS2SJson("{\"type\":\"tts_playback_done\"}");
                }
                if (!meeting_qrcode_url_.empty()) {
                    ESP_LOGI(TAG, "TTS: 播放排空 + bye已收到 → 显示会议总结二维码 %ds 后回Idle",
                             meeting_qr_seconds_);
                    auto display = Board::GetInstance().GetDisplay();
                    display->ShowMeetingQrcode(meeting_qrcode_url_.c_str());
                    qr_countdown_ticks_ = meeting_qr_seconds_ * 5;  // 200ms/tick
                } else {
                    ESP_LOGI(TAG, "TTS: 播放排空 + bye已收到 → 回Idle待机");
                    SetDeviceState(kDeviceStateIdle);
                }
                return;
            }
            ESP_LOGI(TAG, "TTS: 播放队列已排空且最后一块已播完，切Listening");
            tts_done_received_ = false;
            // 2026-09-10：设备端 TTS 真正播完通知服务端（服务端刷新锚点+重置VAD，
            // 帮助丢弃 TTS 播完后缓冲期残留的回采语音段，双保险）
            if (protocol_ && protocol_->IsAudioChannelOpened()) {
                protocol_->SendS2SJson("{\"type\":\"tts_playback_done\"}");
            }
            SetDeviceState(kDeviceStateListening);
        }
    } else {
        idle_clock_ticks_ = 0;
        heartbeat_ticks_ = 0;
        pingpong_pending_ = false;
        pingpong_wait_ticks_ = 0;
    }
}

void App::SendHeartbeat() {
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->SendPingpong(esp_timer_get_time() / 1000000);
    }
}

void App::HandleActivationDoneEvent() {
    SystemInfo::PrintHeapStats();

    // 仅在预对话状态下才转入Idle（Connecting/WifiConfiguring/Starting）
    // 如果服务端已通过TTS接管状态机（Speaking/Listening），不能覆盖
    auto current_state = GetDeviceState();
    if (current_state == kDeviceStateConnecting ||
        current_state == kDeviceStateWifiConfiguring ||
        current_state == kDeviceStateStarting) {
        SetDeviceState(kDeviceStateIdle);
    }

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    display->SetChatMessage("system", "");

    // ★ 连接成功提示音已移至 OnAudioChannelOpened 回调（先于 SendS2SFirstFrame 入队），
    //   保证"提示音→服务端TTS播报"严格依次播放；此处不再重复播放
    if (reconnect_retry_count_ > 0) {
        ESP_LOGI(TAG, "Reconnect succeeded after %d attempt(s)", reconnect_retry_count_);
        reconnect_retry_count_ = 0;
    }

    ota_.reset();
    // ★ 不再切 LOW_POWER（2026-09-14 对话延迟根因修复）：S2S 常连架构下唤醒对话
    //   复用同一条 WebSocket，没有新的 OnAudioChannelOpened 事件把 WiFi 切回
    //   PERFORMANCE——启动末尾设 LOW_POWER(MAX_MODEM) 会导致整个对话期间 WiFi
    //   深度省电，下行 TTS 音频流被 Modem 缓冲拖慢（实测说完话 5-6 秒才出声）。
    //   WiFi 省电只保留断开场景（OnAudioChannelClosed → LOW_POWER）。
}

void App::ActivationTask() {
    ota_ = std::make_unique<Ota>();
    InitializeProtocol();

    // ★ 2026-09-25 OTA 回归开机最前时机（0075 实测 idle 路径死路根修）：
    //   idle 态常连 WS TLS + MultiNet 唤醒引擎把内部 SRAM 吃到 1-12KB，
    //   OTA 的第二条 TLS 必然分配失败（实测 header timeout + EspSsl 收包任务
    //   退出）→ OTA 在 idle 永远不可能成功，新版本永远送不到板上。
    //   开机此刻：WS 未连、会话未开始、MultiNet 未加载、SRAM 最富裕——
    //   检查+升级一气呵成（原版 xiaozhi 同款时序），全程与音频零并发。
    //   升级成功自动重启；检查失败/无新版本则继续正常连接。
    //   仅开机首次激活执行：重连路径（心跳超时/断线）不再查——重连要快，
    //   且新版本下下次开机自然拿到。
    if (!ota_boot_check_done_) {
        ota_boot_check_done_ = true;
        esp_err_t ota_chk = ota_->CheckVersion();
        if (ota_chk == ESP_OK && ota_->IsServerVersionNewer(ota_->GetFirmwareVersion())) {
            ESP_LOGI(TAG, "OTA 发现新版本 %s（当前 %s），开机升级后重启",
                     ota_->GetFirmwareVersion().c_str(),
                     ota_->GetCurrentVersion().c_str());
            UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion());
            return;   // 升级成功已自动重启；失败则继续正常启动（下次开机重试）
        }
    }

    Schedule([this]() {
        auto display = Board::GetInstance().GetDisplay();
        const auto* desc = esp_app_get_description();
        char version_buf[128];
        snprintf(version_buf, sizeof(version_buf), "%s v%s", Lang::Strings::VERSION, desc->version);
        display->ShowNotification(version_buf);
    });
    vTaskDelay(pdMS_TO_TICKS(200));

    // ★ 连接WebSocket：失败则无限重试，必须连接成功
    while (!protocol_->OpenAudioChannel()) {
        reconnect_retry_count_++;
        ESP_LOGW(TAG, "WebSocket连接失败(第%d次)，%dms后重试", reconnect_retry_count_, RECONNECT_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    std::string assets_version;
    std::string assets_url;
    // assets_info 在 S2S 协议下也由服务端推送，暂不处理
    (void)assets_version;
    (void)assets_url;
    // ★ ESP32端唤醒词硬编码为"你好如意"+"如意如意"(在CustomWakeWord::SetWakeWords默认值中)
    // 不再从服务端获取local_detect_words，也不再调用audio_service_.SetLocalWakeWords
    // CustomWakeWord初始化时会自动使用默认唤醒词

    // S2S 协议：上行 Opus 编码（裸流+2字节长度前缀），下行 PCM 直通
    audio_service_.SetPcmMode(false);

    // （assets 字库应用已前移至 Initialize 中 SetupUI 之前——见该处注释；
    //   此处仅保留服务端推送 assets 更新的通道，S2S 协议下 assets_version
    //   恒为空，暂不触发）

    if (!assets_version.empty() && !assets_url.empty()) {
        auto& assets = Assets::GetInstance();

        std::string current_assets_version;
        void* ptr = nullptr;
        size_t size = 0;
        if (assets.GetAssetData("index.json", ptr, size)) {
            cJSON* root = cJSON_ParseWithLength(static_cast<char*>(ptr), size);
            if (root != nullptr) {
                cJSON* version = cJSON_GetObjectItem(root, "version");
                if (cJSON_IsString(version)) {
                    current_assets_version = version->valuestring;
                }
                cJSON_Delete(root);
            }
        }

        if (current_assets_version != assets_version) {
            bool download_success = assets.Download(assets_url, [this](int progress, size_t speed) {
                ESP_LOGI(TAG, "资源下载进度：%d%%, 速度：%u B/s", progress, speed);
            });

            if (download_success) {
                if (assets.Apply(true)) {
                    ESP_LOGI(TAG, "资源应用成功");
                } else {
                    ESP_LOGE(TAG, "资源应用失败");
                }
            } else {
                ESP_LOGE(TAG, "资源下载失败");
            }
        } else {
            if (assets.Apply(false)) {
                ESP_LOGI(TAG, "资源应用成功（SR 模型已加载）");
            }
        }
    }

    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void App::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    // ★ 工具清理（2026-09-16 华哥拍板）：暂时只保留 set_volume。
    //   已移除：search_and_play_song（点歌）、RegisterDisplayTools（set_our_theme/
    //   SetTheme/SetBrightness）注册调用；set_press_to_talk 在 tool.cc 内移除。
    //   后续需要时再按需注册，避免大量工具撑爆 LLM 上下文（TOOL 路由失效）。
    ToolManager::GetInstance().RegisterBuiltinTools();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    {
        auto app_desc = esp_app_get_description();
        char info[128];
        snprintf(info, sizeof(info), "%s v%s | %s | RAM:%uKB",
            BOARD_NAME, app_desc->version,
            SystemInfo::GetChipModelName().c_str(),
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        display->SetChatMessage("system", info);
    }

    protocol_ = std::make_unique<WebsocketProtocol>();

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });

    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        pingpong_pending_ = false;
        pingpong_wait_ticks_ = 0;
        // ★ 用 tts_receiving_ 原子标志判断，不依赖异步Schedule切换的GetDeviceState()
        // tts_start 在WebSocket线程同步置位 tts_receiving_=true，确保音频包不被丢弃
        // 避免 Schedule 延迟导致 tts_start 后的音频包因 state!=Speaking 被丢弃→卡顿
        if (tts_receiving_ || music_playing_) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet), false);
        }
    });

    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        // ★ 连接成功提示音先入播放队列，再发首帧——服务端TTS播报必然排在提示音之后，
        //   保证"提示音→登录播报"依次播放（原实现在ActivationTask里调用，晚于首帧，
        //   TTS可能先入队导致提示音插在播报之后/中间）
        PlaySound(Lang::Sounds::OGG_SUCCESS);
        // S2S 协议：连接建立后立即发首帧配置（stream_upload=true，不带 noaudio/text）
        // 告知服务端进入流式上行模式，等用户说话时发 binary 音频帧
        // 唤醒时再发 noaudio=true + text 帧触发 LLM（服务端 _receive_streaming_round 识别）
        protocol_->SendS2SFirstFrame("");
        // ★ 设备能力上报（2026-09-15）：必须在首帧配置之后——服务端把连接后的第一帧
        //   JSON 当认证帧（api_key 等），hello 若抢先会认证失败。首帧之后发 hello
        //   （board 名 + AEC 能力），服务端按 has_aec 决定播报期打断监听是否启用
        //   （无 AEC 板播报期上行已关，服务端若仍收帧只能是异常回采，直接丢弃——
        //   防"回采→ASR幻听→AI自言自语"循环）
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            // ★ 2026-09-18⑨ 根修：board 字段必须用 BOARD_NAME（板型 sisi/anan/meeting），
            //   原用 board.GetBoardType() 返回的是"网络类型"（wifi/ml307），sisi 板上报
            //   成了 "wifi"（实测日志）→ 与服务端首帧 board 字段语义不一致。
            std::string hello = "{\"type\":\"hello\",\"board\":\"" +
                std::string(BOARD_NAME) + "\",\"has_aec\":" +
                (board.HasAecReference() ? "true" : "false") + "}";
            protocol_->SendS2SJson(hello);
            ESP_LOGI(TAG, ">>> hello 已上报: %s", hello.c_str());
        }
    });

    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        // ★ WebSocket线程同步复位TTS接收标志：断开后旧连接残留的TTS音频包不再入队，
        //   防止残留播报与新连接的提示音/播报交织（"播报中间插入提示音"的根因之一）
        tts_receiving_ = false;
        Schedule([this]() {
            // ★ 不调用 SetChatMessage("system","")：
            //   1. 紧接着 SetDeviceState(kDeviceStateIdle) 会触发 ClearChatMessages 清空所有消息（包括 system），此处删除冗余
            //   2. 先单独删除 system container 再 lv_obj_clean 会触发 LVGL 内部两次删除流程，
            //      invalidate/事件队列残留导致 lv_label_event → lv_area_diff use-after-free 崩溃
            // ★ 断开清场：连接已断，残留TTS播报作废，先清队列再播断开提示音，
            //   保证提示音从干净的队列开始、与服务端播报严格串行
            audio_service_.StopPlayback();
            // ★ 断连清场：会议自动恢复标志复位（重连后由服务端重新判定是否自动恢复）
            meeting_auto_active_ = false;
            // ★ 断连时立即播报提示音（必须在SetDeviceState之前，否则Idle状态被跳过）
            PlaySound(Lang::Sounds::OGG_EXCLAMATION);
            SetDeviceState(kDeviceStateIdle);

            if (protocol_ && !protocol_->IsAudioChannelOpened()) {
                StartReconnectTask("reconnect");
            }
        });
    });

    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        pingpong_pending_ = false;
        pingpong_wait_ticks_ = 0;
        auto type = cJSON_GetObjectItem(root, "type");
        if (!type || !cJSON_IsString(type)) return;

        const char* type_str = type->valuestring;

        // ========== TTS 开始 ==========
        if (strcmp(type_str, "tts_start") == 0) {
            // ★ 同步置位 tts_receiving_：WebSocket线程立即生效，确保后续音频包不被OnIncomingAudio丢弃
            // （Schedule是异步的，状态切换可能晚于音频包到达，导致丢包卡顿）
            tts_receiving_ = true;
            Schedule([this]() {
                aborted_ = false;
                tts_done_received_ = false;  // 进入Speaking，重置TTS完成标志
                SetDeviceState(kDeviceStateSpeaking);
            });
            return;
        }

        // ========== TTS 句子（文本+音频流） ==========
        if (strcmp(type_str, "tts_sentence") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text) && text->valuestring) {
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("assistant", message.c_str());
                });
            }
            return;
        }

        // ========== TTS 句子结束 ==========
        if (strcmp(type_str, "tts_sentence_end") == 0) {
            // 单句 TTS 结束，音频流继续，无需特殊处理
            return;
        }

        // ========== TTS 全部结束 ==========
        if (strcmp(type_str, "tts_done") == 0) {
            // ★ 同步清零 tts_receiving_：WebSocket线程立即生效，停止接收新音频包
            // tts_done 表示服务端TTS数据已发完，后续不再有音频包
            tts_receiving_ = false;
            Schedule([this]() {
                auto state = GetDeviceState();
                if (state == kDeviceStateSpeaking || state == kDeviceStateConnecting) {
                    // ★ tts_done 表示服务端TTS数据已发完，不代表ESP32已播完
                    // 只标记完成，等播放队列排空再切Listening（CheckIdleTimeout中检查）
                    // 避免清空队列丢弃未播音频导致"只播了第一句就卡住"
                    tts_done_received_ = true;
                    ESP_LOGI(TAG, "TTS: tts_done收到，等待播放队列排空再切Listening");
                }
            });
            return;
        }

        // ========== 服务端工具调用（复用 ToolManager 已注册工具，如 set_volume） ==========
        if (strcmp(type_str, "tool") == 0) {
            /* ★ 2026-09-17 崩溃根修：工具执行（主题对象创建/NVS写/LVGL主题注册）原本
             *   直接跑在 ssl_receive 任务上（mbedtls读→WS解析→JSON→工具执行→SendText
             *   回复），链路深度轻松突破8K栈，且拉长 TLS 上下文使用窗口。
             *   对齐 xiaozhi 架构：接收任务只做 TLS读+WS帧解析+轻分发，重活全部
             *   Schedule 到主循环。序列化一份 JSON 字符串投递，主循环里重新解析执行。 */
            char* payload = cJSON_PrintUnformatted(const_cast<cJSON*>(root));
            if (payload != nullptr) {
                Schedule([payload_str = std::string(payload)]() {
                    cJSON* cmd = cJSON_Parse(payload_str.c_str());
                    if (cmd != nullptr) {
                        std::string result;
                        std::string tool_id;
                        ToolManager::GetInstance().ExecuteToolCommand(cmd, result, tool_id);
                        cJSON_Delete(cmd);
                    }
                });
                cJSON_free(payload);
            }
            return;
        }

        // ========== 登录令牌（语音登录成功后服务端签发） ==========
        if (strcmp(type_str, "login_token") == 0) {
            auto token = cJSON_GetObjectItem(root, "token");
            if (cJSON_IsString(token) && token->valuestring && token->valuestring[0]) {
                Schedule([message = std::string(token->valuestring)]() {
                    Settings settings("sisi", true);
                    settings.SetString("user_token", message);
                    ESP_LOGI(TAG, ">>> 登录令牌已存储（后续连接免登录）");
                });
            }
            return;
        }

        // ========== 角色切换通知（服务端语音切换角色后下发，NVS 持久化实现重启保持） ==========
        if (strcmp(type_str, "role_switched") == 0) {
            auto aid = cJSON_GetObjectItem(root, "ai_character_id");
            auto rname = cJSON_GetObjectItem(root, "name");
            if (cJSON_IsNumber(aid) && aid->valueint > 0) {
                std::string name_str = cJSON_IsString(rname) && rname->valuestring ? rname->valuestring : "";
                Schedule([v = aid->valueint, name_str]() {
                    Settings settings("sisi", true);
                    settings.SetInt("ai_character_id", v);
                    ESP_LOGI(TAG, ">>> 角色已切换: ai_character_id=%d %s（NVS持久化，重启保持）",
                             v, name_str.c_str());
                });
            }
            return;
        }

        // ========== 板级配置下发（唤醒模式/会议模式，管理后台设置服务端推送） ==========
        if (strcmp(type_str, "device_config") == 0) {
            HandleDeviceConfig(root);
            return;
        }

        // ========== ASR 识别结果 ==========
        if (strcmp(type_str, "asr_partial") == 0) {
            // 流式 ASR 增量文本（服务端已累积为全量）：更新当前用户气泡，不新建
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text) && text->valuestring && text->valuestring[0]) {
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user_stream", message.c_str());
                });
            }
            return;
        }

        if (strcmp(type_str, "asr_result") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            auto silent = cJSON_GetObjectItem(root, "silent");
            if (cJSON_IsString(text) && text->valuestring && text->valuestring[0]) {
                ESP_LOGI(TAG, ">>> ASR: %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user_final", message.c_str());
                });
            }
            // silent=True: 服务端检测到静音，不会有TTS响应
            if (cJSON_IsTrue(silent)) {
                ESP_LOGI(TAG, ">>> ASR silent=True, 服务端判定静音，恢复聆听");
                Schedule([this]() {
                    waiting_for_server_ = false;
                    waiting_server_ticks_ = 0;
                    s2s_waiting_tts_greeting_ = false;
                    ClearStreamAudio();
                    // ★ listen_stop 时已关闭上行，恢复聆听必须重开——状态机已在
                    //   Listening，SetDeviceState 不会触发状态进入回调（重开代码在那里）
                    audio_service_.EnableAudioSending(true);
                });
            }
            return;
        }

        // ========== 回采过滤（TTS回声被过滤，客户端保持聆听） ==========
        if (strcmp(type_str, "echo_filtered") == 0) {
            auto reason = cJSON_GetObjectItem(root, "reason");
            ESP_LOGI(TAG, ">>> 回采过滤: %s", cJSON_IsString(reason) ? reason->valuestring : "(unknown)");
            Schedule([this]() {
                // 回采过滤：不切换到Speaking，保持Listening继续累积音频
                waiting_for_server_ = false;
                waiting_server_ticks_ = 0;
                s2s_waiting_tts_greeting_ = false;
                ClearStreamAudio();
                // ★ 同上：listen_stop 后上行已关，保持聆听必须重开
                audio_service_.EnableAudioSending(true);
            });
            return;
        }

        // ========== LLM 开始生成 ==========
        if (strcmp(type_str, "llm_start") == 0) {
            ESP_LOGI(TAG, ">>> LLM 开始生成");
            return;
        }

        // ========== LLM 流式文本 ==========
        if (strcmp(type_str, "llm_chunk") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text) && text->valuestring) {
                ESP_LOGD(TAG, ">>> LLM chunk: %s", text->valuestring);
            }
            return;
        }

        // ========== 本轮结束 ==========
        if (strcmp(type_str, "done") == 0) {
            auto silent = cJSON_GetObjectItem(root, "silent");
            if (cJSON_IsTrue(silent)) {
                ESP_LOGI(TAG, ">>> done(silent): 静音轮结束，恢复聆听");
                Schedule([this]() {
                    auto state = GetDeviceState();
                    if (state == kDeviceStateListening) {
                        waiting_for_server_ = false;
                        waiting_server_ticks_ = 0;
                        s2s_waiting_tts_greeting_ = false;
                        ClearStreamAudio();
                        // ★ listen_stop 时已关闭上行，恢复聆听必须重开
                        audio_service_.EnableAudioSending(true);
                    }
                });
            } else {
                ESP_LOGI(TAG, ">>> done: 本轮结束");
                // ASR 空文本等无 TTS 场景：done 时若仍在 Listening（未进 Speaking），
                // 需恢复聆听，否则 waiting_for_server_ 保持 true 导致 10s 超时才回 Idle
                Schedule([this]() {
                    auto state = GetDeviceState();
                    if (state == kDeviceStateListening) {
                        ESP_LOGI(TAG, ">>> done: 无TTS本轮结束，恢复聆听");
                        waiting_for_server_ = false;
                        waiting_server_ticks_ = 0;
                        s2s_waiting_tts_greeting_ = false;
                        ClearStreamAudio();
                        // ★ 上行重开（listen_stop 时关闭，状态机已在 Listening 不会
                        //   触发状态进入回调，必须在此显式重开，否则麦克风音频全被
                        //   丢弃→"再说话也不识别"卡死聆听）
                        audio_service_.EnableAudioSending(true);
                    }
                });
            }
            return;
        }

        // ========== 碎片语音：保持聆听继续录音（服务端判定不成句，本轮不回复）==========
        // ★ 2026-09-19 华哥需求（[holdon_listen]）：服务端识别出用户只说了
        //   一两个字/不成文句时，不发任何回复、只发本消息——设备保持 Listening
        //   持续上行即可（服务端已把该段暂存，与下一段拼接后重新识别）。
        if (strcmp(type_str, "holdon_listen") == 0) {
            ESP_LOGI(TAG, ">>> holdon_listen: 碎片语音，保持聆听继续录音");
            return;
        }

        // ========== 本轮被打断（服务端打断 LLM/TTS 生成） ==========
        if (strcmp(type_str, "aborted") == 0) {
            ESP_LOGI(TAG, ">>> aborted: 本轮被打断");
            return;
        }

        // ========== 超车打断（2026-09-10 服务端声纹判定） ==========
        // 服务端在 TTS 播放期间检测到当前用户声纹插话 → 发 interrupt。
        // WebSocket 全双工：此控制消息随时插入音频流，设备端收到后立即清
        // 播放/解码队列停止播放（超车），切回 Listening 持续上行采集用户抢话。
        if (strcmp(type_str, "interrupt") == 0) {
            ESP_LOGI(TAG, ">>> interrupt: 服务端声纹判定用户抢话 → 超车打断TTS");
            Schedule([this]() {
                // 立即停止播放并清空所有待播 TTS 队列（关键：超车，不等队列播完）
                audio_service_.StopPlayback();
                audio_service_.ResetDecoder();
                // TTS 期间上行已关（无端侧AEC防回声），抢话后必须重开，采集用户抢话语音
                audio_service_.EnableAudioSending(true);
                // 丢弃已累积的流式上行缓冲，重新开始采集用户抢话语音
                ClearStreamAudio();
                if (GetDeviceState() == kDeviceStateSpeaking) {
                    SetDeviceState(kDeviceStateListening);
                }
            });
            return;
        }

        // ========== 插话确认 ==========
        if (strcmp(type_str, "interrupt_ack") == 0) {
            ESP_LOGI(TAG, ">>> 服务端确认插话");
            return;
        }

        // ========== 会话结束（2026-09-12：服务端 bye，如会议总结播完） ==========
        // 服务端在全部 TTS 发完后发 bye。设备播完当前 TTS 后回 Idle 待机，
        // 不再进入对话聆听（修复：会议结束后默认又进入对话模式）。
        // 2026-09-14：bye 可携带 qrcode（会议总结二维码内容）+ qr_seconds（显示时长），
        // 设备显示二维码层倒计时 qr_seconds 后销毁并回 Idle。
        if (strcmp(type_str, "bye") == 0) {
            auto* qr_item = cJSON_GetObjectItem(root, "qrcode");
            auto* qs_item = cJSON_GetObjectItem(root, "qr_seconds");
            std::string qrcode_url = cJSON_IsString(qr_item) ? qr_item->valuestring : "";
            int qr_seconds = cJSON_IsNumber(qs_item) ? qs_item->valueint : 60;
            if (qr_seconds <= 0 || qr_seconds > 300) {
                qr_seconds = 60;
            }
            ESP_LOGI(TAG, ">>> bye: 服务端会话结束，TTS播完后回Idle (qrcode=%s %ds)",
                     qrcode_url.c_str(), qr_seconds);
            Schedule([this, display, qrcode_url, qr_seconds]() {
                meeting_bye_received_ = true;
                meeting_qrcode_url_ = qrcode_url;
                meeting_qr_seconds_ = qr_seconds;
                auto state = GetDeviceState();
                // 无 TTS 在播（Listening/Idle）：直接收尾（显示二维码或回 Idle）
                if (state == kDeviceStateListening || state == kDeviceStateIdle) {
                    meeting_bye_received_ = false;
                    meeting_auto_active_ = false;   // 会议结束，恢复超时机制
                    waiting_for_server_ = false;
                    waiting_server_ticks_ = 0;
                    s2s_waiting_tts_greeting_ = false;
                    ClearStreamAudio();
                    SetDeviceState(kDeviceStateIdle);
                    if (!meeting_qrcode_url_.empty()) {
                        display->ShowMeetingQrcode(meeting_qrcode_url_.c_str());
                        qr_countdown_ticks_ = meeting_qr_seconds_ * 5;  // 200ms/tick
                    }
                }
                // Speaking 状态：只标记，由 CheckIdleTimeout 在播放队列排空后切 Idle
            });
            return;
        }

        // ========== 会议自动恢复（2026-09-17 华哥需求②：断线/重启重连自动接续） ==========
        // 服务端发现本设备有未结束会议时下发（免唤醒直接进聆听持续录音）。
        // 会议期间跳过等待/空闲超时（CheckIdleTimeout），直到 bye（会议结束）
        // 或断连（OnAudioChannelClosed）复位 meeting_auto_active_。
        if (strcmp(type_str, "meeting_resume") == 0) {
            auto* mr_item = cJSON_GetObjectItem(root, "meeting_id");
            int meeting_id = cJSON_IsNumber(mr_item) ? mr_item->valueint : 0;
            ESP_LOGI(TAG, ">>> meeting_resume: 服务端自动恢复会议 id=%d → 免唤醒进聆听", meeting_id);
            Schedule([this, display]() {
                meeting_auto_active_ = true;
                meeting_bye_received_ = false;
                meeting_qrcode_url_.clear();    // 清上一场残留二维码，防止展示层复活
                waiting_for_server_ = false;
                waiting_server_ticks_ = 0;
                waiting_interrupt_speech_start_us_ = 0;
                s2s_waiting_tts_greeting_ = false;
                idle_clock_ticks_ = 0;
                ClearStreamAudio();
                display->SetChatMessage("system", "已自动恢复会议，请继续发言");
                if (GetDeviceState() != kDeviceStateListening) {
                    SetDeviceState(kDeviceStateListening);
                }
            });
            return;
        }

        // ========== 说话开始确认 ==========
        if (strcmp(type_str, "speech_started") == 0) {
            ESP_LOGI(TAG, ">>> 服务端确认 start_speech");
            return;
        }

        // ========== 错误消息 ==========
        if (strcmp(type_str, "error") == 0) {
            auto message = cJSON_GetObjectItem(root, "message");
            if (cJSON_IsString(message)) {
                std::string msg = message->valuestring;
                ESP_LOGW(TAG, ">>> 服务端错误: %s", msg.c_str());
                Schedule([display, msg]() {
                    display->SetChatMessage("system", msg.c_str());
                });
            }
            return;
        }

        // ========== 配置更新 ==========
        if (strcmp(type_str, "config_updated") == 0) {
            ESP_LOGI(TAG, ">>> 服务端配置已更新");
            return;
        }

        // ========== 其他消息（pingpong/ota_info/assets_info/connected 等已由 websocket.cc 处理）：静默忽略 ==========
    });

    protocol_->Start();
}

void App::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void App::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetChatMessage("system", "");
    }
}

void App::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void App::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void App::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

// ========== 按键事件处理（与原始application.cc保持一致）==========

void App::HandleToggleChatEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateConnecting) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        // 配网模式下按钮操作无特殊处理
        return;
    }

    if (!protocol_) {
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this]() {
                ContinueOpenAudioChannel();
            });
            return;
        }
        // S2S: 按键触发，发送首帧(仅配置，无文本)，服务端等待后续OGG音频
        SendWakeupMessage("");
    } else if (state == kDeviceStateSpeaking) {
        // ★ 打断TTS：彻底清除（包括未播报的），切到聆听
        // 注意：Speaking 状态下 has_wakeup 已是 True（之前唤醒过），无需重发 wakeup
        AbortSpeaking(kAbortReasonNone);
        audio_service_.StopPlayback();  // 清空decode+playback队列（不关I2S，避免竞态崩溃）
        SetDeviceState(kDeviceStateListening);
    } else if (state == kDeviceStateListening) {
        // S2S: 按钮切回Idle，清空累积音频
        audio_service_.EnableAudioSending(false);
        audio_service_.EnableVoiceProcessing(false);
        ClearStreamAudio();
        SetDeviceState(kDeviceStateIdle);
    }
}

void App::ContinueOpenAudioChannel() {
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            return;
        }
    }

    // S2S: 连接建立后发送首帧(仅配置)，然后进入Listening
    SendWakeupMessage("");
}

void App::HandleStartListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateConnecting) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        // 配网模式下不启动聆听
        return;
    }

    if (!protocol_) {
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this]() {
                ContinueOpenAudioChannel();
            });
            return;
        }
        // S2S: 发送首帧(仅配置)，然后进入Listening
        SendWakeupMessage("");
    } else if (state == kDeviceStateSpeaking) {
        // ★ 打断TTS：彻底清除（包括未播报的），切到聆听
        AbortSpeaking(kAbortReasonNone);
        audio_service_.StopPlayback();  // 清空decode+playback队列 + 关闭I2S输出
        SetDeviceState(kDeviceStateListening);
    }
}

void App::HandleStopListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateListening) {
        // S2S: 清空累积音频，回到Idle（无stop消息）
        ClearStreamAudio();
        SetDeviceState(kDeviceStateIdle);
    }
}

void App::HandleVadChangeEvent() {
    // ★ 2026-09-10 架构升级：设备端彻底去掉 VAD 判定，全部交给服务端！
    //   设备端任何时候都持续上行 Opus（Listening 与 Speaking 均保持上行），
    //   服务端用 Silero VAD 流式判定语音开始/结束（静音≥500ms 触发 ASR），
    //   TTS 播放期间的打断由服务端声纹判定（命中当前用户声纹才发 interrupt）。
    //   设备端不再做：说完判定 / 打断三重确认 / listen_stop 发送。
    //   此函数保留空实现仅为兼容事件回调，无任何业务逻辑。
    if (music_playing_) {
        return;
    }
}

void App::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        // ★ 客户端唤醒（2026-09-16）：本地 MultiNet 检测到唤醒词，直接发 noaudio
        //   首帧（text=你好/会议开始/开始汇报）驱动服务端，不上传唤醒词预滚音频
        //   （原 EncodeWakeWord 预滚编码链路已废弃——预滚音频上行会被服务端
        //   anyWordWakeup 二次匹配造成重复唤醒）
        wake_word = audio_service_.GetLastWakeWord();

        // ★ 彻底简化：设备开机已连接WebSocket，pingpong保活
        // 唤醒时只需检查底层TCP连接，已连接就直接发wakeup，绝不重连
        bool connected = false;
        if (auto* ws = dynamic_cast<WebsocketProtocol*>(protocol_.get())) {
            connected = ws->IsWebSocketConnected();
        } else {
            connected = protocol_->IsAudioChannelOpened();
        }

        if (!connected) {
            // 极端情况：WebSocket确实断了，才需要重连
            ESP_LOGW(TAG, "唤醒时WebSocket未连接，需要重连");
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this, wake_word]() {
                DoConnectAndSendWakeup(wake_word);
            });
            return;
        }

        // ★ 已连接：直接发wakeup，零重连
        SendWakeupMessage(wake_word);

    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeUp);
        while (audio_service_.PopPacketFromSendQueue());

        // 重新进入Listening：由HandleStateChangedEvent统一处理所有协议逻辑
        audio_service_.EnableAudioSending(false);
        if (state == kDeviceStateListening) {
            // Listening中唤醒（切换角色）：ResetVAD后重新进Listening
            audio_service_.ResetVoiceProcessing();
            SetDeviceState(kDeviceStateListening);
        } else {
            // Speaking中唤醒：直接转Listening
            SetDeviceState(kDeviceStateListening);
        }
    } else if (state == kDeviceStateConnecting) {
        // Connecting状态收到唤醒（用户不耐烦再喊一次），忽略
        ESP_LOGW(TAG, "唤醒时已在Connecting状态，忽略重复唤醒");
    }
}

// S2S: 唤醒后发送首帧 JSON（带文本"你好"触发LLM回应），然后进入 Listening 等待TTS
// ★ 会议唤醒词（"会议开始/开始会议/会议模式"，action="meeting"）：发 text="会议开始"，
//   服务端 noaudio 链路以 text 替代 ASR 结果，命中 s2s_meeting.START_WORDS 直达会议模式
// ★ 汇报唤醒词（"开始汇报/汇报开始/汇报模式"，action="report"）：发 text="开始汇报"，
//   2026-09-17 会议/汇报模式合并：触发词统一走 s2s_meeting.START_WORDS（含汇报词）
//   拦截进会议，场景（会议/工作汇报）由服务端总结模板 AI 自判，设备端两 action 行为一致
void App::SendWakeupMessage(const std::string& wake_word) {
    bool with_text = !wake_word.empty();
    // ★ 2026-09-17 假唤醒根修：text 必须发实际检出的唤醒词（"你好如意"等），
    //   不能发笼统的"你好"——服务端 noaudio 门控用唤醒词表匹配，"你好"不在
    //   表内被当垃圾忽略 → 设备零回应（假唤醒，需二次唤醒才有"我在"）。
    //   词表见 s2s_service.py _S2S_WAKE_WORDS_RAW，与 custom_wake_word.cc 对齐。
    std::string wake_text = with_text ? wake_word : "";
    std::string action = audio_service_.GetLastWakeWordAction();
    if (with_text && action == "meeting") {
        wake_text = "会议开始";
    } else if (with_text && action == "report") {
        wake_text = "开始汇报";
    }
    ESP_LOGI(TAG, "Wakeup detected (action=%s), sending S2S first frame (text=%s): %s",
             action.c_str(), with_text ? wake_text.c_str() : "(none)", wake_word.c_str());
    protocol_->SendS2SFirstFrame(wake_text);
    // 带文本时等待服务端TTS问候，期间忽略VAD（防止空OGG发送）
    s2s_waiting_tts_greeting_ = with_text;
    SetDeviceState(kDeviceStateListening);
}

// ★ 当前生效唤醒模式（2026-09-16 客户端/服务端唤醒运行时切换）：
//   client=本地 MultiNet 唤醒（idle 不上行）；server=服务端 anyWordWakeup（idle 常开麦）。
//   编译打包了本地唤醒模型（CONFIG_USE_CUSTOM_WAKE_WORD，16MB 板）默认 client；
//   服务端 device_config 消息可运行时改写（NVS "sisi"."wake_mode" 持久化，重启保持）。
bool App::IsClientWakeMode() {
#if CONFIG_USE_CUSTOM_WAKE_WORD
    Settings settings("sisi", false);
    return settings.GetString("wake_mode") != "server";  // 默认 client，显式 server 才切换
#else
    return false;  // 8MB 板（stick）未打包本地唤醒模型：只能服务端唤醒
#endif
}

// ★ 服务端 device_config 下发（板级配置：唤醒模式/会议模式）：
//   {"type":"device_config","wake_mode":"client|server","meeting_mode":"meeting|report"}
//   NVS 持久化 + idle 态即时重挂待命（pending 异步，不在 WS 线程直接操作音频服务）。
void App::HandleDeviceConfig(const cJSON* root) {
    auto wake_mode = cJSON_GetObjectItem(root, "wake_mode");
    auto meeting_mode = cJSON_GetObjectItem(root, "meeting_mode");
    std::string wm = cJSON_IsString(wake_mode) && wake_mode->valuestring
                         ? wake_mode->valuestring : "";
    std::string mm = cJSON_IsString(meeting_mode) && meeting_mode->valuestring
                         ? meeting_mode->valuestring : "";
    ESP_LOGI(TAG, ">>> device_config: wake_mode=%s meeting_mode=%s", wm.c_str(), mm.c_str());
    Schedule([this, wm, mm]() {
        {
            Settings settings("sisi", true);
            if (!wm.empty()) settings.SetString("wake_mode", wm);
            if (!mm.empty()) settings.SetString("meeting_mode", mm);
        }
        // idle 态即时应用新唤醒模式：关旧待命、重挂新待命（下一拍生效）
        if (GetDeviceState() == kDeviceStateIdle) {
            audio_service_.EnableWakeWordDetection(false);
            audio_service_.EnableVoiceProcessing(false);
            if (IsClientWakeMode()) {
                audio_service_.EnableAudioSending(false);
                idle_wake_pending_ = true;
            } else {
                audio_service_.EnableAudioSending(true);
                idle_voice_pending_ = true;
            }
            ESP_LOGI(TAG, ">>> 唤醒模式已切换并重挂 idle 待命");
        }
    });
}

// ★ 仅用于极端情况：WebSocket确实断了才走这里
void App::DoConnectAndSendWakeup(const std::string& wake_word) {
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->OpenAudioChannel()) {
        SetDeviceState(kDeviceStateIdle);
#if CONFIG_USE_CUSTOM_WAKE_WORD
        audio_service_.EnableWakeWordDetection(true);
#endif
        return;
    }

    SendWakeupMessage(wake_word);
}

// 核心原则：每个状态只管自己的音频开关，不越权操作

void App::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = Board::GetInstance().GetLed();
    led->OnStateChanged();

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateStarting:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();
            display->ShowProductSelector();
            // ★ 唤醒待命按模式分流（2026-09-16 华哥决策：meeting 板用客户端唤醒）：
            //   client=本地 MultiNet 检测唤醒词，idle 不上行音频（服务端零误触发，
            //   【23】"随便说话就进对话"的釜底抽薪方案）；server=服务端 anyWordWakeup，
            //   idle 常开麦上行待命（stick 等 8MB 板无本地模型，唯一选择）。
            //   ★ 两者都不能在状态机回调里同步调（主循环死等风险，见 pending 注释）。
            if (IsClientWakeMode()) {
                audio_service_.EnableAudioSending(false);
                idle_wake_pending_ = true;
            } else {
                audio_service_.EnableAudioSending(true);
                idle_voice_pending_ = true;
            }
            waiting_for_server_ = false;  // 退Idle，重置
            break;

        case kDeviceStateUpgrading:
            display->SetStatus(Lang::Strings::UPGRADING);
            display->HideProductSelector();
            // ★ Alert() 已播过 OGG_UPGRADE，此处不重复播放
            break;

        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetChatMessage("system", "");
            display->HideProductSelector();
            meeting_bye_received_ = false;  // 新会话开始，复位会话结束标志
            meeting_qrcode_url_.clear();    // 复位会议二维码状态（残留层随倒计时/此处销毁）
            qr_countdown_ticks_ = 0;
            Board::GetInstance().GetDisplay()->HideMeetingQrcode();
            break;

        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->HideProductSelector();

            // ★ 2026-09-10 架构升级：设备端不再做 VAD 判定（vad_has_spoken_/
            //   vad_silence_start_us_/vad_silence_timer 已全部移除），持续上行由
            //   EnableAudioSending(true) 保证，服务端 Silero VAD 判定语音端点。
            // S2S: 首帧带文本时保留 waiting_for_server_=true（等待TTS问候，忽略VAD）
            if (!s2s_waiting_tts_greeting_) {
                waiting_for_server_ = false;  // 正常聆听，重置等待标志
            }
            waiting_server_ticks_ = 0;
            waiting_interrupt_speech_start_us_ = 0;  // 重置打断防抖计时
            idle_clock_ticks_ = 0;       // 重置空闲超时计数
            listening_audio_bytes_ = 0;   // 重置音频字节数统计

            // S2S: 清空上一轮残留的累积音频
            ClearStreamAudio();

            // ★ 进入Listening：processor可能在Speaking期间被关闭，需重新启动
            if (!audio_service_.IsAudioProcessorRunning()) {
                audio_service_.EnableVoiceProcessing(true);
                if (previous_state_ == kDeviceStateSpeaking) {
                    vad_warmup_start_us_ = esp_timer_get_time();
                    vad_warmup_duration_ms_ = VAD_WARMUP_AFTER_TTS_MS;
                    ESP_LOGI(TAG, ">>> 启动AudioProcessor（TTS后warmup %dms）", VAD_WARMUP_AFTER_TTS_MS);
                } else {
                    vad_warmup_start_us_ = esp_timer_get_time();
                    vad_warmup_duration_ms_ = VAD_WARMUP_MS;
                    ESP_LOGI(TAG, ">>> 启动AudioProcessor（默认warmup %dms）", VAD_WARMUP_MS);
                }
            } else {
                audio_service_.ResetVoiceProcessing();
                if (previous_state_ == kDeviceStateSpeaking) {
                    vad_warmup_start_us_ = esp_timer_get_time();
                    vad_warmup_duration_ms_ = VAD_WARMUP_AFTER_TTS_MS;
                    ESP_LOGI(TAG, ">>> AudioProcessor保持运行（TTS后warmup %dms）", VAD_WARMUP_AFTER_TTS_MS);
                } else {
                    vad_warmup_start_us_ = esp_timer_get_time();
                    vad_warmup_duration_ms_ = VAD_WARMUP_MS;
                    ESP_LOGI(TAG, ">>> AudioProcessor保持运行（ResetVAD）");
                }
            }
            // S2S: 无需 SendStartListening（音频本身就是开始信号）
            ESP_LOGI(TAG, ">>> S2S Listening (accumulate audio, waiting_greeting=%d)", s2s_waiting_tts_greeting_ ? 1 : 0);
            audio_service_.EnableWakeWordDetection(false);
            audio_service_.EnableAudioSending(true);

            break;

        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);
            display->HideProductSelector();
            // S2S: 无需 SendStopListening（音频本身就是结束信号）
            ESP_LOGI(TAG, ">>> S2S Speaking (enter, clear greeting flag)");
            // ★ 2026-09-18⑧ 华哥定论：TTS 播报期按 AEC 能力决定是否上行——
            //   有客户端 AEC 的板（meeting，硬件回采参考）保持上行 → 超车打断可用；
            //   无 AEC 的板（anan/sisi）**直接关麦不上行**：回采没有硬件对消，服务端
            //   必然把它当"真人插话"→ AI 一说话就自己打断自己。关麦从物理上杜绝。
            //   会议进行中没有 TTS 播放（不进入 Speaking）→ 持续录音上行不受任何影响，
            //   只在会议总结播报等 Speaking 期间按本规则关麦。
            audio_service_.EnableAudioSending(aec_mode_ == kAecOnDeviceSide);
            // ★ processor 全程运行（2026-09-09）：保持 AFE 持续处理，播完即聆零 warmup。
            s2s_waiting_tts_greeting_ = false;  // TTS开始，清除问候等待标志
            waiting_for_server_ = false;
            break;

        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            audio_service_.PlaySound(Lang::Sounds::OGG_WIFICONFIG);
            break;

        default:
            break;
    }
}

void App::StartReconnectTask(const char* task_name) {
    // ★ 2026-09-18：重连任务创建串行化（见 app.h 注释）——心跳超时与断连回调
    //   两条路径可能同时到达，原"先查句柄再创建"无锁竞态会起两个重连任务。
    std::lock_guard<std::mutex> lock(reconnect_mutex_);
    if (activation_task_handle_ != nullptr) {
        return;   // 已有重连任务在跑，不重复起
    }
    if (reconnect_retry_count_ >= MAX_RECONNECT_RETRIES) {
        ESP_LOGW(TAG, "Max reconnect retries reached (%d), will keep trying", reconnect_retry_count_);
        return;
    }
    reconnect_retry_count_++;
    ESP_LOGI(TAG, "Reconnect attempt %d (delay %d ms)", reconnect_retry_count_, RECONNECT_DELAY_MS);
    xTaskCreate([](void* arg) {
        App* app = static_cast<App*>(arg);
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
        app->ActivationTask();
        {
            std::lock_guard<std::mutex> lk(app->reconnect_mutex_);
            app->activation_task_handle_ = nullptr;
        }
        vTaskDelete(NULL);
    }, task_name, 4096 * 2, this, 2, &activation_task_handle_);
}

void App::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // ★ 2026-09-18 队列有界（上一轮只给音频队列加了上限，这条漏网）：本队列由
        //   WS 接收任务生产（每句 tts_sentence / 每条 tool 消息一条），主循环消费；
        //   主循环一旦被拖慢，它会无界增长直到内存耗尽。满则丢最旧并限频告警。
        if (main_tasks_.size() >= kMaxPendingTasks) {
            main_tasks_.pop_front();
            static uint32_t s_task_drop_cnt = 0;
            if (++s_task_drop_cnt % 10 == 1) {
                ESP_LOGW(TAG, "主循环调度队列已满(%u)，丢弃最旧任务，累计丢弃=%lu",
                         (unsigned)kMaxPendingTasks, (unsigned long)s_task_drop_cnt);
            }
        }
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void App::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    tts_receiving_ = false;  // 同步停止接收TTS音频包
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void App::Reboot() {
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool App::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // ★ 升级前强制关闭WebSocket，防止服务端idle watcher在下载期间误触发"你先忙，再见"
    // 不检查IsAudioChannelOpened()，因为session_id可能已清空导致检查失败
    if (protocol_) {
        protocol_->CloseAudioChannel();
    }

    // ★ 清空所有残留音频队列（TTS解码/播放），防止升级播报与旧TTS混音
    audio_service_.StopPlayback();
    audio_service_.ResetDecoder();
    audio_service_.EnableAudioSending(false);
    audio_service_.EnableVoiceProcessing(false);

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    // ★ 挂起 taskLVGL 任务，避免 OTA 写 flash 期间 cache 禁用导致 taskLVGL 崩溃
    // 根因：esp_https_ota 写 flash 时会临时禁用 SPI0/1 cache（PSRAM 共享同一总线），
    // taskLVGL 此时若执行 flash/PSRAM 中的代码或访问 PSRAM 中的 LVGL 数据，
    // 会触发 LoadProhibited（EXCVADDR 是小偏移量地址，典型 use-after-free 症状）。
    // ESP-IDF IPC 机制虽让另一 CPU 进入 IRAM spin，但 cache 禁用瞬间 taskLVGL
    // 仍可能处于中断/异常返回路径上访问无效地址。
    // 解决方案：OTA 前先持有 LVGL 锁（确保 taskLVGL 不在临界区），再 vTaskSuspend，
    // OTA 期间 LVGL 静止（进度不刷新，但 OTA 完成后会立即重启显示结果）。
    TaskHandle_t lvgl_task = xTaskGetHandle("taskLVGL");
    if (lvgl_task != nullptr) {
        lvgl_port_lock(portMAX_DELAY);  // 持有锁，确保 taskLVGL 不在持有锁状态
        vTaskSuspend(lvgl_task);
        lvgl_port_unlock();
        ESP_LOGI(TAG, "OTA: taskLVGL suspended to avoid cache-disable crash during flash write");
    } else {
        ESP_LOGW(TAG, "OTA: taskLVGL not found, cannot suspend (LVGL crash risk)");
    }

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        // Schedule 不会被消费（主线程被 OTA 阻塞），仅记录日志便于调试
        ESP_LOGI(TAG, "OTA progress: %d%% %uKB/s", progress, speed / 1024);
    });

    if (!upgrade_success) {
        // 恢复 taskLVGL，让 UI 重新工作以显示错误
        if (lvgl_task != nullptr) {
            vTaskResume(lvgl_task);
        }
        audio_service_.Start();
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // 恢复 taskLVGL 显示"升级成功"，再重启
        if (lvgl_task != nullptr) {
            vTaskResume(lvgl_task);
        }
        display->SetChatMessage("system", "升级成功，正在重启...");
        vTaskDelay(pdMS_TO_TICKS(2000));  // 等待 LVGL 刷新一帧显示提示
        Reboot();
        return true;
    }
}

void App::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
        // ★ 同样彻底简化：已连接就直接发wakeup
        bool connected = false;
        if (auto* ws = dynamic_cast<WebsocketProtocol*>(protocol_.get())) {
            connected = ws->IsWebSocketConnected();
        } else {
            connected = protocol_->IsAudioChannelOpened();
        }

        if (!connected) {
            ESP_LOGW(TAG, "按键唤醒时WebSocket未连接，需要重连");
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this, wake_word]() {
                DoConnectAndSendWakeup(wake_word);
            });
            return;
        }

        // ★ 已连接：直接发wakeup，零重连
        SendWakeupMessage(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {
        // ★ 聆听中唤醒（切换角色）：中断当前对话，重新进Listening，不关闭WS
        Schedule([this]() {
            AbortSpeaking(kAbortReasonWakeUp);
            while (audio_service_.PopPacketFromSendQueue());
            audio_service_.EnableAudioSending(false);
            audio_service_.ResetVoiceProcessing();
            SetDeviceState(kDeviceStateListening);
        });
    }
}

bool App::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    return true;
}

void App::SendToolResponse(const std::string& payload) {
    Schedule([this, payload = std::move(payload)]() {
        if (protocol_) {
            protocol_->SendToolResponse(payload);
        }
    });
}

void App::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        default:
            break;
        }

        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void App::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void App::ResetProtocol() {
    Schedule([this]() {
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        protocol_.reset();
    });
}

// ════════════════════════════════════════════════════════════════
// S2S 流式发送（2026-09-10 架构升级）
// 设备端去掉 VAD 判定后不再发送 listen_stop，持续上行由 MAIN_EVENT_SEND_AUDIO
// 达阈值即流式发送完成；服务端 Silero VAD 判定语音端点（静音≥500ms 触发 ASR）。
// FlushStreamAudio 已删除（无调用方）。
// ════════════════════════════════════════════════════════════════

void App::ClearStreamAudio() {
    if (!stream_send_buffer_.empty()) {
        ESP_LOGI(TAG, "S2S: 清空流式缓冲 [opus_bytes=%u]", (unsigned)stream_send_buffer_.size());
        stream_send_buffer_.clear();
    }
    listening_audio_bytes_ = 0;
}
