#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <atomic>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "state.h"
#include "device_state.h"
#include "ogg_muxer.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
// #define MAIN_EVENT_VAD_DIAG_TICK        (1 << 13)  // 已移除：诊断日志刷屏导致串口超时+TTS卡顿
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class App {
public:
    static App& GetInstance() {
        static App instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendToolResponse(const std::string& payload);  // 发送工具响应（替代 MCP）
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
    
    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();

private:
    App();
    ~App();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    // ★ 2026-09-18：主循环调度队列硬上限（WS 接收任务生产、主循环消费；满丢最旧）
    static constexpr size_t kMaxPendingTasks = 64;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    DeviceState previous_state_ = kDeviceStateUnknown;
    AecMode aec_mode_ = kAecOnDeviceSide;
    std::string last_error_message_;
    AudioService audio_service_;
    std::unique_ptr<Ota> ota_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool waiting_for_server_ = false;  // 已发送音频等待服务端响应，此期间不触发Listening超时
    bool tts_done_received_ = false;   // 服务端TTS数据已发完（不代表ESP32已播完），等播放队列空再切Listening
    bool meeting_bye_received_ = false; // 服务端bye（会话结束）：TTS播完后切Idle待机，不再进对话聆听
    // ★ 2026-09-17 服务端自动恢复会议（meeting_resume）：免唤醒持续聆听录音，
    //   跳过等待/空闲超时（会议是长时录音场景），直到 bye（会议结束）或断连复位
    bool meeting_auto_active_ = false;
    // ★ 2026-09-16 服务端唤醒待命（华哥确认架构）：idle 需常开 VoiceProcessing 维持上行，
    //   但 bye 切 Idle 发生在主循环 tick——同步调 EnableVoiceProcessing(true) 内部
    //   ResetDecoder 要等输出任务收尾，TTS 尾巴未清空时主循环死等（状态卡死不回 Idle）。
    //   改为置 pending 标志，下一拍 CheckIdleTimeout 顶部异步执行（那时输出必然空闲）。
    bool idle_voice_pending_ = false;
    // ★ 2026-09-16 客户端唤醒待命（华哥决策：meeting 等 16MB 板用客户端唤醒模型，
    //   stick 等 8MB 板才走服务端唤醒）：idle 开本地 MultiNet 检测、不上行音频，
    //   服务端零误触发。同样走 pending 机制延后一拍异步执行。
    bool idle_wake_pending_ = false;
    // ★ 2026-09-25 OTA 时序回归开机最前（0075 实测 idle 路径死路）：OTA 检查/升级
    //   在 ActivationTask 最前执行——此刻 WS 未连、会话未开始、MultiNet 未加载、
    //   SRAM 最富裕，零并发；升级成功自动重启。idle 态常连 WS TLS + MN7 已把
    //   SRAM 吃到 1-12KB，OTA 第二条 TLS 在 idle 必然分配失败（实测 header
    //   timeout + EspSsl 退出），idle 路径废弃。
    //   （09-20 的"避让会话"结论仍有效：绝不在会话活跃期跑 OTA——开机最前时机
    //     天然满足，且比"挂起到 Idle"多解决了 idle 内存死路。）
    bool ota_boot_check_done_ = false;  // 仅开机首次激活检查，重连路径不查（重连要快）
    bool IsClientWakeMode();            // 当前生效唤醒模式：client=本地唤醒 / server=服务端唤醒
    void HandleDeviceConfig(const cJSON* root);  // 服务端 device_config 下发（唤醒/会议模式）
    std::string meeting_qrcode_url_;    // bye携带的会议总结二维码内容（空=无二维码，直接回Idle）
    int meeting_qr_seconds_ = 60;       // 二维码显示时长（秒）
    int qr_countdown_ticks_ = 0;        // 二维码显示倒计时（CheckIdleTimeout 200ms/tick，到0销毁并回Idle）
    std::atomic<bool> tts_receiving_{false};  // TTS音频接收中（tts_start同步置true，tts_done同步置false，OnIncomingAudio据此收包，不依赖异步Schedule切换的state）
    int waiting_server_ticks_ = 0;      // 等待服务端响应的tick计数（100ms/tick）
    static constexpr int WAITING_SERVER_TIMEOUT_TICKS = 100;  // 等待服务端TTS响应10秒无响应→回Idle
    static constexpr int LISTENING_IDLE_TIMEOUT_TICKS = 600;  // 聆听状态60秒无语音→回Idle（未唤醒，仍连接WS）
    int listening_audio_bytes_ = 0;   // 本轮聆听中累计发送的音频字节数
int64_t waiting_interrupt_speech_start_us_ = 0;  // waiting_for_server期间打断防抖：需持续说话1500ms才确认是用户主动打断
    int64_t vad_warmup_start_us_ = 0;  // VAD warmup：进入Listening后AFE稳定前的忽略期起始时间
    int vad_warmup_duration_ms_ = 400; // 当前warmup时长(ms)，可动态调整（TTS后缩短）
static constexpr int VAD_WARMUP_MS = 400;  // VAD warmup默认时长(ms)：AFE启动后前400ms的VAD结果忽略
static constexpr int VAD_WARMUP_AFTER_TTS_MS = 0;  // TTS结束后warmup：processor全程运行无需重启，零忽略期（话头零丢失）
    std::atomic<bool> music_playing_{false};

    // S2S 流式发送缓冲：Listening 期间边累积 Opus 帧（带长度前缀）边发送，达阈值立即流式上传，VAD 静音发剩余+listen_stop
    std::vector<uint8_t> stream_send_buffer_;
    static constexpr size_t kStreamChunkSize = 2400;  // 流式发送阈值：~20 opus帧/400ms/2400B，达此值立即上传
    // S2S 首帧(带文本)已发送，等待服务端TTS问候期间忽略VAD(防止空音频发送)
    bool s2s_waiting_tts_greeting_ = false;
    // Opus 帧采样数（用于 OGG granule 计算，16kHz@20ms = 320 samples）
    // 注意：OGG granule 必须用 48kHz 域，所以发送时需要换算：320 * 48000/16000 = 960
    static constexpr int OPUS_FRAME_SAMPLES_16K = 320;       // 16kHz@20ms
    static constexpr int OPUS_FRAME_SAMPLES_48K = 960;       // 48kHz@20ms（OGG granule 用）
    int clock_ticks_ = 0;
    int idle_clock_ticks_ = 0;
    static constexpr int IDLE_TIMEOUT_TICKS = 300;
    int heartbeat_ticks_ = 0;
    static constexpr int HEARTBEAT_INTERVAL_TICKS = 60;   // 60秒心跳周期
    // Listening超时由服务端控制（60秒无语音发idle），客户端不再管理
    int pingpong_wait_ticks_ = 0;
    bool pingpong_pending_ = false;
    static constexpr int PINGPONG_TIMEOUT_TICKS = 20;    // 20秒pingpong超时
    TaskHandle_t activation_task_handle_ = nullptr;
    // ★ 2026-09-18：重连任务创建串行化（心跳超时路径与断连回调路径原先各自
    //   "先查句柄再创建"无锁竞态 → 可同时起两个重连任务、protocol_ 被重复替换）。
    std::mutex reconnect_mutex_;
    void StartReconnectTask(const char* task_name);
    int reconnect_retry_count_ = 0;
    static constexpr int MAX_RECONNECT_RETRIES = 9999;   // 几乎无限重试，不放弃
    static constexpr int RECONNECT_DELAY_MS = 5000;        // 重连间隔5秒

    // Idle timeout check
    void CheckIdleTimeout();
    void SendHeartbeat();
    
    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleVadChangeEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ContinueOpenAudioChannel();
    void SendWakeupMessage(const std::string& wake_word);
    void DoConnectAndSendWakeup(const std::string& wake_word);

    // S2S 流式发送（2026-09-10 架构升级）：设备端去掉 VAD 判定后不再发 listen_stop，
    // 持续上行由 MAIN_EVENT_SEND_AUDIO 达阈值即发；FlushStreamAudio 已删除。
    void ClearStreamAudio();

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void InitializeProtocol();
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
