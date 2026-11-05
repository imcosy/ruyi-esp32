#ifndef _BOX_AUDIO_CODEC_H
#define _BOX_AUDIO_CODEC_H

#include "audio_codec.h"

#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <mutex>
#include <atomic>


class BoxAudioCodec : public AudioCodec {
private:
    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* out_ctrl_if_ = nullptr;
    const audio_codec_if_t* out_codec_if_ = nullptr;
    const audio_codec_ctrl_if_t* in_ctrl_if_ = nullptr;
    const audio_codec_if_t* in_codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;

    esp_codec_dev_handle_t output_dev_ = nullptr;
    esp_codec_dev_handle_t input_dev_ = nullptr;
    std::mutex data_if_mutex_;

    // ★ 2026-09-25 槽位运行时自适配（0078 重写，根修旧版"永不锁定"缺陷）：
    //   ES7210 TDM RX 从机槽相位每次开机对齐有随机性（同一固件实测不同 boot 回采
    //   分别落在槽1/槽2，甚至槽0 是回采、真麦在其它槽），固定 reference_channel
    //   必然看运气。旧判据（0075-0077）要求"单拍能量>1500 且连续2次（间隔~1s）
    //   成立"，而取证采样每秒只统计一个~10ms 帧、TTS 电平起伏使条件反复闪烁、
    //   确认计数被不断清零——0077 实测整个会话零锁定，参考槽永远停在 meeting.cc
    //   传的死槽2（恒0-20），AEC 参考恒零（AEC_DEBUG R_energy=0~22）：引导语
    //   回声原样泄漏上行成幻影轮，AGC 被近满幅回声压 gain 后用户语音再被压到
    //   服务端 VAD 门限以下 → "报手机号没有任何反应"的终极根因。
    //   新判据按物理签名取证，任一成立即锁、无连续确认计数：
    //   ① 回采槽（播放突发中识别）：喇叭播放时电信号直连的回采恒为全槽能量最大
    //      （近满幅，声学回声/人声量级差 20 倍以上），且播放停止后能量塌缩到底噪
    //      （用户语音停止后持续存在，天然被排除）。对整段突发取各槽最大值再判定，
    //      不受单帧电平起伏欺骗；判据一成立立即锁定，本突发剩余部分 AEC 即生效。
    //   ② 麦克风槽（静默间隙识别）：播放停止后能量最大且≥3倍于次大槽的即用户
    //      语音所在槽（回采已塌陷、死槽恒≈0）；播放突发的声学回声同样标记麦克风
    //      槽（回采锁定成功后 argmax 非回采槽），默认槽错误时第一条提示音期内
    //      即自我纠正。I2S 省电重开后槽相位再洗牌，下一条提示音重新取证纠正。
    std::atomic<int> ref_slot_{2};
    std::atomic<int> mic_slot_{0};
    std::atomic<int64_t> last_write_us_{0};    // 最近一次喇叭输出写入时刻（回采检测门控）
    std::atomic<int64_t> input_enabled_us_{0}; // 最近一次 EnableInput(true) 时刻（warmup 取证跳过窗口）

    // ★ 2026-09-25 0079 根修：回采参考（物理 MIC3）增益固定 30dB，与主麦 input_gain_
    //   （meeting 36dB 远场拾音，0042 起）彻底解耦。
    //   【回归链】0042 把主麦 30→36dB 时，EnableInput 的参考通道一行复用同一 input_gain_
    //   （注释还自以为"配比不变"）→ MIC3 被推到 36dB；0072 修复只移除了 0dB 覆盖行，
    //   漏了本行 → 0072~0078 参考一直是 36dB。0078 实测实锤：ES7210 dump REG45=0x1d
    //   （=0x10|0x0d，GAIN_36DB），TTS 播放期参考槽 |avg|=25424~30115（正弦满幅上限
    //   仅 ~20800）→ 参考削顶失真 → AEC 线性滤波器无法匹配失真参考 → 零抑制
    //   （播放期 micRMS=25182 vs AFE 输出 12467，仅 -6dB）→ TTS 回声原样灌入上行 →
    //   服务端把回声当用户语音（ASR 幻影轮、声纹误判）→ 无法唤醒/无法 AEC 打断/
    //   录音全是回声，"step" 被埋。AEC 滤波器发散时还有病态计算占死 CPU1 →
    //   "Ringbuffer of AFE(FEED) is full" 刷屏（连上 wifi 直接刷的那次）。
    //   【实测依据】0070：MIC3=30dB 幅比 6719:14865≈0.45 = 健康区间；0071：0dB 太弱
    //   （参考 110~175 vs 回声 13770+，幅比 1:125）是误判教训。参考通道是 ES8311
    //   电子直连（非声学拾音），电平远热，锁 30dB，严禁再与主麦共用增益。
    static constexpr float kReferenceGainDb = 30.0f;

    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

public:
    BoxAudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
        gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
        gpio_num_t pa_pin, uint8_t es8311_addr, uint8_t es7210_addr, bool input_reference,
        int reference_channel = 1);
    virtual ~BoxAudioCodec();

    virtual void SetOutputVolume(int volume) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
};

#endif // _BOX_AUDIO_CODEC_H
