#include "wifi_board.h"

#include "display.h"
#include "app.h"
#include "system_info.h"
#include "settings.h"
#include "audio/assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>
#include <esp_sntp.h>
#include <time.h>
#include <utility>

#include <font_awesome.h>
#include <wifi_manager.h>
#include <wifi_station.h>
#include <ssid_manager.h>
#include "afsk_demod.h"
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#include "blufi.h"
#endif

static const char *TAG = "WifiBoard";

// Connection timeout in seconds
static constexpr int CONNECT_TIMEOUT_SEC = 60;

WifiBoard::WifiBoard() {
    // Create connection timeout timer
    esp_timer_create_args_t timer_args = {
        .callback = OnWifiConnectTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_connect_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &connect_timer_);
}

WifiBoard::~WifiBoard() {
    if (connect_timer_) {
        esp_timer_stop(connect_timer_);
        esp_timer_delete(connect_timer_);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::StartNetwork() {
    auto& wifi_manager = WifiManager::GetInstance();

    // Initialize WiFi manager
    WifiManagerConfig config;
    config.ssid_prefix = "NiceAI";
    config.language = Lang::CODE;
    wifi_manager.Initialize(config);

    // Set unified event callback - forward to NetworkEvent with SSID data
    wifi_manager.SetEventCallback([this](WifiEvent event, const std::string& data) {
        switch (event) {
            case WifiEvent::Scanning:
                OnNetworkEvent(NetworkEvent::Scanning);
                break;
            case WifiEvent::Connecting:
                OnNetworkEvent(NetworkEvent::Connecting, data);
                break;
            case WifiEvent::Connected:
                OnNetworkEvent(NetworkEvent::Connected, data);
                break;
            case WifiEvent::Disconnected:
                OnNetworkEvent(NetworkEvent::Disconnected);
                break;
            case WifiEvent::ConfigModeEnter:
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter);
                break;
            case WifiEvent::ConfigModeExit:
                OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
                break;
        }
    });

    // Try to connect or enter config mode
    TryWifiConnect();
}

void WifiBoard::TryWifiConnect() {
    auto& ssid_manager = SsidManager::GetInstance();
    bool have_ssid = !ssid_manager.GetSsidList().empty();

    if (have_ssid) {
        // Start connection attempt with timeout
        ESP_LOGI(TAG, "Starting WiFi connection attempt");
        esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
        WifiManager::GetInstance().StartStation();
    } else {
        // No SSID configured, enter config mode
        // Wait for the board version to be shown
        vTaskDelay(pdMS_TO_TICKS(1500));
        StartWifiConfigMode();
    }
}

void WifiBoard::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {
        case NetworkEvent::Connected:
            // Stop timeout timer
            esp_timer_stop(connect_timer_);
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
            // make sure blufi resources has been released
            Blufi::GetInstance().deinit();
#endif
            in_config_mode_ = false;
            ESP_LOGI(TAG, "Connected to WiFi: %s", data.c_str());
            // ★ S2S 协议下服务端不再推送 server_time，WiFi 连接成功后启动 SNTP 同步系统时间
            StartSntpSync();
            break;
        case NetworkEvent::Scanning:
            ESP_LOGI(TAG, "WiFi scanning");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "WiFi connecting to %s", data.c_str());
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "WiFi disconnected");
            break;
        case NetworkEvent::WifiConfigModeEnter:
            ESP_LOGI(TAG, "WiFi config mode entered");
            in_config_mode_ = true;
            break;
        case NetworkEvent::WifiConfigModeExit:
            ESP_LOGI(TAG, "WiFi config mode exited");
            in_config_mode_ = false;
            // Try to connect with the new credentials
            TryWifiConnect();
            break;
        default:
            break;
    }

    // Notify external callback if set
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void WifiBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

// ★ 2026-09-18 根修（esp_timer 任务禁止阻塞）：本回调运行在 ESP_TIMER_TASK 里，
//   而 StopStation（esp_wifi_stop/disconnect/netif_destroy）与 StartWifiConfigMode
//   （起 AP + HTTP 配置服务器 + DNS）都是数秒级网络栈重操作。一旦在此阻塞，同一任务上
//   承载的 App 200ms 时钟 tick 立即停摆 → 主循环永久阻塞（整机卡死：无心跳/无重连）。
//   改为投递到独立任务执行，回调本身只投递。
void WifiBoard::OnWifiConnectTimeout(void* arg) {
    ESP_LOGW(TAG, "WiFi connection timeout, entering config mode");
    xTaskCreate([](void* a) {
        auto* board = static_cast<WifiBoard*>(a);
        WifiManager::GetInstance().StopStation();
        board->StartWifiConfigMode();
        vTaskDelete(NULL);
    }, "wifi_cfg_timeout", 4096, arg, 3, nullptr);
}

void WifiBoard::StartWifiConfigMode() {
    in_config_mode_ = true;
    // Transition to wifi configuring state
    App::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
    auto& wifi_manager = WifiManager::GetInstance();

    wifi_manager.StartConfigAp();

    // Show config prompt after a short delay
    // ★ 2026-09-12：原实现按引用捕获栈上局部引用（[&wifi_manager]），Schedule 延迟
    //   执行时 StartWifiConfigMode 栈帧已退出 → 悬空引用 → Alert（含配网提示音）
    //   可能整段不执行或读到脏数据。改为按值捕获 this（WifiBoard 单例堆对象，
    //   生命周期恒定），内部通过 WifiManager::GetInstance() 取数据。
    App::GetInstance().Schedule([this]() {
        std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
        hint += WifiManager::GetInstance().GetApSsid();
        hint += Lang::Strings::ACCESS_VIA_BROWSER;
        hint += WifiManager::GetInstance().GetApWebUrl();

        App::GetInstance().Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "gear", Lang::Sounds::OGG_WIFICONFIG);
    });
#elif CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    auto &blufi = Blufi::GetInstance();
    // initialize esp-blufi protocol
    blufi.init();
#endif
#if CONFIG_USE_ACOUSTIC_WIFI_PROVISIONING
    // Start acoustic provisioning task
    auto codec = Board::GetInstance().GetAudioCodec();
    int channel = codec ? codec->input_channels() : 1;
    ESP_LOGI(TAG, "Starting acoustic WiFi provisioning, channels: %d", channel);

    xTaskCreate([](void* arg) {
        auto ch = reinterpret_cast<intptr_t>(arg);
        auto& app = App::GetInstance();
        auto& wifi = WifiManager::GetInstance();
        auto disp = Board::GetInstance().GetDisplay();
        audio_wifi_config::ReceiveWifiCredentialsFromAudio(&app, &wifi, disp, ch);
        vTaskDelete(NULL);
    }, "acoustic_wifi", 4096, reinterpret_cast<void*>(channel), 2, NULL);
#endif
}

void WifiBoard::EnterWifiConfigMode() {
    ESP_LOGI(TAG, "EnterWifiConfigMode called");
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);

    auto& app = App::GetInstance();
    auto state = app.GetDeviceState();

    // ★ 2026-09-20 根修（华哥"meeting 板连 WiFi 时进不了配网、卡着一直打不开"）：
    //   本函数由 BOOT 按钮回调调用，而 iot_button 的 button_cb 与 App 的 200ms
    //   clock_timer 的 dispatch_method 都是 ESP_TIMER_TASK —— esp_timer 是**单任务**，
    //   两者共用同一条任务。配网要做 StopStation（esp_wifi_scan_stop/disconnect/stop +
    //   netif 销毁）与 StartWifiConfigMode（起 AP + HTTP 配置服务器 + DNS），都是数秒级
    //   网络栈重操作。原实现的 Starting 分支在此**同步**执行，于是 esp_timer 任务被独占：
    //   ① clock_timer 停摆（App 主循环唯一周期唤醒源）→ 主循环冻结、状态机不推进；
    //   ② wifi_scan_timer 无法派发 → 配网网页 /scan 拿不到列表 → 看不到 SSID、点不进
    //      "账号密码输入页"；
    //   ③ StartWifiConfigMode 里 Schedule 的配网提示（热点名 + 访问网址）排不上 → 提示页
    //      一直出不来。
    //   a14d7f4ac 全局迁出 esp_timer 阻塞点时只改了 OnWifiConnectTimeout，漏了按钮这条
    //   （Starting 分支），此处补齐：两条分支一律"回调只投递，重活交独立任务"。
    if (state == kDeviceStateSpeaking || state == kDeviceStateListening || state == kDeviceStateIdle) {
        // Reset protocol (close audio channel, reset protocol)
        App::GetInstance().ResetProtocol();

        xTaskCreate([](void* arg) {
            auto* board = static_cast<WifiBoard*>(arg);

            // Wait for 1 second to allow speaking to finish gracefully
            vTaskDelay(pdMS_TO_TICKS(1000));

            // Stop any ongoing connection attempt
            esp_timer_stop(board->connect_timer_);
            WifiManager::GetInstance().StopStation();

            // Enter config mode
            board->StartWifiConfigMode();

            vTaskDelete(NULL);
        }, "wifi_cfg_delay", 4096, this, 2, NULL);
        return;
    }

    if (state != kDeviceStateStarting) {
        ESP_LOGE(TAG, "EnterWifiConfigMode called but device state is not starting or speaking, device state: %d", state);
        return;
    }

    // Starting 态（尚未连上 WiFi，无说话需要等待、无音频通道需要重置）：同因投递，
    // 保持与原来一致的语义（不额外延迟、不动 protocol），只是把重活移出 esp_timer 任务。
    xTaskCreate([](void* arg) {
        auto* board = static_cast<WifiBoard*>(arg);

        // Stop any ongoing connection attempt
        esp_timer_stop(board->connect_timer_);
        WifiManager::GetInstance().StopStation();

        // Enter config mode
        board->StartWifiConfigMode();

        vTaskDelete(NULL);
    }, "wifi_cfg_start", 4096, this, 2, NULL);
}

bool WifiBoard::IsInWifiConfigMode() const {
    return WifiManager::GetInstance().IsConfigMode();
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return FONT_AWESOME_WIFI;
    }
    if (!wifi.IsConnected()) {
        return FONT_AWESOME_WIFI_SLASH;
    }

    int rssi = wifi.GetRssi();
    if (rssi >= -65) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -75) {
        return FONT_AWESOME_WIFI_FAIR;
    }
    return FONT_AWESOME_WIFI_WEAK;
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi = WifiManager::GetInstance();
    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";

    if (!wifi.IsConfigMode()) {
        json += R"("ssid":")" + wifi.GetSsid() + R"(",)";
        json += R"("rssi":)" + std::to_string(wifi.GetRssi()) + R"(,)";
        json += R"("channel":)" + std::to_string(wifi.GetChannel()) + R"(,)";
        json += R"("ip":")" + wifi.GetIpAddress() + R"(",)";
    }

    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"("})";
    return json;
}

void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    WifiPowerSaveLevel wifi_level;
    switch (level) {
        case PowerSaveLevel::LOW_POWER:
            wifi_level = WifiPowerSaveLevel::LOW_POWER;
            break;
        case PowerSaveLevel::BALANCED:
            wifi_level = WifiPowerSaveLevel::BALANCED;
            break;
        case PowerSaveLevel::PERFORMANCE:
        default:
            wifi_level = WifiPowerSaveLevel::PERFORMANCE;
            break;
    }
    WifiManager::GetInstance().SetPowerSaveLevel(wifi_level);
}

std::string WifiBoard::GetDeviceStatusJson() {
    // 公共骨架（audio_speaker/screen/battery）由基类统一实现，此处只填本板 network 段
    cJSON* root = BuildCommonDeviceStatus();

    // Network
    auto& wifi = WifiManager::GetInstance();
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi.GetSsid().c_str());
    int rssi = wifi.GetRssi();
    const char* signal = rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    cJSON_AddStringToObject(network, "signal", signal);
    cJSON_AddItemToObject(root, "network", network);

    // Chip temperature
    float temp = 0.0f;
    if (GetTemperature(temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    return DeviceStatusJsonToString(root);
}

void WifiBoard::StartSntpSync() {
    if (sntp_started_) {
        return;
    }
    sntp_started_ = true;

    // 设置中国时区 CST-8
    setenv("TZ", "CST-8", 1);
    tzset();

    // 启动 SNTP 后台同步（异步，不阻塞）
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "ntp.aliyun.com");
    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP time sync started (CST-8, pool.ntp.org + ntp.aliyun.com)");
}
