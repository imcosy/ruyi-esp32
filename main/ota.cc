#include "ota.h"
#include "system_info.h"
#include "settings.h"
#include "audio/assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_crt_bundle.h>
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include <cstring>
#include <vector>
#include <sstream>
#include <algorithm>

#define TAG "Ota"

// Default OTA URL - sisi server
#ifndef CONFIG_OTA_URL
#define CONFIG_OTA_URL "https://nice-ai.cn/ota/"
#endif

// OTA 版本检查地址（静态 version.json）
// ★ 2026-09-12 根因修复：OTA 源按板型隔离。教训：此前用 #ifndef CONFIG_OTA_VERSION_URL
//   包裹板型分支，但该宏在 Kconfig（sdkconfig 固化为 sisi URL）中已有定义，
//   #ifndef 永远为假 → 板型隔离从未生效，chatbox 板一直检查 sisi 的 version.json
//   （拿到 sisi 包后靠板型校验兜底拒绝——能防错刷但收不到 chatbox 自己的新版本）。
//   现改为编译期常量直接按板型选择，绕开 Kconfig 固化值：
//   - sisi/standar → /download/esp32/version.json（board 字段维护，板型校验兜底）
//   - meeting → /download/esp32/meeting/version.json（独立通道，2026-09-14 补：
//     0912 修复时 meeting 被归入主源，但主 version.json 只维护 sisi 板，
//     meeting 设备拿到 sisi 包被板型校验拒绝 → 永远收不到自己的更新）
//   - chatbox → /download/esp32/chatbox/version.json（手动烧录板独立源；平时不存在
//     404 自动跳过，要 OTA 时放 version.json 即可，铁律同 sisi：bin 先到位最后更新）
#if CONFIG_CHATBOX_BOARD
static const char* const kOtaVersionUrl = "https://dev.nice-ai.cn/download/esp32/chatbox/version.json";
#elif CONFIG_MEETING_BOARD
static const char* const kOtaVersionUrl = "https://dev.nice-ai.cn/download/esp32/meeting/version.json";
#elif CONFIG_STICK_S3_BOARD
// stick-s3 → /download/esp32/stick/version.json（独立通道，2026-09-15 新增板型：
//   M5Stack StickS3 与 sisi/meeting 板硬件差异大（ES8311 单 codec+PM1 电源+135×240 屏），
//   固件不可互刷，必须独立 version.json；发布目录 download/esp32/stick/，铁律同前：
//   bin 先到位，最后才更新 version.json）
static const char* const kOtaVersionUrl = "https://dev.nice-ai.cn/download/esp32/stick/version.json";
#elif CONFIG_ANAN_BOARD
// anan → /download/esp32/anan/version.json（独立通道，2026-09-16 新增板型：
//   面包板单麦克风+2.0寸ST7789 SPI 屏，与 sisi 板引脚完全不同，固件不可互刷；
//   手动烧录板，同 chatbox 模式：version.json 平时可放最新 bin 供下载烧录）
static const char* const kOtaVersionUrl = "https://dev.nice-ai.cn/download/esp32/anan/version.json";
#else
static const char* const kOtaVersionUrl = "https://dev.nice-ai.cn/download/esp32/version.json";
#endif


Ota::Ota() {
    // Initialize current version from app description
    auto app_desc = esp_app_get_description();
    if (app_desc != nullptr) {
        current_version_ = app_desc->version;
    }
    
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    // Read Serial Number from efuse user_data
    uint8_t serial_number[33] = {0};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK) {
        if (serial_number[0] == 0) {
            has_serial_number_ = false;
        } else {
            serial_number_ = std::string(reinterpret_cast<char*>(serial_number), 32);
            has_serial_number_ = true;
        }
    }
#endif
}

Ota::~Ota() {
}

std::string Ota::GetCheckVersionUrl() {
    // 按板型隔离的版本检查源（见文件头 kOtaVersionUrl 注释）
    return kOtaVersionUrl;
}

std::unique_ptr<Http> Ota::SetupHttp() {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    auto user_agent = SystemInfo::GetUserAgent();
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Client-Id", board.GetUuid());
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_.c_str());
        ESP_LOGI(TAG, "Setup HTTP, User-Agent: %s, Serial-Number: %s", user_agent.c_str(), serial_number_.c_str());
    }
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept-Language", Lang::CODE);
    http->SetHeader("Content-Type", "application/json");

    return http;
}

/* 
 * Specification: https://ccnphfhqs21z.feishu.cn/wiki/FjW6wZmisimNBBkov6OcmfvknVd
 */
esp_err_t Ota::CheckVersion() {
    auto app_desc = esp_app_get_description();

    // Check if there is a new firmware version available
    current_version_ = app_desc->version;
    ESP_LOGI(TAG, "Current version: %s", current_version_.c_str());

    std::string url = GetCheckVersionUrl();
    if (url.length() < 10) {
        ESP_LOGE(TAG, "Check version URL is not properly set");
        return ESP_ERR_INVALID_ARG;
    }

    auto http = SetupHttp();
    std::string data;  // 用于存放 HTTP 响应体

    // S2S 时代：OTA 版本检查走 HTTP GET 静态 version.json，不再 POST 系统信息
    if (!http->Open("GET", url)) {
        int last_error = http->GetLastError();
        ESP_LOGE(TAG, "Failed to open HTTP connection, code=0x%x", last_error);
        return last_error;
    }

    auto status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to check version, status code: %d", status_code);
        return status_code;
    }

    data = http->ReadAll();
    http->Close();

    // Response: { "firmware": { "version": "1.0.0", "url": "http://" } }
    // Parse the JSON response and check if the version is newer
    // If it is, set has_new_version_ to true and store the new version and URL
    
    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    has_activation_code_ = false;
    has_activation_challenge_ = false;
    cJSON *activation = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(activation)) {
        cJSON* message = cJSON_GetObjectItem(activation, "message");
        if (cJSON_IsString(message)) {
            activation_message_ = message->valuestring;
        }
        cJSON* code = cJSON_GetObjectItem(activation, "code");
        if (cJSON_IsString(code)) {
            activation_code_ = code->valuestring;
            has_activation_code_ = true;
        }
        cJSON* challenge = cJSON_GetObjectItem(activation, "challenge");
        if (cJSON_IsString(challenge)) {
            activation_challenge_ = challenge->valuestring;
            has_activation_challenge_ = true;
        }
        cJSON* timeout_ms = cJSON_GetObjectItem(activation, "timeout_ms");
        if (cJSON_IsNumber(timeout_ms)) {
            activation_timeout_ms_ = timeout_ms->valueint;
        }
    }

    has_mqtt_config_ = false;
    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        Settings settings("mqtt", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, mqtt) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_mqtt_config_ = true;
    }

    has_websocket_config_ = false;
    cJSON *websocket = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(websocket)) {
        Settings settings("websocket", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, websocket) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_websocket_config_ = true;
    } else {
        ESP_LOGI(TAG, "No websocket section found!");
    }

    has_server_time_ = false;
    cJSON *server_time = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(server_time)) {
        cJSON *timestamp = cJSON_GetObjectItem(server_time, "timestamp");
        cJSON *timezone_offset = cJSON_GetObjectItem(server_time, "timezone_offset");
        
        if (cJSON_IsNumber(timestamp)) {
            // 设置系统时间
            struct timeval tv;
            double ts = timestamp->valuedouble;
            
            // 如果有时区偏移，计算本地时间
            if (cJSON_IsNumber(timezone_offset)) {
                ts += (timezone_offset->valueint * 60 * 1000); // 转换分钟为毫秒
            }
            
            tv.tv_sec = (time_t)(ts / 1000);  // 转换毫秒为秒
            tv.tv_usec = (suseconds_t)((long long)ts % 1000) * 1000;  // 剩余的毫秒转换为微秒
            settimeofday(&tv, NULL);
            has_server_time_ = true;
        }
    }

    has_new_version_ = false;
    cJSON *firmware = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware)) {
        cJSON *version = cJSON_GetObjectItem(firmware, "version");
        if (cJSON_IsString(version)) {
            firmware_version_ = version->valuestring;
        }
        cJSON *url = cJSON_GetObjectItem(firmware, "url");
        if (cJSON_IsString(url)) {
            firmware_url_ = url->valuestring;
        }

        // ★ 板型校验（2026-09-12，用户要求）：version.json 的 firmware 节必须声明
        //   board 且与本固件板型（BOARD_NAME：sisi/chatbox/meeting/standar）一致才
        //   允许升级——各板型引脚配置不同（如 chatbox MCLK=48/WS=20，sisi MCLK=15/
        //   WS=13），跨板型互刷直接导致音频/外设失效。双保险机制：
        //   ① URL 隔离（chatbox 走 /chatbox/version.json）② 板型身份硬校验（本处）
        //   meeting/standar 板即使 URL 指到 sisi 的 version.json，board 不匹配也拒绝。
        //   发布纪律：各板型 version.json 必须带 firmware.board 字段。
        cJSON *board = cJSON_GetObjectItem(firmware, "board");
        if (!cJSON_IsString(board) || board->valuestring[0] == '\0') {
            ESP_LOGW(TAG, "OTA 服务端包未声明 board 字段，拒绝升级（version.json 需含 firmware.board=\"%s\"）", BOARD_NAME);
            firmware_version_.clear();
            firmware_url_.clear();
            cJSON_Delete(root);
            return ESP_OK;
        }
        if (strcmp(board->valuestring, BOARD_NAME) != 0) {
            ESP_LOGW(TAG, "OTA 板型不匹配: 本板=%s 服务端包=%s，拒绝升级", BOARD_NAME, board->valuestring);
            firmware_version_.clear();
            firmware_url_.clear();
            cJSON_Delete(root);
            return ESP_OK;
        }

        if (cJSON_IsString(version) && cJSON_IsString(url) && !firmware_url_.empty()) {
            has_new_version_ = true;
            ESP_LOGI(TAG, "Server firmware: %s (board=%s)", firmware_version_.c_str(), board->valuestring);
        }
        cJSON *force = cJSON_GetObjectItem(firmware, "force");
        if (cJSON_IsNumber(force) && force->valueint == 1) {
            has_new_version_ = true;
            ESP_LOGI(TAG, "Force upgrade requested by server");
        }
    } else {
        ESP_LOGW(TAG, "No firmware section found!");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    ESP_LOGI(TAG, "Running partition: %s", partition->label);
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get state of partition");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

static std::function<void(int progress, size_t speed)> g_ota_progress_callback;

static struct {
    size_t total_len;
    size_t content_length;
    size_t last_reported_len;
    int64_t last_report_time;
} g_ota_stats = {0, 0, 0, 0};

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ON_CONNECTED:
            g_ota_stats.content_length = 0;
            g_ota_stats.total_len = 0;
            g_ota_stats.last_reported_len = 0;
            g_ota_stats.last_report_time = esp_timer_get_time();
            break;
        case HTTP_EVENT_ON_HEADER:
            // 在收到响应头时获取Content-Length（此时才有值）
            if (evt->header_key && evt->header_value && strcasecmp(evt->header_key, "Content-Length") == 0) {
                g_ota_stats.content_length = atol(evt->header_value);
                ESP_LOGI(TAG, "OTA: 文件大小=%u bytes", g_ota_stats.content_length);
            }
            break;
        case HTTP_EVENT_ON_DATA:
            g_ota_stats.total_len += evt->data_len;
            if (g_ota_progress_callback) {
                auto now = esp_timer_get_time();
                auto elapsed = now - g_ota_stats.last_report_time;
                if (elapsed >= 500000) {
                    int progress = 0;
                    if (g_ota_stats.content_length > 0) {
                        progress = (int)(g_ota_stats.total_len * 100 / g_ota_stats.content_length);
                        if (progress > 100) progress = 100;
                        if (progress < 0) progress = 0;
                    }
                    size_t downloaded_since_last = g_ota_stats.total_len - g_ota_stats.last_reported_len;
                    size_t speed = elapsed > 0 ? (downloaded_since_last * 1000000 / elapsed) : 0;
                    g_ota_progress_callback(progress, speed);
                    g_ota_stats.last_reported_len = g_ota_stats.total_len;
                    g_ota_stats.last_report_time = now;
                }
            }
            break;
        case HTTP_EVENT_DISCONNECTED:
            memset(&g_ota_stats, 0, sizeof(g_ota_stats));
            break;
        default:
            break;
    }
    return ESP_OK;
}

bool Ota::Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback) {
    ESP_LOGI(TAG, "Upgrading firmware from %s", firmware_url.c_str());
    
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
        esp_ota_img_states_t ota_state;
        if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK && ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, "Current app in PENDING_VERIFY state, confirming before OTA...");
            esp_err_t confirm_ret = esp_ota_mark_app_valid_cancel_rollback();
            if (confirm_ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to confirm current app: %s", esp_err_to_name(confirm_ret));
            } else {
                ESP_LOGI(TAG, "Current app confirmed successfully");
            }
        }
    }
    
    g_ota_progress_callback = callback;
    
    // Use esp_https_ota which supports automatic redirect handling
    esp_http_client_config_t http_config = {
        .url = firmware_url.c_str(),
        .event_handler = http_event_handler,
        .skip_cert_common_name_check = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };
    
    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Firmware upgrade successful, marking as valid...");

        // 关键：标记新固件为有效，防止Bootloader回滚到老固件！
        // esp_https_ota写入后分区状态为PENDING_VERIFY，
        // 如果不调用此函数确认，Bootloader重启时会回滚到之前的固件
        const esp_partition_t* updated = esp_ota_get_next_update_partition(NULL);
        if (updated) {
            esp_err_t mark_ret = esp_ota_mark_app_valid_cancel_rollback();
            if (mark_ret == ESP_OK) {
                ESP_LOGI(TAG, "New firmware marked as valid (rollback cancelled)");
            } else {
                ESP_LOGE(TAG, "Failed to mark new firmware valid: %s", esp_err_to_name(mark_ret));
                // 即使标记失败也继续（总比回滚好）
            }
        }

        // 升级成功后，将pending_version移到current_version
        Settings ota_settings("ota", true);
        std::string pending = ota_settings.GetString("pending_version", "");
        if (!pending.empty()) {
            ota_settings.SetString("current_version", pending);
            ota_settings.EraseKey("pending_version");
            ESP_LOGI(TAG, "NVS已更新: current_version=%s, pending_version已清除", pending.c_str());
        }

        ESP_LOGI(TAG, "Rebooting...");
        return true;
    } else {
        ESP_LOGE(TAG, "Firmware upgrade failed: %s", esp_err_to_name(ret));
        return false;
    }
}

bool Ota::StartUpgrade(std::function<void(int progress, size_t speed)> callback) {
    return Upgrade(firmware_url_, callback);
}


std::vector<int> Ota::ParseVersion(const std::string& version) {
    std::vector<int> versionNumbers;
    std::stringstream ss(version);
    std::string segment;
    
    // ★ 2026-09-18：版本串可能含非数字段（如 "20260918.0052-rc"），原 std::stoi
//   会抛 std::invalid_argument —— 未捕获异常直接 abort 整机。改用 strtol：
//   非数字段按 0 计，解析永不抛异常。
    while (std::getline(ss, segment, '.')) {
        const char* start = segment.c_str();
        char* end = nullptr;
        long v = strtol(start, &end, 10);
        versionNumbers.push_back(end != start ? (int)v : 0);
    }
    
    return versionNumbers;
}

bool Ota::IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion) {
    std::vector<int> current = ParseVersion(currentVersion);
    std::vector<int> newer = ParseVersion(newVersion);
    
    for (size_t i = 0; i < std::min(current.size(), newer.size()); ++i) {
        if (newer[i] > current[i]) {
            return true;
        } else if (newer[i] < current[i]) {
            return false;
        }
    }
    
    return newer.size() > current.size();
}

std::string Ota::GetActivationPayload() {
    if (!has_serial_number_) {
        return "{}";
    }

    std::string hmac_hex;
#ifdef SOC_HMAC_SUPPORTED
    uint8_t hmac_result[32]; // SHA-256 输出为32字节
    
    // 使用Key0计算HMAC
    esp_err_t ret = esp_hmac_calculate(HMAC_KEY0, (uint8_t*)activation_challenge_.data(), activation_challenge_.size(), hmac_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HMAC calculation failed: %s", esp_err_to_name(ret));
        return "{}";
    }

    for (size_t i = 0; i < sizeof(hmac_result); i++) {
        char buffer[3];
        sprintf(buffer, "%02x", hmac_result[i]);
        hmac_hex += buffer;
    }
#endif

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "serial_number", serial_number_.c_str());
    cJSON_AddStringToObject(payload, "challenge", activation_challenge_.c_str());
    cJSON_AddStringToObject(payload, "hmac", hmac_hex.c_str());
    auto json_str = cJSON_PrintUnformatted(payload);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(payload);

    ESP_LOGI(TAG, "Activation payload: %s", json.c_str());
    return json;
}

esp_err_t Ota::Activate() {
    if (!has_activation_challenge_) {
        ESP_LOGW(TAG, "No activation challenge found");
        return ESP_FAIL;
    }

    std::string url = GetCheckVersionUrl();
    if (url.back() != '/') {
        url += "/activate";
    } else {
        url += "activate";
    }

    auto http = SetupHttp();

    std::string data = GetActivationPayload();
    http->SetContent(std::move(data));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return ESP_FAIL;
    }
    
    auto status_code = http->GetStatusCode();
    if (status_code == 202) {
        return ESP_ERR_TIMEOUT;
    }
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to activate, code: %d, body: %s", status_code, http->ReadAll().c_str());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Activation successful");
    return ESP_OK;
}
