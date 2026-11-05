#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// 如果使用 Duplex I2S 模式，请注释下面一行
#define AUDIO_I2S_METHOD_SIMPLEX

#ifdef AUDIO_I2S_METHOD_SIMPLEX

#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_48
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_21
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_47
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_42
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_40
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_39
// MAX98357A SD_MODE pin (active high, put the amp into I2S mode)
#define SD_MODE_GPIO GPIO_NUM_41
#else

#define AUDIO_I2S_GPIO_WS GPIO_NUM_48
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_40
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_47
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_42

#endif


#define BUILTIN_LED_GPIO        GPIO_NUM_NC
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define TOUCH_BUTTON_GPIO       GPIO_NUM_NC
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC

// A MCP Test: Control a lamp
#define LAMP_GPIO GPIO_NUM_NC

// GC9306 240x320 SPI 屏（FPC 10脚，VSPI 硬件引脚）
// 接线：DC=15, CS=16, SCL=18, SDA=17, RST=7, 背光LED_A直连3.3V(无常控脚)
// 驱动：GC9A01 驱动 + GC9306 专属序列（config.cc gc9306_lcd_init_cmds，
//       与 GC9A01/GC9107 圆屏序列完全不同——0004/0005 白屏即因序列用错）
//
// ★ 2026-09-12 屏幕暂撤（0007）：三版正确序列仍白屏→硬件问题概率极高；
//   且 LVGL 任务加剧 CPU 过载（AFE FEED 爆仓+IDLE1 饿死 watchdog）。
//   硬件核对后（FPC脚序方向/SCL-SDA是否接反/虚焊）恢复：取消下行注释。
// #define SISI_HAS_DISPLAY 1
#define LCD_TYPE_GC9A01_SERIAL
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  320
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
#define DISPLAY_INVERT_COLOR    true
#define DISPLAY_RGB_ORDER  LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE 0
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_NC
#define DISPLAY_MOSI_PIN      GPIO_NUM_17
#define DISPLAY_CLK_PIN       GPIO_NUM_18
#define DISPLAY_DC_PIN        GPIO_NUM_15
#define DISPLAY_RST_PIN       GPIO_NUM_7
#define DISPLAY_CS_PIN        GPIO_NUM_16

#endif // _BOARD_CONFIG_H_
