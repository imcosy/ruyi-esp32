#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "app.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>

#define TAG "ChatboxBoard"

class ChatboxBoard : public WifiBoard {
private:
    Button boot_button_;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;

    void InitializeI2C() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_MASTER_NUM,
            .sda_io_num = I2C_MASTER_SDA_GPIO,
            .scl_io_num = I2C_MASTER_SCL_GPIO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags = {
                .enable_internal_pullup = true,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
        ESP_LOGI(TAG, "I2C initialized: SDA=%d, SCL=%d", I2C_MASTER_SDA_GPIO, I2C_MASTER_SCL_GPIO);
        // ★ I2C 设备探针：声/录同时全挂时，先确认模组控制接口是否活着——
        //   都OK → 模组通电+I2C通，问题锁定时钟线(MCLK)；都ERR → 供电/SDA/SCL接线问题
        esp_err_t r8311 = i2c_master_probe(i2c_bus_, ES8311_I2C_ADDR >> 1, 100);
        esp_err_t r7210 = i2c_master_probe(i2c_bus_, ES7210_I2C_ADDR >> 1, 100);
        ESP_LOGW(TAG, "I2C-PROBE: ES8311(0x18)=%s, ES7210(0x41)=%s",
                 r8311 == ESP_OK ? "OK" : esp_err_to_name(r8311),
                 r7210 == ESP_OK ? "OK" : esp_err_to_name(r7210));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = App::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializePA() {
#ifdef PA_ENABLE_GPIO
        if (PA_ENABLE_GPIO != GPIO_NUM_NC) {
            gpio_reset_pin(PA_ENABLE_GPIO);
            gpio_set_direction(PA_ENABLE_GPIO, GPIO_MODE_OUTPUT);
            gpio_set_level(PA_ENABLE_GPIO, 0);
            ESP_LOGI(TAG, "PA pin configured on GPIO %d (disabled to save power)", PA_ENABLE_GPIO);
        }
#endif
    }

public:
    ChatboxBoard() :
        boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2C();
        InitializePA();
        InitializeButtons();
    }

    virtual Led* GetLed() override {
#ifdef BUILTIN_LED_GPIO
        if (BUILTIN_LED_GPIO == GPIO_NUM_NC) {
            static NoLed led;
            return &led;
        }
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
#else
        static NoLed led;
        return &led;
#endif
    }

    virtual AudioCodec* GetAudioCodec() override {
        ESP_LOGI(TAG, "Audio pins: MCLK=%d BCLK=%d WS=%d DOUT=%d DIN=%d",
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
        
        // BoxAudioCodec: ES7210(输入) + ES8311(输出)
        // input_reference=true 启用 AEC 回声消除参考输入
        // 注意: Duplex 模式要求输入输出采样率相同
        static BoxAudioCodec audio_codec(
            i2c_bus_,                           // I2C 总线句柄
            AUDIO_INPUT_SAMPLE_RATE,            // 输入采样率 16kHz
            AUDIO_OUTPUT_SAMPLE_RATE,           // 输出采样率 16kHz
            AUDIO_I2S_GPIO_MCLK,                // MCLK
            AUDIO_I2S_GPIO_BCLK,                // BCLK
            AUDIO_I2S_GPIO_WS,                  // WS/LRCK
            AUDIO_I2S_GPIO_DOUT,                // 数据输出到 ES8311
            AUDIO_I2S_GPIO_DIN,                 // 数据输入从 ES7210
            PA_ENABLE_GPIO,                     // PA 使能引脚
            ES8311_I2C_ADDR,                    // ES8311 I2C 地址
            ES7210_I2C_ADDR,                    // ES7210 I2C 地址
            true                                // 启用 AEC 参考输入
        );
        return &audio_codec;
    }
};

DECLARE_BOARD(ChatboxBoard);
