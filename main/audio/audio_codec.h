#ifndef _AUDIO_CODEC_H
#define _AUDIO_CODEC_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <driver/i2s_std.h>

#include <vector>
#include <string>
#include <functional>

#include "board.h"

#define AUDIO_CODEC_DMA_DESC_NUM 8
#define AUDIO_CODEC_DMA_FRAME_NUM 480

class AudioCodec {
public:
    AudioCodec();
    virtual ~AudioCodec();
    
    virtual void SetOutputVolume(int volume);
    virtual void SetInputGain(float gain);
    virtual void EnableInput(bool enable);
    virtual void EnableOutput(bool enable);

    // data 可能是 PSRAM 缓冲(见 PsramAllocator)，用指针+长度避免拷贝回内部SRAM
    virtual void OutputData(const int16_t* data, size_t samples);
    virtual bool InputData(std::vector<int16_t>& data);
    virtual void Start();

    inline bool duplex() const { return duplex_; }
    inline bool input_reference() const { return input_reference_; }
    inline int input_sample_rate() const { return input_sample_rate_; }
    inline int output_sample_rate() const { return output_sample_rate_; }
    inline int input_channels() const { return input_channels_; }
    inline int output_channels() const { return output_channels_; }
    inline int output_volume() const { return output_volume_; }
    inline float input_gain() const { return input_gain_; }
    inline bool input_enabled() const { return input_enabled_; }
    inline bool output_enabled() const { return output_enabled_; }

protected:
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;

    bool duplex_ = false;
    bool input_reference_ = false;
    int reference_channel_ = 1;
    bool input_enabled_ = false;
    bool output_enabled_ = false;
    int input_sample_rate_ = 0;
    int output_sample_rate_ = 0;
    int input_channels_ = 1;
    int output_channels_ = 1;
    int output_volume_ = 70;
    float input_gain_ = 0.0;

    virtual int Read(int16_t* dest, int samples) = 0;
    virtual int Write(const int16_t* data, int samples) = 0;

    // ★ 2026-09-18 重复实现合并：I2S 双通道（TX+RX 全双工）创建与启用。
    //   原 es8311/es8374/es8388/es8389 四个 codec 各有一份 ~55 行的 CreateDuplexChannels，
    //   除类名外逻辑完全相同（实测 diff 仅函数名、mclk 字段书写顺序与 DMA 深度差异）——
    //   上提到基类统一维护，子类构造函数直接调用本方法即可（DMA 深度可按板型覆盖）。
    //   16kHz/16bit/立体声/256×MCLK，master 模式，I2S_NUM_0。
    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                              gpio_num_t dout, gpio_num_t din,
                              int dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
                              int dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM);
};

#endif // _AUDIO_CODEC_H
