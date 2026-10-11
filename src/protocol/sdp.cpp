#include "nexus/protocol/sdp.hpp"
#include "nexus/codec/aac_parser.hpp"
#include "nexus/codec/h264_parser.hpp"

#include <iomanip>
#include <sstream>

namespace nexus::protocol {

namespace {
constexpr std::string_view kBase64Chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}  // namespace

std::string SdpCodec::Base64Encode(std::span<const uint8_t> data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < data.size()) n |= static_cast<uint32_t>(data[i + 2]);

        out.push_back(kBase64Chars[(n >> 18) & 0x3F]);
        out.push_back(kBase64Chars[(n >> 12) & 0x3F]);
        out.push_back((i + 1 < data.size()) ? kBase64Chars[(n >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < data.size()) ? kBase64Chars[n & 0x3F] : '=');
    }
    return out;
}

std::vector<uint8_t> SdpCodec::Base64Decode(std::string_view encoded) {
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; ++i) {
        T[static_cast<uint8_t>(kBase64Chars[i])] = i;
    }
    std::vector<uint8_t> out;
    int val = 0, valb = -8;
    for (char c : encoded) {
        int d = T[static_cast<uint8_t>(c)];
        if (d == -1) {
            if (c == '=') break;
            continue;
        }
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string SdpCodec::HexEncode(std::span<const uint8_t> data) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');
    for (uint8_t b : data) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

std::vector<uint8_t> SdpCodec::HexDecode(std::string_view hex) {
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        int hi = hex_val(hex[i]);
        int lo = hex_val(hex[i + 1]);
        if (hi >= 0 && lo >= 0) {
            out.push_back(static_cast<uint8_t>((hi << 4) | lo));
        }
    }
    return out;
}

std::string SdpCodec::Generate(
    const std::optional<core::VideoTrackInfo>& video,
    const std::optional<core::AudioTrackInfo>& audio,
    std::string_view server_ip,
    std::string_view session_name) {
    std::ostringstream oss;
    oss << "v=0\r\n"
        << "o=- 0 0 IN IP4 " << server_ip << "\r\n"
        << "s=" << session_name << "\r\n"
        << "c=IN IP4 " << server_ip << "\r\n"
        << "t=0 0\r\n"
        << "a=tool:NexusStream/1.0\r\n"
        << "a=control:*\r\n";

    if (video) {
        uint8_t profile_idc = video->profile_idc;
        uint8_t profile_iop = video->profile_iop;
        uint8_t level_idc = video->level_idc;
        if (video->sps.size() >= 4) {
            profile_idc = video->sps[1];
            profile_iop = video->sps[2];
            level_idc = video->sps[3];
        }
        uint8_t pl_bytes[3] = {profile_idc, profile_iop, level_idc};
        std::string profile_level_id = HexEncode(pl_bytes);

        oss << "m=video 0 RTP/AVP 96\r\n"
            << "a=rtpmap:96 H264/90000\r\n"
            << "a=fmtp:96 packetization-mode=1;profile-level-id=" << profile_level_id;
        if (!video->sps.empty() && !video->pps.empty()) {
            oss << ";sprop-parameter-sets=" << Base64Encode(video->sps)
                << "," << Base64Encode(video->pps);
        }
        oss << "\r\n"
            << "a=control:trackID=0\r\n";
    }

    if (audio) {
        std::vector<uint8_t> asc = audio->audio_specific_config;
        if (asc.empty()) {
            asc = codec::AacParser::BuildAudioSpecificConfig(
                audio->profile, audio->sample_rate, audio->channels);
        }
        oss << "m=audio 0 RTP/AVP 97\r\n"
            << "a=rtpmap:97 mpeg4-generic/" << audio->sample_rate << "/"
            << static_cast<int>(audio->channels) << "\r\n"
            << "a=fmtp:97 profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;"
               "indexdeltalength=3;config=" << HexEncode(asc) << "\r\n"
            << "a=control:trackID=1\r\n";
    }

    return oss.str();
}

SdpSessionInfo SdpCodec::Parse(std::string_view sdp_text) {
    SdpSessionInfo info;
    enum class Section { kGlobal, kVideo, kAudio };
    Section current = Section::kGlobal;

    size_t pos = 0;
    while (pos < sdp_text.size()) {
        size_t end = sdp_text.find('\n', pos);
        if (end == std::string_view::npos) end = sdp_text.size();
        std::string_view line = sdp_text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        pos = end + 1;

        if (line.rfind("s=", 0) == 0) {
            info.session_name = std::string(line.substr(2));
        } else if (line.rfind("m=video", 0) == 0) {
            current = Section::kVideo;
            info.has_video = true;
        } else if (line.rfind("m=audio", 0) == 0) {
            current = Section::kAudio;
            info.has_audio = true;
        } else if (line.rfind("a=control:", 0) == 0) {
            std::string ctrl(line.substr(10));
            if (current == Section::kVideo) {
                info.video_control = ctrl;
            } else if (current == Section::kAudio) {
                info.audio_control = ctrl;
            }
        } else if (line.rfind("a=fmtp:", 0) == 0) {
            if (current == Section::kVideo) {
                auto sprop_pos = line.find("sprop-parameter-sets=");
                if (sprop_pos != std::string_view::npos) {
                    auto val = line.substr(sprop_pos + 21);
                    auto semi = val.find(';');
                    if (semi != std::string_view::npos) val = val.substr(0, semi);
                    auto comma = val.find(',');
                    if (comma != std::string_view::npos) {
                        info.video_track.sps = Base64Decode(val.substr(0, comma));
                        info.video_track.pps = Base64Decode(val.substr(comma + 1));
                        auto sps_parsed = codec::H264Parser::ParseSps(info.video_track.sps);
                        if (sps_parsed) {
                            info.video_track.width = sps_parsed->width;
                            info.video_track.height = sps_parsed->height;
                            if (sps_parsed->fps > 0) info.video_track.fps = sps_parsed->fps;
                            info.video_track.profile_idc = sps_parsed->profile_idc;
                            info.video_track.level_idc = sps_parsed->level_idc;
                        }
                    }
                }
            } else if (current == Section::kAudio) {
                auto cfg_pos = line.find("config=");
                if (cfg_pos != std::string_view::npos) {
                    auto val = line.substr(cfg_pos + 7);
                    auto semi = val.find(';');
                    if (semi != std::string_view::npos) val = val.substr(0, semi);
                    info.audio_track.audio_specific_config = HexDecode(val);
                    auto asc_info = codec::AacParser::ParseAudioSpecificConfig(
                        info.audio_track.audio_specific_config);
                    if (asc_info) {
                        info.audio_track.profile = asc_info->profile;
                        info.audio_track.sample_rate = asc_info->sample_rate;
                        info.audio_track.channels = asc_info->channel_config;
                    }
                }
            }
        }
    }

    return info;
}

}  // namespace nexus::protocol
