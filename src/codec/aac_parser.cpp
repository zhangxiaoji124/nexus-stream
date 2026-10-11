#include "nexus/codec/aac_parser.hpp"

#include <array>

namespace nexus::codec {

namespace {
constexpr std::array<uint32_t, 13> kSampleRateTable = {
    96000, 88200, 64000, 48000, 44100, 32000,
    24000, 22050, 16000, 12000, 11025, 8000, 7350
};
}  // namespace

uint32_t AacParser::FreqIndexToSampleRate(uint8_t freq_index) noexcept {
    if (freq_index < kSampleRateTable.size()) {
        return kSampleRateTable[freq_index];
    }
    return 44100;
}

uint8_t AacParser::SampleRateToFreqIndex(uint32_t sample_rate) noexcept {
    for (size_t i = 0; i < kSampleRateTable.size(); ++i) {
        if (kSampleRateTable[i] == sample_rate) {
            return static_cast<uint8_t>(i);
        }
    }
    return 4;  // default 44100 Hz
}

std::optional<AdtsHeaderInfo> AacParser::ParseAdtsHeader(std::span<const uint8_t> data) {
    if (data.size() < 7) return std::nullopt;
    // Syncword: 12 bits of 1s (0xFFF)
    if (data[0] != 0xFF || (data[1] & 0xF0) != 0xF0) {
        return std::nullopt;
    }

    bool protection_absent = (data[1] & 0x01) != 0;
    uint8_t header_size = protection_absent ? 7 : 9;
    if (data.size() < header_size) return std::nullopt;

    AdtsHeaderInfo info{};
    info.header_size = header_size;
    info.profile = static_cast<uint8_t>(((data[2] >> 6) & 0x03) + 1);
    info.sampling_freq_index = static_cast<uint8_t>((data[2] >> 2) & 0x0F);
    info.sample_rate = FreqIndexToSampleRate(info.sampling_freq_index);
    info.channel_config = static_cast<uint8_t>(((data[2] & 0x01) << 2) | ((data[3] >> 6) & 0x03));
    info.frame_length = static_cast<uint16_t>(
        ((static_cast<uint16_t>(data[3] & 0x03)) << 11) |
        (static_cast<uint16_t>(data[4]) << 3) |
        ((data[5] >> 5) & 0x07));

    if (info.frame_length < header_size) {
        return std::nullopt;
    }
    return info;
}

std::vector<AacFrameView> AacParser::SplitAdtsFrames(std::span<const uint8_t> adts_stream) {
    std::vector<AacFrameView> frames;
    size_t offset = 0;
    while (offset + 7 <= adts_stream.size()) {
        auto rest = adts_stream.subspan(offset);
        auto hdr = ParseAdtsHeader(rest);
        if (!hdr || hdr->frame_length > rest.size()) {
            break;
        }
        AacFrameView view;
        view.header = *hdr;
        view.raw_aac = rest.subspan(hdr->header_size, hdr->frame_length - hdr->header_size);
        frames.push_back(view);
        offset += hdr->frame_length;
    }
    return frames;
}

std::vector<uint8_t> AacParser::BuildAdtsHeader(
    uint8_t audio_object_type,
    uint32_t sample_rate,
    uint8_t channels,
    size_t raw_aac_size) {
    uint8_t freq_idx = SampleRateToFreqIndex(sample_rate);
    uint8_t profile = (audio_object_type > 0) ? (audio_object_type - 1) : 1;
    uint16_t full_len = static_cast<uint16_t>(raw_aac_size + 7);

    std::vector<uint8_t> adts(7);
    adts[0] = 0xFF;
    adts[1] = 0xF1;  // MPEG-4, Layer 0, protection_absent = 1
    adts[2] = static_cast<uint8_t>(((profile & 0x03) << 6) |
                                   ((freq_idx & 0x0F) << 2) |
                                   ((channels >> 2) & 0x01));
    adts[3] = static_cast<uint8_t>(((channels & 0x03) << 6) | ((full_len >> 11) & 0x03));
    adts[4] = static_cast<uint8_t>((full_len >> 3) & 0xFF);
    adts[5] = static_cast<uint8_t>(((full_len & 0x07) << 5) | 0x1F);
    adts[6] = 0xFC;
    return adts;
}

std::vector<uint8_t> AacParser::BuildAudioSpecificConfig(
    uint8_t audio_object_type,
    uint32_t sample_rate,
    uint8_t channels) {
    uint8_t freq_idx = SampleRateToFreqIndex(sample_rate);
    uint8_t b0 = static_cast<uint8_t>(((audio_object_type & 0x1F) << 3) | ((freq_idx >> 1) & 0x07));
    uint8_t b1 = static_cast<uint8_t>(((freq_idx & 0x01) << 7) | ((channels & 0x0F) << 3));
    return {b0, b1};
}

std::optional<AdtsHeaderInfo> AacParser::ParseAudioSpecificConfig(std::span<const uint8_t> asc) {
    if (asc.size() < 2) return std::nullopt;
    AdtsHeaderInfo info{};
    info.profile = static_cast<uint8_t>((asc[0] >> 3) & 0x1F);
    info.sampling_freq_index = static_cast<uint8_t>(((asc[0] & 0x07) << 1) | ((asc[1] >> 7) & 0x01));
    info.sample_rate = FreqIndexToSampleRate(info.sampling_freq_index);
    info.channel_config = static_cast<uint8_t>((asc[1] >> 3) & 0x0F);
    return info;
}

}  // namespace nexus::codec
