#include <esp_log.h>
#include <esp_err.h>
#include <esp_app_desc.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app.h"
#include "system_info.h"

#define TAG "main"

extern "C" void app_main(void)
{
    auto app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "  NiceAI 固件启动");
    ESP_LOGI(TAG, "  版本: %s", app_desc->version);
    ESP_LOGI(TAG, "  芯片: %s", SystemInfo::GetChipModelName().c_str());
    ESP_LOGI(TAG, "  编译时间: %s %s", app_desc->date, app_desc->time);
    ESP_LOGI(TAG, "====================================");

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS闪存已损坏，正在擦除以修复");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize and run the application
    auto& app = App::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}
