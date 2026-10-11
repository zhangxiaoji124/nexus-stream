#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "nexus/core/media_frame.hpp"

namespace nexus::protocol {

struct SdpSessionInfo {
    std::string session_name = "NexusStream Live Session";
    bool has_video = false;
    bool has_audio = false;
    core::VideoTrackInfo video_track;
    core::AudioTrackInfo audio_track;
    std::string video_control = "trackID=0";
    std::string audio_control = "trackID=1";
};

class SdpCodec {
public:
    static std::string Base64Encode(std::span<const uint8_t> data);
    static std::vector<uint8_t> Base64Decode(std::string_view encoded);
    static std::string HexEncode(std::span<const uint8_t> data);
    static std::vector<uint8_t> HexDecode(std::string_view hex);

    /**
     * @brief Generates an RFC 4566 / RFC 6184 / RFC 3640 compliant SDP string
     *        for RTSP DESCRIBE responses or ANNOUNCE requests.
     */
    static std::string Generate(
        const std::optional<core::VideoTrackInfo>& video,
        const std::optional<core::AudioTrackInfo>& audio,
        std::string_view server_ip = "0.0.0.0",
        std::string_view session_name = "NexusStream");

    /**
     * @brief Parses an SDP text body and extracts H.264 SPS/PPS and AAC AudioSpecificConfig.
     */
    static SdpSessionInfo Parse(std::string_view sdp_text);
};

}  // namespace nexus::protocol
