#ifndef _SINGLE_LED_H_
#define _SINGLE_LED_H_

#include "led.h"
#include <driver/gpio.h>
#include <led_strip.h>
#include <esp_timer.h>
#include <atomic>
#include <mutex>

class SingleLed : public Led {
public:
    SingleLed(gpio_num_t gpio);
    virtual ~SingleLed();

    void OnStateChanged() override;

private:
    std::mutex mutex_;
    // ★ 2026-09-18：LED 刷新改由本任务执行（原为 esp_timer 回调，见 .cc 顶部注释）
    TaskHandle_t blink_task_ = nullptr;
    led_strip_handle_t led_strip_ = nullptr;
    uint8_t r_ = 0, g_ = 0, b_ = 0;
    // 闪烁参数（调用方与闪烁任务共享，原子访问）
    std::atomic<int> blink_times_{0};        // <0 = 无限闪烁
    std::atomic<int> blink_interval_ms_{0};
    std::atomic<bool> blink_active_{false};

    void StartBlinkTask(int times, int interval_ms);
    void BlinkTask();

    void BlinkOnce();
    void Blink(int times, int interval_ms);
    void StartContinuousBlink(int interval_ms);
    void TurnOn();
    void TurnOff();
    void SetColor(uint8_t r, uint8_t g, uint8_t b);
};

#endif // _SINGLE_LED_H_
