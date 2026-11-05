#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// BoxAudioCodec Duplex 模式要求输入输出采样率相同
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

// ESP32-S3 (N16R8) + ES7210+ES8311+NS4150B AEC CODEC 音频模块
// 硬件: ES7210(双麦ADC输入) + ES8311(DAC输出) + NS4150B(3W功放)
// AEC: ES8311输出回接到ES7210一路麦克风输入，实现回声消除
// 功能: AEC回声消除，支持实时打断对话
//
// 接线对照表（2026-09-12 修正版 v2，实拍模组丝印从上到下:
// GND VCC EN SDA SCL MCLK BCK DO WS DI —— ★DO在WS前面，v1表格把顺序记反了！）:
// 模块 GND  -> ESP32-S3 GND      (共地)
// 模块 VCC  -> ESP32-S3 3V3      (模块供电)
// 模块 EN   -> ESP32-S3 3V3      (功放常开，省GPIO)
// 模块 SDA  -> ESP32-S3 GPIO1    (I2C SDA)
// 模块 SCL  -> ESP32-S3 GPIO2    (I2C SCL)
// 模块 MCLK -> ESP32-S3 GPIO40   (I2S MCLK；★勿接GPIO48——板载WS2812 RGB在48。
//                                  曾用41：20260914换40排查飞线段问题，GPIO41侧方波输出已验证正常)
// 模块 BCK  -> ESP32-S3 GPIO47   (I2S BCLK)
// 模块 DO   -> ESP32-S3 GPIO21   (I2S DIN，麦声音进入ESP)
// 模块 WS   -> ESP32-S3 GPIO20   (I2S WS/LRCK)
// 模块 DI   -> ESP32-S3 GPIO42   (I2S DOUT，ESP发送音频到喇叭)

// I2C 配置（用于控制 ES7210 和 ES8311）
// 注意: 必须使用 I2C_NUM_1，与 BoxAudioCodec 内部硬编码的 port 一致
#define I2C_MASTER_NUM          I2C_NUM_1
#define I2C_MASTER_SDA_GPIO     GPIO_NUM_1
#define I2C_MASTER_SCL_GPIO     GPIO_NUM_2
#define I2C_MASTER_FREQ_HZ      400000

// ES7210/ES8311 I2C 地址（8位地址格式，组件内部会右移1位转7位）
// ES8311: 0x30 (8位写地址) = 0x18 (7位地址)
// ES7210: 0x82 (8位写地址) = 0x41 (7位地址)，AD0=1
#define ES8311_I2C_ADDR         0x30
#define ES7210_I2C_ADDR         0x82

// I2S 配置（ES7210 和 ES8311 共用 I2S 时钟线）
// TX(输出): STD标准模式 -> ES8311 DAC
// RX(输入): TDM模式4通道 -> ES7210 ADC (双麦+回采参考)
#define AUDIO_I2S_GPIO_MCLK     GPIO_NUM_40
#define AUDIO_I2S_GPIO_BCLK     GPIO_NUM_47
#define AUDIO_I2S_GPIO_WS       GPIO_NUM_20
#define AUDIO_I2S_GPIO_DOUT     GPIO_NUM_42
#define AUDIO_I2S_GPIO_DIN      GPIO_NUM_21

// 功放 PA 引脚（新版接线 EN 直接连 3V3 常开，无 PA GPIO）
#define PA_ENABLE_GPIO          GPIO_NUM_NC

// 按键定义
#define BUILTIN_LED_GPIO        GPIO_NUM_10
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define TOUCH_BUTTON_GPIO       GPIO_NUM_NC

#endif // _BOARD_CONFIG_H_
