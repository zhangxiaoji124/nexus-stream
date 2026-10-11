#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace nexus::protocol {

struct RtspTransportInfo {
    bool is_tcp_interleaved = true;
    uint8_t rtp_channel = 0;
    uint8_t rtcp_channel = 1;
    uint16_t client_rtp_port = 0;
    uint16_t client_rtcp_port = 0;
    std::string mode = "PLAY";  // "PLAY" or "RECORD"
};

struct RtspRequest {
    std::string method;
    std::string url;
    std::string version = "RTSP/1.0";
    uint32_t cseq = 0;
    std::string session;
    std::map<std::string, std::string> headers;
    std::string body;

    std::string GetHeader(std::string_view key) const;
    std::string ExtractStreamPath() const;
    int ExtractTrackId() const;
    RtspTransportInfo ParseTransport() const;

    std::string Serialize() const;
    static std::optional<RtspRequest> Parse(std::string_view raw_text, size_t* consumed_bytes = nullptr);
};

struct RtspResponse {
    std::string version = "RTSP/1.0";
    int status_code = 200;
    std::string reason = "OK";
    uint32_t cseq = 0;
    std::string session;
    std::map<std::string, std::string> headers;
    std::string body;

    std::string Serialize() const;
    static std::optional<RtspResponse> Parse(std::string_view raw_text, size_t* consumed_bytes = nullptr);
};

}  // namespace nexus::protocol
