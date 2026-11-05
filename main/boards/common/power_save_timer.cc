#include "power_save_timer.h"
#include "app.h"
#include "settings.h"

#include <esp_log.h>

#define TAG "PowerSaveTimer"


PowerSaveTimer::PowerSaveTimer(int cpu_max_freq, int seconds_to_sleep, int seconds_to_shutdown)
    : cpu_max_freq_(cpu_max_freq), seconds_to_sleep_(seconds_to_sleep), seconds_to_shutdown_(seconds_to_shutdown) {
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto self = static_cast<PowerSaveTimer*>(arg);
            self->PowerSaveCheck();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "power_save_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &power_save_timer_));
}

PowerSaveTimer::~PowerSaveTimer() {
    esp_timer_stop(power_save_timer_);
    esp_timer_delete(power_save_timer_);
}

void PowerSaveTimer::SetEnabled(bool enabled) {
    if (enabled && !enabled_) {
        Settings settings("wifi", false);
        if (!settings.GetBool("sleep_mode", true)) {
            ESP_LOGI(TAG, "Power save timer is disabled by settings");
            return;
        }

        ticks_ = 0;
        enabled_ = enabled;
        ESP_ERROR_CHECK(esp_timer_start_periodic(power_save_timer_, 1000000));
        ESP_LOGI(TAG, "Power save timer enabled");
    } else if (!enabled && enabled_) {
        ESP_ERROR_CHECK(esp_timer_stop(power_save_timer_));
        enabled_ = enabled;
        WakeUp();
        ESP_LOGI(TAG, "Power save timer disabled");
    }
}

void PowerSaveTimer::OnEnterSleepMode(std::function<void()> callback) {
    on_enter_sleep_mode_ = callback;
}

void PowerSaveTimer::OnExitSleepMode(std::function<void()> callback) {
    on_exit_sleep_mode_ = callback;
}

void PowerSaveTimer::OnShutdownRequest(std::function<void()> callback) {
    on_shutdown_request_ = callback;
}

void PowerSaveTimer::PowerSaveCheck() {
    auto& app = App::GetInstance();
    if (!in_sleep_mode_ && !app.CanEnterSleepMode()) {
        ticks_ = 0;
        return;
    }

    ticks_++;
    // ★ 2026-09-18 根修（esp_timer 任务禁止阻塞）：进入省电模式原本在本回调里同步做
    //   关唤醒词（可能等任务收尾）、codec->EnableInput(false)（I2S disable 阻塞）、
    //   esp_pm_configure，且中间还有 vTaskDelay(100ms) —— 全部是阻塞操作。本回调与
    //   App 的 200ms 时钟 tick 共用 esp_timer 任务，一旦阻塞，主循环立即停摆（整机卡死）。
    //   现只置位并投递独立任务执行重活。
    if (seconds_to_sleep_ != -1 && ticks_ >= seconds_to_sleep_ && !in_sleep_mode_) {
        in_sleep_mode_ = true;
        xTaskCreate([](void* arg) {
            static_cast<PowerSaveTimer*>(arg)->EnterSleepModeWork();
            vTaskDelete(NULL);
        }, "pm_enter_sleep", 4096, this, 3, nullptr);
    }
    if (seconds_to_shutdown_ != -1 && ticks_ >= seconds_to_shutdown_
        && on_shutdown_request_ && !shutdown_started_) {
        shutdown_started_ = true;   // 关机请求同样不在 esp_timer 上下文执行
        xTaskCreate([](void* arg) {
            static_cast<PowerSaveTimer*>(arg)->ShutdownWork();
            vTaskDelete(NULL);
        }, "pm_shutdown", 4096, this, 3, nullptr);
    }
}

// 进入省电模式的实际动作（独立任务上下文）
void PowerSaveTimer::EnterSleepModeWork() {
    auto& app = App::GetInstance();
    ESP_LOGI(TAG, "Enabling power save mode");
    if (on_enter_sleep_mode_) {
        on_enter_sleep_mode_();
    }

    if (cpu_max_freq_ != -1) {
        // Disable wake word detection
        auto& audio_service = app.GetAudioService();
        is_wake_word_running_ = audio_service.IsWakeWordRunning();
        if (is_wake_word_running_) {
            audio_service.EnableWakeWordDetection(false);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        // Disable audio input
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec) {
            codec->EnableInput(false);
        }

        esp_pm_config_t pm_config = {
            .max_freq_mhz = cpu_max_freq_,
            .min_freq_mhz = 40,
            .light_sleep_enable = true,
        };
        esp_pm_configure(&pm_config);
    }
}

// 关机请求（独立任务上下文）
void PowerSaveTimer::ShutdownWork() {
    if (on_shutdown_request_) {
        on_shutdown_request_();
    }
}

void PowerSaveTimer::WakeUp() {
    ticks_ = 0;
    if (in_sleep_mode_) {
        ESP_LOGI(TAG, "Exiting power save mode");
        in_sleep_mode_ = false;

        if (cpu_max_freq_ != -1) {
            esp_pm_config_t pm_config = {
                .max_freq_mhz = cpu_max_freq_,
                .min_freq_mhz = cpu_max_freq_,
                .light_sleep_enable = false,
            };
            esp_pm_configure(&pm_config);

            // Enable wake word detection
            auto& app = App::GetInstance();
            auto& audio_service = app.GetAudioService();
            if (is_wake_word_running_) {
                audio_service.EnableWakeWordDetection(true);
            }
        }

        if (on_exit_sleep_mode_) {
            on_exit_sleep_mode_();
        }
    }
}
