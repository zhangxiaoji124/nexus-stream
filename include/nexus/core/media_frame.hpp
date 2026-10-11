#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace nexus::core {

enum class CodecType : uint8_t {
    kUnknown = 0,
    kH264 = 1,
    kH265 = 2,
    kAAC = 3,
    kOpus = 4,
    kPCMA = 5
};

enum class FrameType : uint8_t {
    kUnknown = 0,
    kKeyFrame = 1,     // IDR frame for H.264
    kInterFrame = 2,   // P / B frame
    kConfigFrame = 3,  // SPS / PPS / AudioSpecificConfig
    kAudioFrame = 4
};

/**
 * @brief Immutable ref-counted media frame so a single encoded frame can be
 *        broadcast to hundreds of RTSP / HTTP-FLV / RTMP subscribers with zero payload copy.
 */
struct MediaFrame {
    CodecType codec = CodecType::kUnknown;
    FrameType frame_type = FrameType::kUnknown;

    uint64_t pts_ms = 0;         // Presentation Timestamp (ms)
    uint64_t dts_ms = 0;         // Decoding Timestamp (ms)
    uint32_t rtp_timestamp = 0;  // 90kHz clock for video, sample_rate clock for audio
    uint32_t sequence_id = 0;

    // Shared immutable payload (e.g., raw H.264 NALU without start code, or raw AAC frame without ADTS)
    std::shared_ptr<const std::vector<uint8_t>> payload;

    static MediaFrame Create(CodecType c, FrameType ft, uint64_t pts, uint64_t dts,
                             std::span<const uint8_t> data, uint32_t clock_rate = 90000) {
        MediaFrame f;
        f.codec = c;
        f.frame_type = ft;
        f.pts_ms = pts;
        f.dts_ms = dts;
        f.rtp_timestamp = static_cast<uint32_t>((pts * clock_rate) / 1000);
        f.payload = std::make_shared<const std::vector<uint8_t>>(data.begin(), data.end());
        return f;
    }

    std::span<const uint8_t> Data() const noexcept {
        if (!payload) return {};
        return {payload->data(), payload->size()};
    }

    size_t Size() const noexcept { return payload ? payload->size() : 0; }
    bool IsVideo() const noexcept { return codec == CodecType::kH264 || codec == CodecType::kH265; }
    bool IsAudio() const noexcept { return codec == CodecType::kAAC || codec == CodecType::kOpus || codec == CodecType::kPCMA; }
    bool IsKeyFrame() const noexcept { return frame_type == FrameType::kKeyFrame; }
};

struct VideoTrackInfo {
    CodecType codec = CodecType::kH264;
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t fps = 30;
    uint8_t profile_idc = 0x42;
    uint8_t profile_iop = 0xE0;
    uint8_t level_idc = 0x1F;
    std::vector<uint8_t> sps;  // Raw SPS NALU (without Annex-B start code)
    std::vector<uint8_t> pps;  // Raw PPS NALU (without Annex-B start code)
};

struct AudioTrackInfo {
    CodecType codec = CodecType::kAAC;
    uint32_t sample_rate = 44100;
    uint8_t channels = 2;
    uint8_t profile = 2;  // 2 = AAC-LC
    std::vector<uint8_t> audio_specific_config;  // 2-byte ISO 14496-3 ASC
};

}  // namespace nexus::core
