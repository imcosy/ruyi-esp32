#ifndef OGG_MUXER_H_
#define OGG_MUXER_H_

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

/// OGG/Opus 简易封装器（仅用于一次性产出完整 OGG 文件）
///
/// 用途：将若干 Opus 帧打包为一个完整的 OGG/Opus 容器，
///       作为 S2S 协议的一轮 Binary 帧发送给 s2s_service.py。
///
/// 标准 OGG/Opus 文件结构：
///   Page 0 (BOS): OpusHead 包
///   Page 1:       OpusTags 包
///   Page 2..n:    Opus 音频帧（每页可含多帧，granule 递增）
///   最后一页:      EOS 标志
///
/// 使用方式：
///   OggMuxer muxer;
///   muxer.Start(serial, sample_rate, channels);
///   muxer.AddOpusFrame(frame_data, frame_len, samples_per_frame);
///   ...
///   std::vector<uint8_t> ogg = muxer.Finalize();
class OggMuxer {
public:
    OggMuxer() = default;
    ~OggMuxer() = default;

    /// 开始一个新的 OGG/Opus 流
    /// serial: 流序列号（建议使用随机值，0 也合法）
    /// sample_rate: OpusHead 中声明的原始采样率（如 16000/48000）
    /// channels: 声道数（一般为 1=mono）
    void Start(uint32_t serial, int sample_rate, int channels = 1);

    /// 追加一个 Opus 帧
    /// data/len: Opus 帧数据
    /// samples_per_frame: 该帧对应的 PCM 采样数（用于 granule 计算，48kHz 域）
    ///                    例如 20ms@48kHz = 960
    void AddOpusFrame(const uint8_t* data, size_t len, int samples_per_frame);

    /// 完成并产出完整的 OGG 字节流
    std::vector<uint8_t> Finalize();

    /// 是否已启动（Start 调用过）
    bool IsStarted() const { return started_; }

private:
    bool started_ = false;
    uint32_t serial_ = 0;
    int sample_rate_ = 48000;
    int channels_ = 1;
    int page_seq_ = 0;            // OGG 页序号
    int64_t granule_ = 0;         // 累计 granule 位置（PCM 采样数，48kHz 域）
    std::vector<uint8_t> out_;    // 输出缓冲

    // 当前页累积的 Opus 帧数据 + 段表
    std::vector<uint8_t> page_data_;
    std::vector<uint8_t> seg_table_;  // 段长度表（每段 0-255）
    int frames_in_page_ = 0;

    void WritePage(uint8_t header_type, int64_t granule);
    void WriteOpusHeadPacket();
    void WriteOpusTagsPacket();
    void FlushAudioPage(bool eos);
    void EmitBytes(const void* p, size_t n);
    static void PutU32LE(std::vector<uint8_t>& v, uint32_t x);
    static void PutU16LE(std::vector<uint8_t>& v, uint16_t x);
    static uint32_t Crc32(const uint8_t* data, size_t len);
};

#endif  // OGG_MUXER_H_
