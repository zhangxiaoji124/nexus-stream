#pragma once

#include <cstdint>
#include <vector>

#include "nexus/core/media_frame.hpp"

namespace nexus::codec {

/**
 * @brief Generates standards-compliant H.264 Annex-B bitstreams (SPS/PPS/IDR/P)
 *        and AAC ADTS audio frames for zero-dependency live streaming, benchmarking,
 *        and unit/integration testing.
 */
class TestStreamGenerator {
public:
    /**
     * @brief Encodes an ITU-T H.264 SPS NALU (with NALU header 0x67, without start code)
     *        for the requested resolution and frame rate using Exp-Golomb coding.
     */
    static std::vector<uint8_t> BuildSpsNalu(
        uint32_t width = 1280,
        uint32_t height = 720,
        uint32_t fps = 30,
        uint8_t profile_idc = 100,  // High Profile
        uint8_t level_idc = 31);    // Level 3.1

    /**
     * @brief Builds a standard H.264 PPS NALU (without start code).
     */
    static std::vector<uint8_t> BuildPpsNalu();

    /**
     * @brief Builds a synthetic H.264 IDR Slice (type 5) or Non-IDR P-Slice (type 1) NALU.
     *        Default IDR size is 4200 bytes (> 1400 MTU) to exercise RFC 6184 FU-A fragmentation.
     */
    static std::vector<uint8_t> BuildVideoSliceNalu(
        bool is_idr,
        uint32_t frame_num,
        size_t payload_size = 0);

    /**
     * @brief Wraps a raw NALU with 4-byte Annex-B start code (00 00 00 01).
     */
    static std::vector<uint8_t> WrapAnnexB(std::span<const uint8_t> raw_nalu);

    /**
     * @brief Builds a raw AAC frame (without ADTS header) or full ADTS frame.
     */
    static std::vector<uint8_t> BuildRawAacFrame(uint32_t frame_index, size_t size = 180);
    static std::vector<uint8_t> BuildAdtsAacFrame(
        uint32_t frame_index,
        uint32_t sample_rate = 44100,
        uint8_t channels = 2,
        size_t raw_size = 180);
};

}  // namespace nexus::codec
