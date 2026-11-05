#include "audio_codec.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <cassert>
#include <driver/i2s_common.h>

#define TAG "AudioCodec"

// ★ 2026-09-18 重复实现合并（声明注释见 audio_codec.h）：
//   原 es8311/es8374/es8388/es8389 各一份 ~55 行同名实现，逻辑完全一致，统一到基类。
//   失败只记录不 abort：ESP_ERROR_CHECK 会把"通道申请/初始化失败"升级成整机崩溃。
void AudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                                      gpio_num_t dout, gpio_num_t din,
                                      int dma_desc_num, int dma_frame_num) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = (uint32_t)dma_desc_num,
        .dma_frame_num = (uint32_t)dma_frame_num,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建 I2S 双通道失败: %d", (int)err);
        tx_handle_ = nullptr;
        rx_handle_ = nullptr;
        return;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,            // 必须按结构体声明顺序书写（ext_clk_freq_hz 在 mclk_multiple 之前）
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    if ((err = i2s_channel_init_std_mode(tx_handle_, &std_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "TX 通道初始化失败: %d", (int)err);
        return;
    }
    if ((err = i2s_channel_init_std_mode(rx_handle_, &std_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "RX 通道初始化失败: %d", (int)err);
        return;
    }
    if ((err = i2s_channel_enable(tx_handle_)) != ESP_OK) {
        ESP_LOGE(TAG, "TX 通道启用失败: %d", (int)err);
        return;
    }
    if ((err = i2s_channel_enable(rx_handle_)) != ESP_OK) {
        ESP_LOGE(TAG, "RX 通道启用失败: %d", (int)err);
        return;
    }
    ESP_LOGI(TAG, "Duplex channels created");
}

AudioCodec::AudioCodec() {
}

AudioCodec::~AudioCodec() {
}

void AudioCodec::OutputData(const int16_t* data, size_t samples) {
    Write(data, samples);
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        return true;
    }
    return false;
}

void AudioCodec::Start() {
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    if (output_volume_ <= 0) {
        ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)", output_volume_);
        output_volume_ = 10;
    }

    ESP_LOGI(TAG, "Audio codec started");
}

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);
    
    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
}

void AudioCodec::SetInputGain(float gain) {
    input_gain_ = gain;
    ESP_LOGI(TAG, "Set input gain to %.1f", input_gain_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}
