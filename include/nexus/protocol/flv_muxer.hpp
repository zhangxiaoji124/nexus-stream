#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "nexus/core/media_frame.hpp"

namespace nexus::protocol {

enum class FlvTagType : uint8_t {
    kAudio = 8,
    kVideo = 9,
    kScriptData = 18
};

struct FlvTagView {
    FlvTagType type = FlvTagType::kVideo;
    uint32_t timestamp_ms = 0;
    bool is_sequence_header = false;
    bool is_keyframe = false;
    std::span<const uint8_t> tag_body;  // Full tag payload including 1-byte FLV media header
};

/**
 * @brief Adobe FLV / HTTP-FLV container muxer and demuxer supporting H.264 (AVC) and AAC.
 */
class FlvMuxer {
public:
    /**
     * @brief Builds the 9-byte FLV header + 4-byte PreviousTagSize0 (13 bytes total).
     */
    static std::vector<uint8_t> BuildFlvHeader(bool has_video = true, bool has_audio = true);

    /**
     * @brief Builds an FLV Video Sequence Header tag (AVCPacketType = 0) containing
     *        the AVCDecoderConfigurationRecord derived from SPS and PPS.
     */
    static std::vector<uint8_t> BuildVideoSequenceHeaderTag(
        std::span<const uint8_t> sps,
        std::span<const uint8_t> pps,
        uint32_t timestamp_ms = 0);

    /**
     * @brief Builds an FLV Audio Sequence Header tag (AACPacketType = 0) containing
     *        the 2-byte ISO 14496-3 AudioSpecificConfig.
     */
    static std::vector<uint8_t> BuildAudioSequenceHeaderTag(
        std::span<const uint8_t> audio_specific_config,
        uint32_t timestamp_ms = 0);

    /**
     * @brief Builds an FLV Video NALU tag (AVCPacketType = 1) from a raw H.264 NALU
     *        (automatically prepends the 4-byte big-endian AVCC NALU length).
     */
    static std::vector<uint8_t> BuildVideoNaluTag(
        std::span<const uint8_t> raw_nalu,
        bool is_keyframe,
        uint32_t dts_ms,
        uint32_t pts_ms);

    /**
     * @brief Builds an FLV Audio Raw AAC tag (AACPacketType = 1) from a raw AAC frame.
     */
    static std::vector<uint8_t> BuildAudioRawTag(
        std::span<const uint8_t> raw_aac,
        uint32_t timestamp_ms);

    /**
     * @brief Wraps a MediaFrame directly into an FLV Tag.
     */
    static std::vector<uint8_t> MuxFrame(const core::MediaFrame& frame);

    /**
     * @brief Parses a complete FLV byte stream (starting with 'FLV' header or raw tags)
     *        into individual FlvTagViews for inspection and verification.
     */
    static std::vector<FlvTagView> ParseFlvStream(std::span<const uint8_t> flv_bytes);

private:
    static std::vector<uint8_t> WrapFlvTag(
        FlvTagType type,
        uint32_t timestamp_ms,
        std::span<const uint8_t> body);
};

}  // namespace nexus::protocol
