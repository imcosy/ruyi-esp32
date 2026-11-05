#include "single_led.h"
#include "app.h"
#include <esp_log.h> 

#define TAG "SingleLed"

#define DEFAULT_BRIGHTNESS 4
#define HIGH_BRIGHTNESS 16
#define LOW_BRIGHTNESS 2

#define BLINK_INFINITE -1


SingleLed::SingleLed(gpio_num_t gpio) {
    if (gpio == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "SingleLed initialized with GPIO_NUM_NC, LED will not function");
        return;
    }

    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = gpio;
    strip_config.max_leds = 1;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.led_model = LED_MODEL_WS2812;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000; // 10MHz

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_));
    led_strip_clear(led_strip_);

    // ★ 2026-09-18 根修（esp_timer 任务禁止阻塞）：
//   闪烁刷新原在 esp_timer 回调里执行，而 led_strip_refresh() 走 RMT，组件内部是
//   rmt_tx_wait_all_done(chan, -1) —— 无限等待，且回调全程持本类 mutex_。esp_timer
//   任务一旦在此挂住，App 的 200ms 时钟 tick（同一任务）随之停摆，主循环永久阻塞在
//   xEventGroupWaitBits（整机卡死：无心跳/无重连/卡聆听中）；同时 TurnOn/TurnOff
//   （主循环调用）抢同一把锁，形成双向互堵。改由本类自己的任务驱动闪烁：RMT 阻塞
//   只影响本任务，时钟与主循环彻底解耦。
    xTaskCreate([](void* arg) {
        auto led = static_cast<SingleLed*>(arg);
        led->BlinkTask();
        vTaskDelete(NULL);
    }, "led_blink", 2048 * 2, this, 3, &blink_task_);
}

SingleLed::~SingleLed() {
    blink_active_.store(false);
    if (blink_task_ != nullptr) {
        vTaskDelete(blink_task_);
        blink_task_ = nullptr;
    }
    if (led_strip_ != nullptr) {
        led_strip_del(led_strip_);
    }
}


void SingleLed::SetColor(uint8_t r, uint8_t g, uint8_t b) {
    r_ = r;
    g_ = g;
    b_ = b;
}

void SingleLed::TurnOn() {
    if (led_strip_ == nullptr) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    blink_active_.store(false);   // 停止闪烁任务（原 esp_timer_stop）
    led_strip_set_pixel(led_strip_, 0, r_, g_, b_);
    led_strip_refresh(led_strip_);
}

void SingleLed::TurnOff() {
    if (led_strip_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    blink_active_.store(false);   // 停止闪烁任务（原 esp_timer_stop）
    led_strip_clear(led_strip_);
}

void SingleLed::BlinkOnce() {
    Blink(1, 100);
}

void SingleLed::Blink(int times, int interval_ms) {
    StartBlinkTask(times, interval_ms);
}

void SingleLed::StartContinuousBlink(int interval_ms) {
    StartBlinkTask(BLINK_INFINITE, interval_ms);
}

void SingleLed::StartBlinkTask(int times, int interval_ms) {
    if (led_strip_ == nullptr || blink_task_ == nullptr) {
        return;
    }
    if (times == 0) {
        return;   // ★ 修复：原实现 times=0 时计数为 0，结束判定永不成立 → 常亮不灭
    }
    blink_times_.store(times);
    blink_interval_ms_.store(interval_ms);
    blink_active_.store(true);
    xTaskNotifyGive(blink_task_);   // 唤醒闪烁任务开始新一轮
}

// 闪烁任务（原 OnBlinkTimer 回调改造）：亮/灭交替 times 轮后自动结束；
// BLINK_INFINITE(<0) 则持续交替直到 TurnOn/TurnOff/新 StartBlinkTask 打断。
void SingleLed::BlinkTask() {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const bool infinite = blink_times_.load() < 0;
        int steps = infinite ? 0 : blink_times_.load() * 2;   // 亮灭各一步
        bool on = true;
        while (blink_active_.load() && (infinite || steps > 0)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (on) {
                    led_strip_set_pixel(led_strip_, 0, r_, g_, b_);
                } else {
                    led_strip_clear(led_strip_);
                }
            }
            on = !on;
            if (!infinite) {
                steps--;
            }
            int interval_ms = blink_interval_ms_.load();
            vTaskDelay(pdMS_TO_TICKS(interval_ms > 0 ? interval_ms : 100));
        }
    }
}


void SingleLed::OnStateChanged() {
    auto& app = App::GetInstance();
    auto device_state = app.GetDeviceState();
    switch (device_state) {
        case kDeviceStateStarting:
            SetColor(0, 0, DEFAULT_BRIGHTNESS);
            StartContinuousBlink(100);
            break;
        case kDeviceStateWifiConfiguring:
            SetColor(0, 0, DEFAULT_BRIGHTNESS);
            StartContinuousBlink(500);
            break;
        case kDeviceStateIdle:
            TurnOff();
            break;
        case kDeviceStateListening:
            if (app.IsVoiceDetected()) {
                SetColor(HIGH_BRIGHTNESS, 0, 0);
            } else {
                SetColor(LOW_BRIGHTNESS, 0, 0);
            }
            TurnOn();
            break;
        case kDeviceStateSpeaking:
            SetColor(0, DEFAULT_BRIGHTNESS, 0);
            TurnOn();
            break;
        case kDeviceStateUpgrading:
            SetColor(0, DEFAULT_BRIGHTNESS, 0);
            StartContinuousBlink(100);
            break;
        case kDeviceStateConnecting:
            SetColor(0, DEFAULT_BRIGHTNESS, 0);
            StartContinuousBlink(500);
            break;
        default:
            ESP_LOGW(TAG, "Unknown led strip event: %d", device_state);
            return;
    }
}
