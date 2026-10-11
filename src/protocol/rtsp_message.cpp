#include "nexus/protocol/rtsp_message.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace nexus::protocol {

namespace {

std::string ToLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string Trim(std::string_view s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return std::string(s.substr(start, end - start));
}

}  // namespace

std::string RtspRequest::GetHeader(std::string_view key) const {
    std::string low_key = ToLower(key);
    for (const auto& [k, v] : headers) {
        if (ToLower(k) == low_key) {
            return v;
        }
    }
    return {};
}

std::string RtspRequest::ExtractStreamPath() const {
    std::string path = url;
    auto scheme = path.find("://");
    if (scheme != std::string::npos) {
        auto slash = path.find('/', scheme + 3);
        path = (slash != std::string::npos) ? path.substr(slash) : "/";
    }
    // Strip trailing /trackID=N if present
    auto track_pos = path.find("/trackID=");
    if (track_pos != std::string::npos) {
        path = path.substr(0, track_pos);
    }
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path.empty() ? "/" : path;
}

int RtspRequest::ExtractTrackId() const {
    auto track_pos = url.find("trackID=");
    if (track_pos != std::string::npos) {
        return std::atoi(url.c_str() + track_pos + 8);
    }
    return 0;
}

RtspTransportInfo RtspRequest::ParseTransport() const {
    RtspTransportInfo info{};
    std::string tr = GetHeader("Transport");
    if (tr.empty()) return info;

    info.is_tcp_interleaved = (tr.find("TCP") != std::string::npos ||
                               tr.find("interleaved=") != std::string::npos);
    if (tr.find("mode=record") != std::string::npos ||
        tr.find("mode=RECORD") != std::string::npos ||
        tr.find("mode=\"RECORD\"") != std::string::npos) {
        info.mode = "RECORD";
    }

    auto il_pos = tr.find("interleaved=");
    if (il_pos != std::string::npos) {
        int ch0 = 0, ch1 = 1;
        if (std::sscanf(tr.c_str() + il_pos + 12, "%d-%d", &ch0, &ch1) >= 1) {
            info.rtp_channel = static_cast<uint8_t>(ch0);
            info.rtcp_channel = static_cast<uint8_t>(ch1);
        }
    }

    auto cp_pos = tr.find("client_port=");
    if (cp_pos != std::string::npos) {
        int p0 = 0, p1 = 0;
        if (std::sscanf(tr.c_str() + cp_pos + 12, "%d-%d", &p0, &p1) >= 1) {
            info.client_rtp_port = static_cast<uint16_t>(p0);
            info.client_rtcp_port = static_cast<uint16_t>(p1 > 0 ? p1 : p0 + 1);
            info.is_tcp_interleaved = false;
        }
    }

    return info;
}

std::string RtspRequest::Serialize() const {
    std::ostringstream oss;
    oss << method << " " << url << " " << version << "\r\n";
    oss << "CSeq: " << cseq << "\r\n";
    if (!session.empty()) {
        oss << "Session: " << session << "\r\n";
    }
    for (const auto& [k, v] : headers) {
        std::string lk = ToLower(k);
        if (lk == "cseq" || lk == "session" || lk == "content-length") continue;
        oss << k << ": " << v << "\r\n";
    }
    if (!body.empty()) {
        oss << "Content-Length: " << body.size() << "\r\n";
    }
    oss << "\r\n" << body;
    return oss.str();
}

std::optional<RtspRequest> RtspRequest::Parse(std::string_view raw_text, size_t* consumed_bytes) {
    auto hdr_end = raw_text.find("\r\n\r\n");
    if (hdr_end == std::string_view::npos) {
        return std::nullopt;
    }

    RtspRequest req;
    std::string_view header_block = raw_text.substr(0, hdr_end);
    size_t line_end = header_block.find("\r\n");
    std::string_view first_line = header_block.substr(0, line_end);

    {
        std::istringstream iss{std::string(first_line)};
        if (!(iss >> req.method >> req.url >> req.version)) {
            return std::nullopt;
        }
    }

    size_t pos = (line_end == std::string_view::npos) ? header_block.size() : line_end + 2;
    size_t content_len = 0;

    while (pos < header_block.size()) {
        size_t next = header_block.find("\r\n", pos);
        if (next == std::string_view::npos) next = header_block.size();
        std::string_view line = header_block.substr(pos, next - pos);
        pos = next + 2;

        auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        std::string key = Trim(line.substr(0, colon));
        std::string val = Trim(line.substr(colon + 1));
        std::string low_key = ToLower(key);

        if (low_key == "cseq") {
            req.cseq = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
        } else if (low_key == "session") {
            auto semi = val.find(';');
            req.session = (semi != std::string::npos) ? val.substr(0, semi) : val;
        } else if (low_key == "content-length") {
            content_len = std::strtoull(val.c_str(), nullptr, 10);
        }
        req.headers[key] = val;
    }

    size_t total_needed = hdr_end + 4 + content_len;
    if (raw_text.size() < total_needed) {
        return std::nullopt;
    }

    if (content_len > 0) {
        req.body = std::string(raw_text.substr(hdr_end + 4, content_len));
    }
    if (consumed_bytes) {
        *consumed_bytes = total_needed;
    }
    return req;
}

std::string RtspResponse::Serialize() const {
    std::ostringstream oss;
    oss << version << " " << status_code << " " << reason << "\r\n";
    oss << "CSeq: " << cseq << "\r\n";
    oss << "Server: NexusStream/1.0\r\n";
    if (!session.empty()) {
        oss << "Session: " << session << "\r\n";
    }
    for (const auto& [k, v] : headers) {
        std::string lk = ToLower(k);
        if (lk == "cseq" || lk == "session" || lk == "content-length" || lk == "server") continue;
        oss << k << ": " << v << "\r\n";
    }
    if (!body.empty()) {
        oss << "Content-Length: " << body.size() << "\r\n";
    }
    oss << "\r\n" << body;
    return oss.str();
}

std::optional<RtspResponse> RtspResponse::Parse(std::string_view raw_text, size_t* consumed_bytes) {
    auto hdr_end = raw_text.find("\r\n\r\n");
    if (hdr_end == std::string_view::npos) {
        return std::nullopt;
    }

    RtspResponse resp;
    std::string_view header_block = raw_text.substr(0, hdr_end);
    size_t line_end = header_block.find("\r\n");
    std::string_view first_line = header_block.substr(0, line_end);

    {
        std::istringstream iss{std::string(first_line)};
        if (!(iss >> resp.version >> resp.status_code)) {
            return std::nullopt;
        }
        std::getline(iss, resp.reason);
        resp.reason = Trim(resp.reason);
    }

    size_t pos = (line_end == std::string_view::npos) ? header_block.size() : line_end + 2;
    size_t content_len = 0;

    while (pos < header_block.size()) {
        size_t next = header_block.find("\r\n", pos);
        if (next == std::string_view::npos) next = header_block.size();
        std::string_view line = header_block.substr(pos, next - pos);
        pos = next + 2;

        auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        std::string key = Trim(line.substr(0, colon));
        std::string val = Trim(line.substr(colon + 1));
        std::string low_key = ToLower(key);

        if (low_key == "cseq") {
            resp.cseq = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
        } else if (low_key == "session") {
            auto semi = val.find(';');
            resp.session = (semi != std::string::npos) ? val.substr(0, semi) : val;
        } else if (low_key == "content-length") {
            content_len = std::strtoull(val.c_str(), nullptr, 10);
        }
        resp.headers[key] = val;
    }

    size_t total_needed = hdr_end + 4 + content_len;
    if (raw_text.size() < total_needed) {
        return std::nullopt;
    }

    if (content_len > 0) {
        resp.body = std::string(raw_text.substr(hdr_end + 4, content_len));
    }
    if (consumed_bytes) {
        *consumed_bytes = total_needed;
    }
    return resp;
}

}  // namespace nexus::protocol
