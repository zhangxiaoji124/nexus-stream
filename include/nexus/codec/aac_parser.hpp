#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nexus::codec {

struct AdtsHeaderInfo {
    uint8_t profile = 2;             // Audio Object Type: 2 = AAC-LC
    uint8_t sampling_freq_index = 4; // 4 = 44100 Hz, 3 = 48000 Hz
    uint32_t sample_rate = 44100;
    uint8_t channel_config = 2;      // 1 = Mono, 2 = Stereo
    uint16_t frame_length = 0;       // Includes ADTS header length (7 or 9 bytes)
    uint8_t header_size = 7;         // 7 if protection_absent == 1, else 9
};

struct AacFrameView {
    AdtsHeaderInfo header;
    std::span<const uint8_t> raw_aac;  // AAC payload WITHOUT 7/9-byte ADTS header
};

class AacParser {
public:
    static uint32_t FreqIndexToSampleRate(uint8_t freq_index) noexcept;
    static uint8_t SampleRateToFreqIndex(uint32_t sample_rate) noexcept;

    /**
     * @brief Parses a 7-byte (or 9-byte with CRC) ADTS header at the start of `data`.
     */
    static std::optional<AdtsHeaderInfo> ParseAdtsHeader(std::span<const uint8_t> data);

    /**
     * @brief Splits a contiguous stream of ADTS frames into individual raw AAC payloads.
     */
    static std::vector<AacFrameView> SplitAdtsFrames(std::span<const uint8_t> adts_stream);

    /**
     * @brief Builds a 7-byte ADTS header for a raw AAC frame of size `raw_aac_size`.
     */
    static std::vector<uint8_t> BuildAdtsHeader(
        uint8_t audio_object_type,
        uint32_t sample_rate,
        uint8_t channels,
        size_t raw_aac_size);

    /**
     * @brief Builds the 2-byte ISO/IEC 14496-3 AudioSpecificConfig (ASC) used in
     *        FLV Audio Sequence Header, MP4 esds box, and SDP config= attribute.
     *        Layout: [5 bits audioObjectType][4 bits freqIndex][4 bits channels][3 bits 0]
     */
    static std::vector<uint8_t> BuildAudioSpecificConfig(
        uint8_t audio_object_type,
        uint32_t sample_rate,
        uint8_t channels);

    /**
     * @brief Parses a 2-byte AudioSpecificConfig back into AdtsHeaderInfo metadata.
     */
    static std::optional<AdtsHeaderInfo> ParseAudioSpecificConfig(std::span<const uint8_t> asc);
};

}  // namespace nexus::codec
