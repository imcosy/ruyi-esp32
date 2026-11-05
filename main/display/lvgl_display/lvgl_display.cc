#include <esp_log.h>
#include <esp_err.h>
#include <string>
#include <cstdlib>
#include <cstring>
#include <font_awesome.h>

#include "lvgl_display.h"
#include "board.h"
#include "app.h"
#include "audio_codec.h"
#include "settings.h"
#include "audio/assets/lang_config.h"
#include "jpg/image_to_jpeg.h"

#define TAG "Display"

LvglDisplay::LvglDisplay() {
    // ★ 2026-09-18 根修：通知条隐藏不再用 esp_timer 回调（回调里取 LVGL 锁会把
    //   esp_timer 任务连同 App 的 200ms 时钟 tick 一起冻死 → 主循环永不苏醒）。
    //   改为到期时间戳，由 UpdateStatusBar()（主循环 tick 驱动）在锁内顺手隐藏。

    // Create a power management lock
    auto ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "display_update", &pm_lock_);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "Power management not supported");
    } else {
        ESP_ERROR_CHECK(ret);
    }
}

LvglDisplay::~LvglDisplay() {
    if (network_label_ != nullptr) {
        lv_obj_del(network_label_);
    }
    if (notification_label_ != nullptr) {
        lv_obj_del(notification_label_);
    }
    if (status_label_ != nullptr) {
        lv_obj_del(status_label_);
    }
    if (mute_label_ != nullptr) {
        lv_obj_del(mute_label_);
    }
    if (battery_label_ != nullptr) {
        lv_obj_del(battery_label_);
    }
    if( low_battery_popup_ != nullptr ) {
        lv_obj_del(low_battery_popup_);
    }
    if (pm_lock_ != nullptr) {
        esp_pm_lock_delete(pm_lock_);
    }
}

void LvglDisplay::SetStatus(const char* status) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetStatus('%s') called before SetupUI() - message will be lost!", status);
    }
    DisplayLockGuard lock(this);
    if (!lock.locked()) {
        return;
    }
    if (status_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetStatus('%s') failed: status_label_ is nullptr (SetupUI() was called but label not created)", status);
        }
        return;
    }
    lv_label_set_text(status_label_, status);
    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    last_status_update_time_ = std::chrono::system_clock::now();
}

void LvglDisplay::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void LvglDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "ShowNotification('%s') called before SetupUI() - message will be lost!", notification);
    }
    DisplayLockGuard lock(this);
    if (!lock.locked() || notification_label_ == nullptr) {
        return;
    }
    lv_label_set_text(notification_label_, notification);
    lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);

    // 到期时刻交由主循环 tick（UpdateStatusBar）检查并隐藏
    notification_hide_at_us_.store(esp_timer_get_time() + (int64_t)duration_ms * 1000);
}

void LvglDisplay::UpdateStatusBar(bool update_all) {
    auto& app = App::GetInstance();
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();

    // ★ 通知条到期隐藏（原 notification_timer 定时器的职责，改为 tick 驱动）：
    //   本函数由 App 主循环每 200ms 调用，在 LVGL 锁内顺手完成，零额外定时器。
    int64_t notify_deadline_us = notification_hide_at_us_.load();
    if (notify_deadline_us != 0 && esp_timer_get_time() >= notify_deadline_us) {
        notification_hide_at_us_.store(0);
        DisplayLockGuard lock(this);
        if (lock.locked() && notification_label_ != nullptr && status_label_ != nullptr) {
            lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Update mute icon
    {
        DisplayLockGuard lock(this);
        if (!lock.locked() || mute_label_ == nullptr) {
            return;
        }

        // Update icon if mute state changes
        if (codec->output_volume() == 0 && !muted_) {
            muted_ = true;
            lv_label_set_text(mute_label_, FONT_AWESOME_VOLUME_XMARK);
        } else if (codec->output_volume() > 0 && muted_) {
            muted_ = false;
            lv_label_set_text(mute_label_, "");
        }
    }

    // Update time
    if (app.GetDeviceState() == kDeviceStateIdle) {
        if (last_status_update_time_ + std::chrono::seconds(10) < std::chrono::system_clock::now()) {
            // Set status to clock "HH:MM"
            time_t now = time(NULL);
            struct tm tm_now;
            localtime_r(&now, &tm_now);   // ★ 线程安全版本（原 localtime 为非线程安全 API，多任务环境下不可用）
            // Check if the we have already set the time
            if (tm_now.tm_year >= 2025 - 1900) {
                char time_str[16];
                strftime(time_str, sizeof(time_str), "%H:%M", &tm_now);
                SetStatus(time_str);
                time_warning_logged_ = false;
            } else if (!time_warning_logged_) {
                ESP_LOGW(TAG, "System time is not set, tm_year: %d", tm_now.tm_year);
                time_warning_logged_ = true;
            }
        }
    }

    esp_pm_lock_acquire(pm_lock_);
    // Update battery icon
    int battery_level;
    bool charging, discharging;
    const char* icon = nullptr;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        if (charging) {
            icon = FONT_AWESOME_BATTERY_BOLT;
        } else {
            const char* levels[] = {
                FONT_AWESOME_BATTERY_EMPTY, // 0-19%
                FONT_AWESOME_BATTERY_QUARTER,    // 20-39%
                FONT_AWESOME_BATTERY_HALF,    // 40-59%
                FONT_AWESOME_BATTERY_THREE_QUARTERS,    // 60-79%
                FONT_AWESOME_BATTERY_FULL, // 80-99%
                FONT_AWESOME_BATTERY_FULL, // 100%
            };
            icon = levels[battery_level / 20];
        }
        DisplayLockGuard lock(this);
        if (lock.locked()) {
            if (battery_label_ != nullptr && battery_icon_ != icon) {
                battery_icon_ = icon;
                lv_label_set_text(battery_label_, battery_icon_);
            }

            // Check low battery popup only when clock tick event is triggered
            // Because when initializing, the battery level is not ready yet.
            if (low_battery_popup_ != nullptr && !update_all) {
                if (strcmp(icon, FONT_AWESOME_BATTERY_EMPTY) == 0 && discharging) {
                    if (lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) { // Show if low battery popup is hidden
                        lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                        app.Schedule([&app]() {
                            app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
                        });
                    }
                } else {
                    // Hide the low battery popup when the battery is not empty
                    if (!lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) { // Hide if low battery popup is shown
                        lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                    }
                }
            }
        }
    }

    // Update network icon every 10 seconds（真实时间门控：tick 为 200ms，
    // 4G 模组的 GetNetworkStateIcon 会走串口 AT 查询，必须低频）
    static int64_t s_last_network_icon_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (update_all || now_us - s_last_network_icon_us >= 10 * 1000 * 1000) {
        s_last_network_icon_us = now_us;
        // Don't read 4G network status during firmware upgrade to avoid occupying UART resources
        auto device_state = App::GetInstance().GetDeviceState();
        static const std::vector<DeviceState> allowed_states = {
            kDeviceStateIdle,
            kDeviceStateStarting,
            kDeviceStateWifiConfiguring,
            kDeviceStateListening,
            kDeviceStateConnecting,
        };
        if (std::find(allowed_states.begin(), allowed_states.end(), device_state) != allowed_states.end()) {
            icon = board.GetNetworkStateIcon();
            if (network_label_ != nullptr && icon != nullptr && network_icon_ != icon) {
                DisplayLockGuard lock(this);
                if (lock.locked()) {
                    network_icon_ = icon;
                    lv_label_set_text(network_label_, network_icon_);
                }
            }
        }
    }

    esp_pm_lock_release(pm_lock_);
}

void LvglDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
}

// 省电模式下仅清空聊天区（背光与刷新由板级 PowerSaveTimer 控制）
void LvglDisplay::SetPowerSaveMode(bool on) {
    (void)on;
    SetChatMessage("system", "");
}

bool LvglDisplay::SnapshotToJpeg(std::string& jpeg_data, int quality) {
#if CONFIG_LV_USE_SNAPSHOT
    DisplayLockGuard lock(this);
    if (!lock.locked()) {
        return false;
    }

    lv_obj_t* screen = lv_screen_active();
    lv_draw_buf_t* draw_buffer = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB565);
    if (draw_buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to take snapshot, draw_buffer is nullptr");
        return false;
    }

    // swap bytes
    uint16_t* data = (uint16_t*)draw_buffer->data;
    size_t pixel_count = draw_buffer->data_size / 2;
    for (size_t i = 0; i < pixel_count; i++) {
        data[i] = __builtin_bswap16(data[i]);
    }

    // Clear output string and use callback version to avoid pre-allocating large memory blocks
    jpeg_data.clear();

    // Use callback-based JPEG encoder to further save memory
    bool ret = image_to_jpeg_cb((uint8_t*)draw_buffer->data, draw_buffer->data_size, draw_buffer->header.w, draw_buffer->header.h, V4L2_PIX_FMT_RGB565, quality,
        [](void *arg, size_t index, const void *data, size_t len) -> size_t {
        std::string* output = static_cast<std::string*>(arg);
        if (data && len > 0) {
            output->append(static_cast<const char*>(data), len);
        }
        return len;
    }, &jpeg_data);
    if (!ret) {
        ESP_LOGE(TAG, "Failed to convert image to JPEG");
    }

    lv_draw_buf_destroy(draw_buffer);
    return ret;
#else
    ESP_LOGE(TAG, "LV_USE_SNAPSHOT is not enabled");
    return false;
#endif
}
