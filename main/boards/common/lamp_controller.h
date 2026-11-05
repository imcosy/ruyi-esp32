#ifndef __LAMP_CONTROLLER_H__
#define __LAMP_CONTROLLER_H__

#include "tool.h"
#include <driver/gpio.h>
#include <esp_log.h>

class LampController {
private:
    bool power_ = false;
    gpio_num_t gpio_num_;

    static LampController* g_instance_;

    static bool HandleGetState(const ToolCommand& cmd, std::string& result) {
        if (!g_instance_) { result = "not initialized"; return false; }
        result = g_instance_->power_ ? "{\"power\":true}" : "{\"power\":false}";
        return true;
    }

    static bool HandleTurnOn(const ToolCommand& cmd, std::string& result) {
        if (!g_instance_) { result = "not initialized"; return false; }
        g_instance_->power_ = true;
        gpio_set_level(g_instance_->gpio_num_, 1);
        result = "on";
        return true;
    }

    static bool HandleTurnOff(const ToolCommand& cmd, std::string& result) {
        if (!g_instance_) { result = "not initialized"; return false; }
        g_instance_->power_ = false;
        gpio_set_level(g_instance_->gpio_num_, 0);
        result = "off";
        return true;
    }

public:
    LampController(gpio_num_t gpio_num) : gpio_num_(gpio_num) {
        if (gpio_num_ == GPIO_NUM_NC) {
            return;
        }

        gpio_config_t config = {
            .pin_bit_mask = (1ULL << gpio_num_),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&config));
        gpio_set_level(gpio_num_, 0);

        g_instance_ = this;
        auto& tool_mgr = ToolManager::GetInstance();
        tool_mgr.RegisterTool("lamp_get_state", HandleGetState);
        tool_mgr.RegisterTool("lamp_turn_on", HandleTurnOn);
        tool_mgr.RegisterTool("lamp_turn_off", HandleTurnOff);
    }
};

LampController* LampController::g_instance_ = nullptr;

#endif // __LAMP_CONTROLLER_H__
