#include "circular_strip.h"
#include "app.h"
#include <esp_log.h>
#include <algorithm>

#define TAG "CircularStrip"

#define BLINK_INFINITE -1

CircularStrip::CircularStrip(gpio_num_t gpio, uint16_t max_leds) : max_leds_(max_leds) {
    // If the gpio is not connected, you should use NoLed class
    assert(gpio != GPIO_NUM_NC);

    colors_.resize(max_leds_);

    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = gpio;
    strip_config.max_leds = max_leds_;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.led_model = LED_MODEL_WS2812;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000; // 10MHz

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_));
    led_strip_clear(led_strip_);

    // ★ 2026-09-18 根修（esp_timer 任务禁止阻塞）：动画帧原在 esp_timer 回调里刷新，
//   而 led_strip_refresh() 走 RMT（组件内部 rmt_tx_wait_all_done(chan, -1) 无限等待）
//   且回调全程持 mutex_——esp_timer 任务一旦挂住，同任务的 App 200ms 时钟 tick 停摆，
//   主循环永久阻塞（整机卡死），并与主循环的 SetAllColor 等抢锁互堵。
//   改由本类自己的任务驱动，硬件阻塞不再牵连同核时钟。
    xTaskCreate([](void* arg) {
        auto strip = static_cast<CircularStrip*>(arg);
        strip->StripTask();
        vTaskDelete(NULL);
    }, "led_strip", 2048 * 2, this, 3, &strip_task_);
}

CircularStrip::~CircularStrip() {
    strip_active_.store(false);
    if (strip_task_ != nullptr) {
        vTaskDelete(strip_task_);
        strip_task_ = nullptr;
    }
    if (led_strip_ != nullptr) {
        led_strip_del(led_strip_);
    }
}


void CircularStrip::SetAllColor(StripColor color) {
    std::lock_guard<std::mutex> lock(mutex_);
    strip_active_.store(false);
    for (int i = 0; i < max_leds_; i++) {
        colors_[i] = color;
        led_strip_set_pixel(led_strip_, i, color.red, color.green, color.blue);
    }
    led_strip_refresh(led_strip_);
}

void CircularStrip::SetSingleColor(uint8_t index, StripColor color) {
    std::lock_guard<std::mutex> lock(mutex_);
    strip_active_.store(false);
    colors_[index] = color;
    led_strip_set_pixel(led_strip_, index, color.red, color.green, color.blue);
    led_strip_refresh(led_strip_);
}

void CircularStrip::SetMultiColors(const std::vector<StripColor>& colors) {
    std::lock_guard<std::mutex> lock(mutex_);
    strip_active_.store(false);
    int count = std::min(max_leds_, static_cast<int>(colors.size()));
    for (int i = 0; i < count; i++) {
        colors_[i] = colors[i];
        led_strip_set_pixel(led_strip_, i, colors[i].red, colors[i].green, colors[i].blue);
    }
    led_strip_refresh(led_strip_);
}

void CircularStrip::Blink(StripColor color, int interval_ms) {
    for (int i = 0; i < max_leds_; i++) {
        colors_[i] = color;
    }
    StartStripTask(interval_ms, [this]() {
        static bool on = true;
        if (on) {
            for (int i = 0; i < max_leds_; i++) {
                led_strip_set_pixel(led_strip_, i, colors_[i].red, colors_[i].green, colors_[i].blue);
            }
            led_strip_refresh(led_strip_);
        } else {
            led_strip_clear(led_strip_);
        }
        on = !on;
    });
}

void CircularStrip::FadeOut(int interval_ms) {
    StartStripTask(interval_ms, [this]() {
        bool all_off = true;
        for (int i = 0; i < max_leds_; i++) {
            colors_[i].red /= 2;
            colors_[i].green /= 2;
            colors_[i].blue /= 2;
            if (colors_[i].red != 0 || colors_[i].green != 0 || colors_[i].blue != 0) {
                all_off = false;
            }
            led_strip_set_pixel(led_strip_, i, colors_[i].red, colors_[i].green, colors_[i].blue);
        }
        if (all_off) {
            led_strip_clear(led_strip_);
            strip_active_.store(false);
        } else {
            led_strip_refresh(led_strip_);
        }
    });
}

void CircularStrip::Breathe(StripColor low, StripColor high, int interval_ms) {
    StartStripTask(interval_ms, [this, low, high]() {
        static bool increase = true;
        static StripColor color = low;
        if (increase) {
            if (color.red < high.red) {
                color.red++;
            }
            if (color.green < high.green) {
                color.green++;
            }
            if (color.blue < high.blue) {
                color.blue++;
            }
            if (color.red == high.red && color.green == high.green && color.blue == high.blue) {
                increase = false;
            }
        } else {
            if (color.red > low.red) {
                color.red--;
            }
            if (color.green > low.green) {
                color.green--;
            }
            if (color.blue > low.blue) {
                color.blue--;
            }
            if (color.red == low.red && color.green == low.green && color.blue == low.blue) {
                increase = true;
            }
        }
        for (int i = 0; i < max_leds_; i++) {
            led_strip_set_pixel(led_strip_, i, color.red, color.green, color.blue);
        }
        led_strip_refresh(led_strip_);
    });
}

void CircularStrip::Scroll(StripColor low, StripColor high, int length, int interval_ms) {
    for (int i = 0; i < max_leds_; i++) {
        colors_[i] = low;
    }
    StartStripTask(interval_ms, [this, low, high, length]() {
        static int offset = 0;
        for (int i = 0; i < max_leds_; i++) {
            colors_[i] = low;
        }
        for (int j = 0; j < length; j++) {
            int i = (offset + j) % max_leds_;
            colors_[i] = high;
        }
        for (int i = 0; i < max_leds_; i++) {
            led_strip_set_pixel(led_strip_, i, colors_[i].red, colors_[i].green, colors_[i].blue);
        }
        led_strip_refresh(led_strip_);
        offset = (offset + 1) % max_leds_;
    });
}

void CircularStrip::StartStripTask(int interval_ms, std::function<void()> cb) {
    if (led_strip_ == nullptr || strip_task_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    strip_active_.store(false);   // 先停旧动画（原 esp_timer_stop）
    strip_callback_ = cb;
    strip_interval_ms_.store(interval_ms);
    strip_active_.store(true);
    xTaskNotifyGive(strip_task_);   // 唤醒动画任务开始新动画
}

// 动画任务（原 esp_timer 回调改造）：
//   每 interval_ms 调一次 strip_callback_ 刷新一帧；持 mutex_ 与主循环 SetAllColor 互斥；
//   回调内部置 strip_active_=false（如 FadeOut 结束）即自动退出本轮。
void CircularStrip::StripTask() {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (strip_active_.load()) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (strip_callback_) {
                    strip_callback_();
                }
            }
            int interval_ms = strip_interval_ms_.load();
            vTaskDelay(pdMS_TO_TICKS(interval_ms > 0 ? interval_ms : 50));
        }
    }
}

void CircularStrip::SetBrightness(uint8_t default_brightness, uint8_t low_brightness) {
    default_brightness_ = default_brightness;
    low_brightness_ = low_brightness;
    OnStateChanged();
}

void CircularStrip::OnStateChanged() {
    auto& app = App::GetInstance();
    auto device_state = app.GetDeviceState();
    switch (device_state) {
        case kDeviceStateStarting: {
            StripColor low = { 0, 0, 0 };
            StripColor high = { low_brightness_, low_brightness_, default_brightness_ };
            Scroll(low, high, 3, 100);
            break;
        }
        case kDeviceStateWifiConfiguring: {
            StripColor color = { low_brightness_, low_brightness_, default_brightness_ };
            Blink(color, 500);
            break;
        }
        case kDeviceStateIdle:
            FadeOut(50);
            break;
        case kDeviceStateListening: {
            StripColor color = { default_brightness_, low_brightness_, low_brightness_ };
            SetAllColor(color);
            break;
        }
        case kDeviceStateSpeaking: {
            StripColor color = { low_brightness_, default_brightness_, low_brightness_ };
            SetAllColor(color);
            break;
        }
        case kDeviceStateUpgrading: {
            StripColor color = { low_brightness_, default_brightness_, low_brightness_ };
            Blink(color, 100);
            break;
        }
        case kDeviceStateConnecting: {
            StripColor color = { low_brightness_, default_brightness_, low_brightness_ };
            Blink(color, 500);
            break;
        }
        default:
            ESP_LOGW(TAG, "Unknown led strip event: %d", device_state);
            return;
    }
}
