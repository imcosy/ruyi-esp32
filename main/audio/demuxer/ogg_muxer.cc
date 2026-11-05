#include "ogg_muxer.h"

// OGG CRC32 表（用于页头校验）
// 多项式 0x04C11DB7（与 zlib 不同的 OGG 专用 CRC）
static uint32_t ogg_crc_table_[256];
static bool ogg_crc_table_inited_ = false;

static void OggCrcInit() {
    if (ogg_crc_table_inited_) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r = i << 24;
        for (int j = 0; j < 8; j++) {
            if (r & 0x80000000) {
                r = (r << 1) ^ 0x04C11DB7;
            } else {
                r <<= 1;
            }
        }
        ogg_crc_table_[i] = r;
    }
    ogg_crc_table_inited_ = true;
}

uint32_t OggMuxer::Crc32(const uint8_t* data, size_t len) {
    OggCrcInit();
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc = (crc << 8) ^ ogg_crc_table_[((crc >> 24) ^ data[i]) & 0xFF];
    }
    return crc;
}

void OggMuxer::PutU32LE(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
    v.push_back((uint8_t)((x >> 16) & 0xFF));
    v.push_back((uint8_t)((x >> 24) & 0xFF));
}

void OggMuxer::PutU16LE(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
}

void OggMuxer::EmitBytes(const void* p, size_t n) {
    auto* b = static_cast<const uint8_t*>(p);
    out_.insert(out_.end(), b, b + n);
}

void OggMuxer::Start(uint32_t serial, int sample_rate, int channels) {
    started_ = true;
    serial_ = serial;
    sample_rate_ = sample_rate;
    channels_ = channels;
    page_seq_ = 0;
    granule_ = 0;
    out_.clear();
    page_data_.clear();
    seg_table_.clear();
    frames_in_page_ = 0;

    // Page 0 (BOS): OpusHead
    WriteOpusHeadPacket();
    WritePage(0x02 /* BOS */, 0);  // granule=0

    // Page 1: OpusTags
    WriteOpusTagsPacket();
    WritePage(0x00, 0);  // granule=0
}

void OggMuxer::WriteOpusHeadPacket() {
    // OpusHead 包结构（19 字节 + 可选 channel mapping）
    // Magic "OpusHead" (8)
    // Version (1) = 1
    // ChannelCount (1)
    // PreSkip (2 LE) = 3840 (推荐值，单位 samples@48kHz)
    // InputSampleRate (4 LE)
    // OutputGain (2 LE) = 0
    // ChannelMappingFamily (1) = 0
    page_data_.clear();
    page_data_.insert(page_data_.end(), (const uint8_t*)"OpusHead", (const uint8_t*)"OpusHead" + 8);
    page_data_.push_back(1);              // version
    page_data_.push_back((uint8_t)channels_);  // channel count
    PutU16LE(page_data_, 3840);           // pre-skip
    PutU32LE(page_data_, (uint32_t)sample_rate_);  // input sample rate
    PutU16LE(page_data_, 0);              // output gain
    page_data_.push_back(0);              // channel mapping family (0 = mono/stereo)
}

void OggMuxer::WriteOpusTagsPacket() {
    // OpusTags 包结构：
    // Magic "OpusTags" (8)
    // Vendor string length (4 LE) + string
    // Comment count (4 LE)
    // (no comments)
    page_data_.clear();
    page_data_.insert(page_data_.end(), (const uint8_t*)"OpusTags", (const uint8_t*)"OpusTags" + 8);
    const char* vendor = "NiceAI-ESP32";
    size_t vlen = 12;
    PutU32LE(page_data_, (uint32_t)vlen);
    page_data_.insert(page_data_.end(), (const uint8_t*)vendor, (const uint8_t*)vendor + vlen);
    PutU32LE(page_data_, 0);  // 0 comments
}

void OggMuxer::AddOpusFrame(const uint8_t* data, size_t len, int samples_per_frame) {
    if (!started_ || len == 0) return;

    // 一个 Opus 帧作为一个段；超过 255 字节需要拆分成多个 255 + 余数
    // 段表中：255 表示"段继续"，非 255 表示"段结束"
    size_t remaining = len;
    while (remaining >= 255) {
        seg_table_.push_back(255);
        remaining -= 255;
    }
    seg_table_.push_back((uint8_t)remaining);  // 段结束（可能为 0）

    page_data_.insert(page_data_.end(), data, data + len);
    granule_ += samples_per_frame;
    frames_in_page_++;

    // 累计超过 ~200ms 或 page_data 过大就 flush 一页
    // 防止单页过大（>4KB）影响接收端缓冲
    if (page_data_.size() >= 4000 || frames_in_page_ >= 10) {
        FlushAudioPage(false);
    }
}

void OggMuxer::FlushAudioPage(bool eos) {
    if (seg_table_.empty()) return;
    uint8_t header_type = 0x00;
    if (eos) header_type |= 0x04;  // EOS
    WritePage(header_type, granule_);
    page_data_.clear();
    seg_table_.clear();
    frames_in_page_ = 0;
}

void OggMuxer::WritePage(uint8_t header_type, int64_t granule) {
    // OGG 页头结构（27 字节 + 段表）
    // Capture pattern "OggS" (4)
    // Version (1) = 0
    // Header type (1): 0x02=BOS, 0x04=EOS, 0x01=continuation
    // Granule position (8 LE)
    // Serial number (4 LE)
    // Page sequence (4 LE)
    // CRC (4 LE) — 占位，后面回填
    // Number of segments (1)
    // Segment table (N bytes)

    size_t page_start = out_.size();
    // header
    EmitBytes("OggS", 4);
    uint8_t version = 0;
    EmitBytes(&version, 1);
    EmitBytes(&header_type, 1);
    // granule 64-bit LE
    uint64_t g = (uint64_t)granule;
    for (int i = 0; i < 8; i++) {
        out_.push_back((uint8_t)((g >> (i * 8)) & 0xFF));
    }
    // serial
    PutU32LE(out_, serial_);
    // page sequence
    PutU32LE(out_, (uint32_t)page_seq_++);
    // CRC 占位（先 0）
    size_t crc_pos = out_.size();
    uint32_t crc_placeholder = 0;
    PutU32LE(out_, crc_placeholder);
    // number of segments
    uint8_t n_seg = (uint8_t)seg_table_.size();
    EmitBytes(&n_seg, 1);
    // segment table
    if (!seg_table_.empty()) {
        EmitBytes(seg_table_.data(), seg_table_.size());
    }
    // page data
    if (!page_data_.empty()) {
        EmitBytes(page_data_.data(), page_data_.size());
    }

    // 回填 CRC（CRC 计算范围：从 "OggS" 起到页尾，CRC 字段本身为 0）
    size_t page_end = out_.size();
    uint32_t crc = Crc32(out_.data() + page_start, page_end - page_start);
    out_[crc_pos + 0] = (uint8_t)(crc & 0xFF);
    out_[crc_pos + 1] = (uint8_t)((crc >> 8) & 0xFF);
    out_[crc_pos + 2] = (uint8_t)((crc >> 16) & 0xFF);
    out_[crc_pos + 3] = (uint8_t)((crc >> 24) & 0xFF);
}

std::vector<uint8_t> OggMuxer::Finalize() {
    if (!started_) return out_;
    // 刷出剩余音频帧（如有），标记 EOS
    if (!seg_table_.empty()) {
        FlushAudioPage(true);
    } else {
        // 没有音频帧时，构造一个空 EOS 页
        // granule 保持当前值，header_type = EOS
        // 但 seg_table 为空，OGG 规范允许空页
        WritePage(0x04 /* EOS */, granule_);
    }
    started_ = false;
    return out_;
}
