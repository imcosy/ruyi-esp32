#include "box_audio_codec.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cstdlib>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <driver/i2s_common.h>
#if CONFIG_MEETING_BOARD
#include <driver/i2s_tdm.h>
#endif

#define TAG "BoxAudioCodec"

BoxAudioCodec::BoxAudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin, uint8_t es8311_addr, uint8_t es7210_addr, bool input_reference,
    int reference_channel) {
    duplex_ = true; // 是否双工
    input_reference_ = input_reference; // 是否使用参考输入，实现回声消除
    reference_channel_ = reference_channel;
    ref_slot_ = reference_channel;  // 参考槽初值=配置槽，Read() 内运行时自适配纠偏
    input_channels_ = input_reference_ ? 2 : 1; // 输入通道数
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    // ★ PGA 回原生默认 30dB（2026-09-14 撤销 33dB 硬编码）：电平交给 AFE AGC
    //   在人声段抬音（agc_init=true），数字/模拟增益 hack 全部撤销——数字 ×3
    //   把底噪同步放大喂 MultiNet（误检源），33dB 硬编码与原生 xiaozhi BOX 配置背离。
    input_gain_ = 30;

    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // Output
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (i2c_port_t)1,
        .addr = es8311_addr,
        .bus_handle = i2c_master_handle,
    };
    out_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(out_ctrl_if_ != NULL);

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != NULL);

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = out_ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = pa_pin;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    out_codec_if_ = es8311_codec_new(&es8311_cfg);
    assert(out_codec_if_ != NULL);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = out_codec_if_,
        .data_if = data_if_,
    };
    output_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(output_dev_ != NULL);

    // Input
    i2c_cfg.addr = es7210_addr;
    in_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(in_ctrl_if_ != NULL);

#if CONFIG_MEETING_BOARD
    // ★ meeting板（立创实战派ESP32-S3，硬件=xiaozhi lckfb/szpi-esp32s3 同一项目，
    //   2026-09-15 硬件溯源确认）：ES7210 全MIC选择 + RX TDM 4槽。
    //   硬件上 ES8311 喇叭输出回采到 ES7210 的 MIC3 输入（硬件AEC参考信号）。
    //   ES7210 TDM 槽位序 ≠ MIC物理序（MIC1,MIC3,MIC2,MIC4）→ 槽1 = MIC3 = 回采参考，
    //   AFE input_format "MR" 的 R 通道正好取到参考信号，硬件AEC才能对消TTS回声。
    //   【槽序佐证】xiaozhi 官方 box_audio_codec.cc 注释原文：
    //   "Physical MIC3 is the playback reference input" +
    //   "ES7210 gain masks use physical MIC numbering, which differs
    //    from the TDM slot order (MIC1, MIC3, MIC2, MIC4)"。
    //   ES7210 驱动(es7210.c)无槽位重排寄存器，槽序重排是芯片内部固定行为（TDM槽序
    //   由芯片 MIC 输入复用决定，0x12=0x02 仅使能 TDM 输出），勿再走 0x13~0x16
    //   写寄存器弯路——那些是 automute/mute-range 寄存器，与槽映射无关。
    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl_if_;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
#else
    // ★ 双麦模组（chatbox）：ES7210 dual 模式（标准 I2S 立体声输出 左=mic1 右=mic2）
    //   不能选 MIC1-4 全选——那会触发组件强开 TDM，与模组硬件不符 → 静音
    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl_if_;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2;
#endif
    in_codec_if_ = es7210_codec_new(&es7210_cfg);
    assert(in_codec_if_ != NULL);

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = in_codec_if_;
    input_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(input_dev_ != NULL);

    ESP_LOGI(TAG, "BoxAudioDevice initialized");
}

BoxAudioCodec::~BoxAudioCodec() {
    ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    esp_codec_dev_delete(output_dev_);
    ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    esp_codec_dev_delete(input_dev_);

    audio_codec_delete_codec_if(in_codec_if_);
    audio_codec_delete_ctrl_if(in_ctrl_if_);
    audio_codec_delete_codec_if(out_codec_if_);
    audio_codec_delete_ctrl_if(out_ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}

void BoxAudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

#if !CONFIG_MEETING_BOARD
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256
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
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
#endif

#if CONFIG_MEETING_BOARD
    // ★ meeting板 RX 用 TDM（与上游 xiaozhi ESP-BOX 实现一致）：16k×16bit。
    //   ES7210 TDM(REG12=0x02, x1 LRCK) 输出槽序 = MIC1,MIC3,MIC2,MIC4 → 槽1 = MIC3 回采参考。
    //   ws_pol=false（Philips）：ES7210 I2S normal 格式槽0在 LRCK 下降沿后输出，
    //   ESP32 TDM Philips 默认帧起始于 WS 下降沿——两者天然对齐（上游实测配置）。
    i2s_tdm_config_t rx_tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)input_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,  // full_duplex_slave 角色下仅占位（跟随线上时钟）
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = I2S_GPIO_UNUSED,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
#else
    // ★ chatbox/sisi 双麦模组 RX 必须用 STD 模式：ESP32-S3 同一 I2S 控制器的 TX/RX 共享时钟发生器，
    //   原 RX 用 TDM(4slot/BCLK=1MHz) 会劫持共享时钟+BCLK/WS引脚，
    //   导致 ES8311 收到错误时钟格式 → 输出无声；ES7210 强开 TDM 与模组不符 → 录音静音。
    //   改为 TX/RX 双 STD：16k × 16bit × 2slot = BCLK 512kHz 完全一致。
    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)input_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,   // ES7210 dual: 左槽=mic1 右槽=mic2
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
            .dout = I2S_GPIO_UNUSED,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
#endif

#if CONFIG_MEETING_BOARD
    // ★ TX 也必须用 TDM（20260915.0032，对话录音变形根因）：
    //   【0031 遗留缺陷】TX=STD 2×16bit=32-BCLK 帧。唤醒期 RX 未受害（EnableInput 把
    //   TX 扩位到 64-BCLK）；但 TTS 起播时 EnableOutput(1ch) 经 esp_codec_dev 协商
    //   （want_bits 32 < run_bits 64）把 TX 缩回 16bit 槽 → BCLK 512kHz、线上变
    //   32-BCLK 帧 → RX TDM 从机(4×16bit/64-BCLK) 槽位彻底错乱 → 对话录音变形/
    //   没有话头/ASR 不准。_i2s_data_write 纯直通 ⇒ TX 硬件槽宽必须=软件 16bit 数据，
    //   同时又要 64-BCLK 帧 ⇒ 唯一解：TX=TDM 4槽16bit（数据只占槽0，DMA 每帧 16bit）。
    //   ES8311 从机只看 WS 低电平期的槽0 数据（16bit I2S normal），完全兼容；
    //   BCLK 1.024MHz / 64-BCLK TDM 帧从此恒定，不再随 Input/Output 开关横跳。
    i2s_tdm_config_t tx_tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,  // 占位：IDF 按 total_slot×slot_bits 实算 BCLK=64Fs
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = i2s_tdm_slot_mask_t(I2S_TDM_SLOT0),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    // ★ 初始化顺序即主从关系（IDF 铁律 i2s_std.c:263 "The later initialized channel
    //   must be slave for full duplex"）：TX(TDM) 先初始化 → TX 为时钟主设备；
    //   RX(TDM) 后初始化 → full_duplex_slave 跟随线上时钟。
    //   【0030 根因】0030 曾把 RX(TDM) 提前当主时钟 → 播放 2 倍速爆噪（已修）。
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(tx_handle_, &tx_tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &rx_tdm_cfg));
#else
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &rx_std_cfg));
#endif
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
#if CONFIG_MEETING_BOARD
    ESP_LOGI(TAG, "Duplex channels created (TX=TDM master slot0, RX=TDM slave slots01, 64-BCLK frame)");
#else
    ESP_LOGI(TAG, "Duplex channels created (TX=STD RX=STD, BCLK=512k)");
#endif
}

void BoxAudioCodec::SetOutputVolume(int volume) {
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, volume));
    AudioCodec::SetOutputVolume(volume);
}

void BoxAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
#if CONFIG_MEETING_BOARD
        // ★ 记录输入使能时刻：EnableInput 后 600ms 是 codec/AFE warmup（实测原始
        //   读取会瞬时打满幅），Read() 的槽位取证跳过该窗口，与 AudioService
        //   上行监视的 warmup 跳过（monitor_skip_until_us_）同一道理。
        input_enabled_us_.store(esp_timer_get_time(), std::memory_order_relaxed);
        // ★ meeting板（与上游 xiaozhi bk/ 原版逐字一致）：channel=4 是扩位协商的钥匙——
        //   esp_codec_dev 的 check_fs_compatible 按 want_bits(4×16=64) > run_bits(2×16=32)
        //   把 TX 重配为 32bit 槽 / BCLK 1.024MHz（get_bits=64/2），RX 重配为 TDM
        //   total_slot=4 → 线上成为 ES7210 TDM 要求的 64-BCLK 帧；
        //   RX 硬件只把 mask 选中的槽写入 DMA（i2s_get_buf_size 按 popcount(mask)
        //   分配，槽0=MIC1、槽1=MIC3回采参考），Read 直通即得 [mic, ref] 2ch 交错帧，
        //   与 input_channels_=2、AFE input_format "MR" 完全对齐。
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 4,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        if (input_reference_) {
            fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(reference_channel_);
        }
#if CONFIG_USE_DEVICE_AEC
        // ★ 2026-09-20 诊断（AEC 参考通道 R_energy=0）：RX 抓全 4 个 TDM 槽，
        //   Read() 逐槽统计能量——验证 ES7210 TDM 槽序假设（槽1=物理MIC3=回采参考）
        //   是否成立：若参考实际在其它槽，本日志直接暴露（TTS 播放时该槽能量显著）。
        fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1)
                        | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3);
#endif
#else
        // ★ 双麦 stereo 2ch（左=mic1 右=mic2）：必须与 TX 同 BCLK(512k)，
        //   原 channel=4 是 TDM 配置，会触发 esp_codec_dev 强开 TDM 模式
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 2,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        if (input_reference_) {
            fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(reference_channel_);
        }
#endif
        ESP_ERROR_CHECK(esp_codec_dev_open(input_dev_, &fs));
        // 主麦（物理 MIC1）：input_gain_（meeting 36dB 远场拾音，0042 起）
        ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(input_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), input_gain_));
        if (input_reference_) {
            // ★ 2026-09-25 0079 根修：回采参考（物理 MIC3）单独锁 30dB，不再复用
            //   input_gain_（36dB）。0042→0078 参考被 36dB 过驱削顶（dump REG45=0x1d，
            //   播放期 |avg|≈3万超正弦满幅上限），AEC 线性滤波匹配失真参考 → 零抑制
            //   → 回声灌上行（无法唤醒/打断/录音差）+ AEC 发散病态计算占死 CPU1
            //   （FEED ringbuffer 满刷屏）。ES7210 增益掩码按物理 MIC 编号：
            //   reference_channel_=2 → 物理 MIC3 = 回采参考（与 TDM 槽号无关，
            //   参考槽由 Read() 运行时锁定，见 ref_slot_ 注释）。
            ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(input_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(reference_channel_), kReferenceGainDb));
        }
#if CONFIG_MEETING_BOARD && CONFIG_USE_DEVICE_AEC
        // ★ 2026-09-20 0072 起意图 = MIC3 保持 30dB（0071 诊断：0dB 参考仅 110~175 vs
        //   回声 13770+，幅比 1:125 太弱；0070 时 30dB 幅比 6719:14865≈0.45 健康）。
        //   0072 只删了 0dB 覆盖行、漏了上方复用 input_gain_ 的参考行（0042 起 36dB），
        //   直到 0079 才真正落实 kReferenceGainDb=30dB。寄存器转储保留（诊断）：
        //   修复后 45 应为 1a（0x10|0x0a=GAIN_30DB），若再出现 1d 即参照旧病回归。
        esp_codec_dev_dump_reg(input_dev_);
#endif
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    }
    AudioCodec::EnableInput(enable);
}

void BoxAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
#if CONFIG_MEETING_BOARD
        // ★ TX 必须按 4ch 帧结构打开（20260915.0032）：数据只占槽0（mask=SLOT0），
        //   DMA 每帧 16bit —— write 直通数据布局不变；esp_codec_dev 的 set_fmt 仅在
        //   channel==1 时强转 2ch，故用 4ch 可与 EnableInput 的公共 fs 完全一致，
        //   check_fs_compatible 走"channel 相同"分支只 set_fs 不缩位 → TX 恒为
        //   TDM 4槽16bit / BCLK 1.024MHz / 64-BCLK 帧，RX 从机槽位永不错乱。
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 4,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
#else
        // Play 16bit 1 channel
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
#endif
        ESP_ERROR_CHECK(esp_codec_dev_open(output_dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, output_volume_));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    }
    AudioCodec::EnableOutput(enable);
}

// 播放突发证据下的槽位锁定（2026-09-25 0078 重写，判据与失效分析见 box_audio_codec.h）：
//   ① 回采槽 = 整段突发能量最大槽，且绝对量级≥2500（电信号直连近满幅，声学回声/
//      人声量级差 20 倍以上）、静默间隙能量 ≤ 播放时 1/8（播放停止即塌缩到底噪——
//      用户语音停止后持续存在，天然排除；同时封死"近距离说话被误判回采"的旧坑）。
//   ② 麦克风槽 = 回采槽以外的突发能量最大者（播放期的声学回声/插话人声必落此槽，
//      死槽恒≈0）——仅在回采槽身份可信（本帧刚锁定或已锁定即最大槽）时才修，
//      防止回采未锁定时把最大槽误锁成麦克风（那会把 TTS 当人声上行、用户语音丢失）。
static void TryLockSlots(const int32_t burst_max[4], const int32_t idle_max[4],
                         std::atomic<int>& ref_slot, std::atomic<int>& mic_slot) {
    int best = 0;
    for (int s = 1; s < 4; s++) {
        if (burst_max[s] > burst_max[best]) best = s;
    }
    int second = (best == 0) ? 1 : 0;
    for (int s = 0; s < 4; s++) {
        if (s != best && burst_max[s] > burst_max[second]) second = s;
    }
    if (burst_max[best] >= 2500 && idle_max[best] <= burst_max[best] / 8
        && best != ref_slot.load(std::memory_order_relaxed)) {
        ref_slot.store(best, std::memory_order_relaxed);
        ESP_LOGW(TAG, "AEC 参考槽自动锁定: slot=%d（回采突发峰值=%d vs 次大=%d，静默=%d）",
                 best, burst_max[best], burst_max[second], idle_max[best]);
    }
    if (best == ref_slot.load(std::memory_order_relaxed)) {
        int m = -1;
        for (int s = 0; s < 4; s++) {
            if (s == best) continue;
            if (m < 0 || burst_max[s] > burst_max[m]) m = s;
        }
        if (m >= 0 && burst_max[m] >= 200 && m != mic_slot.load(std::memory_order_relaxed)) {
            mic_slot.store(m, std::memory_order_relaxed);
            ESP_LOGW(TAG, "麦克风槽自动锁定: slot=%d（播放期声学证据=%d）", m, burst_max[m]);
        }
    }
}

int BoxAudioCodec::Read(int16_t* dest, int samples) {
    if (input_enabled_) {
#if CONFIG_MEETING_BOARD && CONFIG_USE_DEVICE_AEC
        // ★ 2026-09-25 0078：DMA 每帧 4 槽 [s0,s1,s2,s3]，读入后按运行时锁定的
        //   槽位抽取成 2ch [麦克风, 回采参考] 交 AFE（"MR"），逐槽能量取证同步进行。
        static int16_t scratch[1024];
        int frames = samples / 2;   // 2ch 帧数
        if (frames * 4 > (int)(sizeof(scratch) / sizeof(scratch[0]))) {
            frames = (int)(sizeof(scratch) / sizeof(scratch[0])) / 4;
        }
        esp_err_t ret = esp_codec_dev_read(input_dev_, (void*)scratch, frames * 4 * sizeof(int16_t));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Read failed: %s", esp_err_to_name(ret));
        }
        int rs = ref_slot_.load(std::memory_order_relaxed);
        int ms = mic_slot_.load(std::memory_order_relaxed);
        for (int i = 0; i < frames; i++) {
            dest[i * 2]     = scratch[i * 4 + ms];  // 麦克风槽（运行时自适配，见 mic_slot_ 注释）
            dest[i * 2 + 1] = scratch[i * 4 + rs];  // 参考槽（运行时自适配，见 ref_slot_ 注释）
        }
        // ★ 2026-09-25 槽位自适配取证（判据与旧版失效分析见 box_audio_codec.h）：
        //   核心修正——对整段播放突发取各槽能量最大值再判定。旧版按单个~10ms 帧
        //   判定且要求连续2次（间隔~1s）成立，TTS 电平起伏使确认计数反复清零，
        //   0077 实测整个会话零锁定 → 参考槽永远是 meeting.cc 传的死槽。
        static int32_t burst_max[4] = {0, 0, 0, 0};  // 当前播放突发各槽能量最大值
        static int32_t idle_max[4]  = {0, 0, 0, 0};  // 当前静默间隙各槽能量最大值
        static bool burst_active = false;            // 是否处于喇叭输出激活窗口
        int32_t ev[4] = {0, 0, 0, 0};
        for (int i = 0; i < frames; i++) {
            for (int s = 0; s < 4; s++) {
                ev[s] += abs(scratch[i * 4 + s]);
            }
        }
        for (int s = 0; s < 4; s++) ev[s] /= frames;
        int64_t now_us = esp_timer_get_time();
        bool out_active = (now_us - last_write_us_.load(std::memory_order_relaxed)) < 500000;
        // EnableInput 后 600ms 是 codec/AFE warmup（实测原始读取瞬时打满幅，
        // 与 AudioService 上行监视的 warmup 跳过同理），不参与任何槽位取证。
        bool warmup = (now_us - input_enabled_us_.load(std::memory_order_relaxed)) < 600000;
        if (out_active && !burst_active) {
            burst_active = true;
            burst_max[0] = burst_max[1] = burst_max[2] = burst_max[3] = 0;
        }
        if (out_active) {
            // 播放突发：各槽能量按最大值累计，判据成立立即锁定（剩余突发 AEC 即生效）
            for (int s = 0; s < 4; s++) {
                if (ev[s] > burst_max[s]) burst_max[s] = ev[s];
            }
            TryLockSlots(burst_max, idle_max, ref_slot_, mic_slot_);
        } else {
            if (burst_active) {
                // 播放结束：整段突发的最终值补一次判定（防音量后段才起的漏锁），
                // 然后重置静默累计——下次突发判定用的必须是本间隙的人声塌缩证据。
                burst_active = false;
                TryLockSlots(burst_max, idle_max, ref_slot_, mic_slot_);
                idle_max[0] = idle_max[1] = idle_max[2] = idle_max[3] = 0;
            }
            if (!warmup) {
                // 静默间隙：麦克风槽取证（用户语音所在槽；回采已塌陷、死槽恒≈0）
                for (int s = 0; s < 4; s++) {
                    if (ev[s] > idle_max[s]) idle_max[s] = ev[s];
                }
                int best = 0;
                for (int s = 1; s < 4; s++) if (idle_max[s] > idle_max[best]) best = s;
                int second = (best == 0) ? 1 : 0;
                for (int s = 0; s < 4; s++) if (s != best && idle_max[s] > idle_max[second]) second = s;
                if (idle_max[best] >= 300 && idle_max[best] >= idle_max[second] * 3
                    && best != mic_slot_.load(std::memory_order_relaxed)) {
                    mic_slot_.store(best, std::memory_order_relaxed);
                    ESP_LOGW(TAG, "麦克风槽自动锁定: slot=%d（静默期人声=%d vs 次大=%d）",
                             best, idle_max[best], idle_max[second]);
                }
            }
        }
        // ★ 诊断日志只在喇叭输出激活（TTS/提示音播放）时打印（0077 起，idle 不刷屏）；
        //   槽位锁定事件已由上方 LOGW 单独打印，不会漏。
        static uint32_t dbg_cnt = 0;
        if (++dbg_cnt % 100 == 1 && out_active) {
            ESP_LOGI(TAG, "[4槽诊断] s0=%d s1=%d s2=%d s3=%d（麦=%d 参=%d）",
                     ev[0], ev[1], ev[2], ev[3],
                     mic_slot_.load(std::memory_order_relaxed),
                     ref_slot_.load(std::memory_order_relaxed));
        }
#else
        // ★ 直通（与上游 xiaozhi bk/ 原版一致，20260915.0031）：
        //   meeting板 DMA 每帧只含 mask 选中的 2 槽 [槽0=MIC1, 槽1=MIC3回采参考]
        //   （i2s_get_buf_size 按 active_slot=popcount(mask) 分配缓冲），
        //   esp_codec_dev_read 纯直通无抽取 → 直接就是 2ch [M,R] 交错帧。
        //   【0022~0030 根因】曾按 4 槽手工抽取：每帧多读一倍字节 → 麦克风流
        //   被压缩成 2 倍速错位帧 → AFE/MultiNet 全部失真 = 无法唤醒。
        esp_err_t ret = esp_codec_dev_read(input_dev_, (void*)dest, samples * sizeof(int16_t));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Read failed: %s", esp_err_to_name(ret));
        }
#endif
    }
    return samples;
}

int BoxAudioCodec::Write(const int16_t* data, int samples) {
    if (output_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(output_dev_, (void*)data, samples * sizeof(int16_t)));
        last_write_us_.store(esp_timer_get_time(), std::memory_order_relaxed);  // 喇叭输出激活记录（回采检测门控）
    }
    return samples;
}
