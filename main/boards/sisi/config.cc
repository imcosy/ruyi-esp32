#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "app.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>

#if defined(SISI_HAS_DISPLAY) && defined(LCD_TYPE_GC9A01_SERIAL)
#include "esp_lcd_gc9a01.h"
// GC9306 专属初始化序列（240x320，四公开源交叉验证：AGM M7 等，与 GC9A01/GC9107 圆屏序列不同！）
// 结构：0xFE/0xEF 解锁 → 电源/VCOM(A4-A9/AC-AF) → 时序(E8/E3/FF) → Gamma(F0-F5)
//       → 全窗(2A/2B/2C) → SLPOUT(120ms) → DISPON(20ms)
// 注意：0x36/0x3A 组件会识别为 overwrite 并按本序列值生效（警告日志正常）
static const gc9a01_lcd_init_cmd_t gc9306_lcd_init_cmds[] = {
    {0xfe, (uint8_t[]){0x00}, 0, 0},                    // Inter register enable 1
    {0xef, (uint8_t[]){0x00}, 0, 0},                    // Inter register enable 2
    {0x36, (uint8_t[]){0x48}, 1, 0},                    // MADCTL: MX|BGR（方向0）
    {0x3a, (uint8_t[]){0x05}, 1, 0},                    // RGB565 16bit/pixel
    {0xa4, (uint8_t[]){0x44, 0x44}, 2, 0},              // Power control 7
    {0xa5, (uint8_t[]){0x42, 0x42}, 2, 0},              // Power control 8
    {0xaa, (uint8_t[]){0x88, 0x88}, 2, 0},              // Power control (undoc)
    {0xe8, (uint8_t[]){0x11, 0x0b}, 2, 0},              // Frame rate control
    {0xe3, (uint8_t[]){0x01, 0x10}, 2, 0},              // Source precharge control
    {0xff, (uint8_t[]){0x61}, 1, 0},                    // Internal register (undoc)
    {0xac, (uint8_t[]){0x00}, 1, 0},                    // LDO enable
    {0xad, (uint8_t[]){0x33}, 1, 0},                    // VGLO voltage control
    {0xae, (uint8_t[]){0x2b}, 1, 0},                    // Internal power (undoc)
    {0xaf, (uint8_t[]){0x55}, 1, 0},                    // DIG_VREFAD_VRDD control
    {0xa6, (uint8_t[]){0x2a, 0x2a}, 2, 0},              // VCOM offset voltage 1
    {0xa7, (uint8_t[]){0x2b, 0x2b}, 2, 0},              // VCOM offset voltage 2
    {0xa8, (uint8_t[]){0x18, 0x18}, 2, 0},              // VCOM offset voltage 3
    {0xa9, (uint8_t[]){0x2a, 0x2a}, 2, 0},              // VCOM offset voltage 4
    {0x2a, (uint8_t[]){0x00, 0x00, 0x00, 0xef}, 4, 0},  // Column addr 0-239
    {0x2b, (uint8_t[]){0x00, 0x00, 0x01, 0x3f}, 4, 0},  // Row addr 0-319
    {0x2c, (uint8_t[]){0x00}, 0, 0},                    // Memory write start
    {0xf0, (uint8_t[]){0x02, 0x00, 0x00, 0x1b, 0x1f, 0x0b}, 6, 0},  // Gamma 1
    {0xf1, (uint8_t[]){0x01, 0x03, 0x00, 0x28, 0x2b, 0x0e}, 6, 0},  // Gamma 2
    {0xf2, (uint8_t[]){0x0b, 0x08, 0x3b, 0x04, 0x03, 0x4c}, 6, 0},  // Gamma 3
    {0xf3, (uint8_t[]){0x0e, 0x07, 0x46, 0x04, 0x05, 0x51}, 6, 0},  // Gamma 4
    {0xf4, (uint8_t[]){0x08, 0x15, 0x15, 0x1f, 0x22, 0x0f}, 6, 0},  // Gamma 5
    {0xf5, (uint8_t[]){0x0b, 0x13, 0x11, 0x1f, 0x21, 0x0f}, 6, 0},  // Gamma 6
    {0x11, (uint8_t[]){0x00}, 0, 120},                  // Sleep out + 120ms
    {0x29, (uint8_t[]){0x00}, 0, 20},                   // Display on + 20ms
};
#endif

#define TAG "SisiBoard"

class SisiBoard : public WifiBoard {
private:
    Button boot_button_;
#ifdef SISI_HAS_DISPLAY
    LcdDisplay* display_ = nullptr;

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        // 手接线/FPC 转接 40MHz 易花屏，保守 20MHz 起步，点亮稳定后可提到 40MHz
        io_config.pclk_hz = 20 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
#if defined(LCD_TYPE_GC9A01_SERIAL)
        // ★ vendor_config 必须在 new_panel 之前赋值——组件在 new_panel/init 时读取
        //   并执行自定义初始化序列
        gc9a01_vendor_config_t gc9306_vendor_config = {
            .init_cmds = gc9306_lcd_init_cmds,
            .init_cmds_size = sizeof(gc9306_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
        };
        panel_config.vendor_config = &gc9306_vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
#endif

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }
#endif // SISI_HAS_DISPLAY

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = App::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            // ★ ESP32端只保留ruyi(如意)一个AI角色，按钮唤醒发送"你好如意"
            // 服务端收到wakeup后，detect_word包含"你好"，会调switch_ai_by_name("如意")切换到ruyi角色
            // 然后调sayhello发TTS问候语，并设置has_wakeup=true进入对话状态
            app.WakeWordInvoke("你好如意");
        });
    }

public:
    SisiBoard() :
        boot_button_(BOOT_BUTTON_GPIO) {
#ifdef SISI_HAS_DISPLAY
        InitializeSpi();
#endif
        // Ensure MAX98357A enters I2S mode
#ifdef SD_MODE_GPIO
        if (SD_MODE_GPIO != GPIO_NUM_NC) {
            if (SD_MODE_GPIO == AUDIO_I2S_MIC_GPIO_SCK || SD_MODE_GPIO == AUDIO_I2S_MIC_GPIO_WS || SD_MODE_GPIO == AUDIO_I2S_MIC_GPIO_DIN) {
                ESP_LOGW(TAG, "SD_MODE_GPIO (%d) conflicts with microphone pins - this may stop mic from working", SD_MODE_GPIO);
            }
            gpio_reset_pin(SD_MODE_GPIO);
            gpio_set_direction(SD_MODE_GPIO, GPIO_MODE_OUTPUT);
            gpio_set_level(SD_MODE_GPIO, 1);
        }
#endif
#ifdef SISI_HAS_DISPLAY
        InitializeLcdDisplay();
#endif
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
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        ESP_LOGI(TAG, "Audio pins: SPK BCLK=%d LRCK=%d DOUT=%d | MIC SCK=%d WS=%d DIN=%d",
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        // ★ sisi 板无硬件 codec 麦克风增益（I2S 数字麦 INMP441 类，无 PGA），只能软件补偿。
        // ★ 2026-09-19 华哥根修：软件增益 7.0 → **2.5（与 anan 板完全一致）**。
        //   华哥定调："跟 anan 是一样的阈值，不知道你为什么这次会把增益调那么大"。
        //   实测依据（本日 anan(2.5) vs sisi(7.0) 各段频谱统计）：两板**整体电平原本
        //   就几乎相同**（RMS 5008 vs 4846）——AGC 已把响度拉平，7.0 并没有带来
        //   "更响"，反而把底噪整体抬高、放大 150-600Hz "呼呼"轰鸣（+3~6dB）。
        //   sisi 真正的短板是高频信噪比（2k-5k 比 anan 低 6.7~7.4dB），那是**降噪**
        //   要解决的问题（NSNet2，见 afe_audio_processor.cc），不是靠软件增益硬凑。
        //   历史包袱说明：2.5→4.0→8.0→4.0→7.0 全部是"用增益补灵敏度"的循环，
        //   每次都被底噪放大反噬；本次回到与 anan 同源配置，噪声治理交给 NS。
        audio_codec.SetInputGain(2.5f);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

#ifdef SISI_HAS_DISPLAY
    virtual Display* GetDisplay() override {
        return display_;
    }
#endif // SISI_HAS_DISPLAY
};

DECLARE_BOARD(SisiBoard);
