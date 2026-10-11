#include "nexus/protocol/rtp_packet.hpp"
#include "nexus/codec/h264_parser.hpp"

#include <algorithm>
#include <cstring>

namespace nexus::protocol {

std::vector<uint8_t> RtpPacket::Serialize() const {
    std::vector<uint8_t> out(kRtpHeaderSize + payload.size());
    out[0] = static_cast<uint8_t>(
        ((header.version & 0x03) << 6) |
        (header.padding ? 0x20 : 0x00) |
        (header.extension ? 0x10 : 0x00) |
        (header.csrc_count & 0x0F));
    out[1] = static_cast<uint8_t>(
        (header.marker ? 0x80 : 0x00) |
        (header.payload_type & 0x7F));
    out[2] = static_cast<uint8_t>((header.sequence >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(header.sequence & 0xFF);

    out[4] = static_cast<uint8_t>((header.timestamp >> 24) & 0xFF);
    out[5] = static_cast<uint8_t>((header.timestamp >> 16) & 0xFF);
    out[6] = static_cast<uint8_t>((header.timestamp >> 8) & 0xFF);
    out[7] = static_cast<uint8_t>(header.timestamp & 0xFF);

    out[8]  = static_cast<uint8_t>((header.ssrc >> 24) & 0xFF);
    out[9]  = static_cast<uint8_t>((header.ssrc >> 16) & 0xFF);
    out[10] = static_cast<uint8_t>((header.ssrc >> 8) & 0xFF);
    out[11] = static_cast<uint8_t>(header.ssrc & 0xFF);

    if (!payload.empty()) {
        std::memcpy(out.data() + kRtpHeaderSize, payload.data(), payload.size());
    }
    return out;
}

std::vector<uint8_t> RtpPacket::SerializeInterleaved(uint8_t channel) const {
    auto rtp = Serialize();
    uint16_t len = static_cast<uint16_t>(rtp.size());
    std::vector<uint8_t> out;
    out.reserve(4 + rtp.size());
    out.push_back('$');
    out.push_back(channel);
    out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(len & 0xFF));
    out.insert(out.end(), rtp.begin(), rtp.end());
    return out;
}

std::optional<RtpPacket> RtpPacket::Parse(std::span<const uint8_t> raw_bytes) {
    if (raw_bytes.size() < kRtpHeaderSize) {
        return std::nullopt;
    }
    RtpPacket pkt;
    pkt.header.version = (raw_bytes[0] >> 6) & 0x03;
    if (pkt.header.version != kRtpVersion) {
        return std::nullopt;
    }
    pkt.header.padding = (raw_bytes[0] & 0x20) != 0;
    pkt.header.extension = (raw_bytes[0] & 0x10) != 0;
    pkt.header.csrc_count = raw_bytes[0] & 0x0F;
    pkt.header.marker = (raw_bytes[1] & 0x80) != 0;
    pkt.header.payload_type = raw_bytes[1] & 0x7F;
    pkt.header.sequence = (static_cast<uint16_t>(raw_bytes[2]) << 8) | raw_bytes[3];
    pkt.header.timestamp = (static_cast<uint32_t>(raw_bytes[4]) << 24) |
                           (static_cast<uint32_t>(raw_bytes[5]) << 16) |
                           (static_cast<uint32_t>(raw_bytes[6]) << 8) |
                           static_cast<uint32_t>(raw_bytes[7]);
    pkt.header.ssrc = (static_cast<uint32_t>(raw_bytes[8]) << 24) |
                      (static_cast<uint32_t>(raw_bytes[9]) << 16) |
                      (static_cast<uint32_t>(raw_bytes[10]) << 8) |
                      static_cast<uint32_t>(raw_bytes[11]);

    size_t offset = kRtpHeaderSize + 4 * pkt.header.csrc_count;
    if (pkt.header.extension) {
        if (offset + 4 > raw_bytes.size()) return std::nullopt;
        uint16_t ext_words = (static_cast<uint16_t>(raw_bytes[offset + 2]) << 8) | raw_bytes[offset + 3];
        offset += 4 + 4 * ext_words;
    }
    if (offset > raw_bytes.size()) return std::nullopt;

    size_t end_pos = raw_bytes.size();
    if (pkt.header.padding && end_pos > offset) {
        uint8_t pad_len = raw_bytes.back();
        if (pad_len <= end_pos - offset) {
            end_pos -= pad_len;
        }
    }

    pkt.payload.assign(raw_bytes.begin() + offset, raw_bytes.begin() + end_pos);
    return pkt;
}

std::vector<RtpPacket> H264RtpPacketizer::PacketizeNalu(
    std::span<const uint8_t> raw_nalu,
    uint32_t rtp_timestamp,
    bool is_last_nalu_of_access_unit) {
    std::vector<RtpPacket> packets;
    if (raw_nalu.empty()) return packets;

    auto nalu_type = codec::H264Parser::GetNaluType(raw_nalu[0]);
    bool is_vcl_slice = (nalu_type == codec::H264NaluType::kIdrSlice ||
                         nalu_type == codec::H264NaluType::kNonIdrSlice);

    // Case 1: Single NAL Unit Packet (RFC 6184 Section 5.6)
    if (raw_nalu.size() <= max_payload_size_) {
        RtpPacket pkt;
        pkt.header.marker = is_vcl_slice && is_last_nalu_of_access_unit;
        pkt.header.payload_type = payload_type_;
        pkt.header.sequence = sequence_++;
        pkt.header.timestamp = rtp_timestamp;
        pkt.header.ssrc = ssrc_;
        pkt.payload.assign(raw_nalu.begin(), raw_nalu.end());
        packets.push_back(std::move(pkt));
        return packets;
    }

    // Case 2: FU-A Fragmentation Unit (RFC 6184 Section 5.8)
    uint8_t nalu_hdr = raw_nalu[0];
    uint8_t fu_indicator = (nalu_hdr & 0xE0) | static_cast<uint8_t>(codec::H264NaluType::kFuA);
    uint8_t orig_type = nalu_hdr & 0x1F;

    const size_t max_frag_data = max_payload_size_ - 2;  // 2 bytes for FU indicator + FU header
    size_t offset = 1;  // Skip original 1-byte NALU header
    const size_t total = raw_nalu.size();

    while (offset < total) {
        size_t chunk_len = std::min(max_frag_data, total - offset);
        bool is_start = (offset == 1);
        bool is_end = (offset + chunk_len == total);

        uint8_t fu_header = orig_type;
        if (is_start) fu_header |= 0x80;  // S bit
        if (is_end)   fu_header |= 0x40;  // E bit

        RtpPacket pkt;
        pkt.header.marker = is_end && is_vcl_slice && is_last_nalu_of_access_unit;
        pkt.header.payload_type = payload_type_;
        pkt.header.sequence = sequence_++;
        pkt.header.timestamp = rtp_timestamp;
        pkt.header.ssrc = ssrc_;

        pkt.payload.reserve(2 + chunk_len);
        pkt.payload.push_back(fu_indicator);
        pkt.payload.push_back(fu_header);
        pkt.payload.insert(pkt.payload.end(),
                           raw_nalu.begin() + offset,
                           raw_nalu.begin() + offset + chunk_len);
        packets.push_back(std::move(pkt));

        offset += chunk_len;
    }

    return packets;
}

std::vector<H264RtpDepacketizer::ReassembledNalu> H264RtpDepacketizer::ProcessPacket(
    const RtpPacket& pkt) {
    std::vector<ReassembledNalu> result;
    if (pkt.payload.empty()) return result;

    uint8_t first_byte = pkt.payload[0];
    uint8_t type = first_byte & 0x1F;

    // 1. Single NAL unit (types 1..23)
    if (type >= 1 && type <= 23) {
        in_fua_ = false;
        fua_buffer_.clear();
        result.push_back({pkt.header.timestamp, pkt.payload});
        return result;
    }

    // 2. STAP-A (Single-time aggregation packet, type 24)
    if (type == static_cast<uint8_t>(codec::H264NaluType::kStapA)) {
        in_fua_ = false;
        fua_buffer_.clear();
        size_t offset = 1;
        while (offset + 2 <= pkt.payload.size()) {
            uint16_t nalu_size = (static_cast<uint16_t>(pkt.payload[offset]) << 8) |
                                 pkt.payload[offset + 1];
            offset += 2;
            if (offset + nalu_size > pkt.payload.size()) break;
            std::vector<uint8_t> sub_nalu(
                pkt.payload.begin() + offset,
                pkt.payload.begin() + offset + nalu_size);
            result.push_back({pkt.header.timestamp, std::move(sub_nalu)});
            offset += nalu_size;
        }
        return result;
    }

    // 3. FU-A (Fragmentation unit A, type 28)
    if (type == static_cast<uint8_t>(codec::H264NaluType::kFuA)) {
        if (pkt.payload.size() < 2) return result;
        uint8_t fu_indicator = pkt.payload[0];
        uint8_t fu_header = pkt.payload[1];
        bool is_start = (fu_header & 0x80) != 0;
        bool is_end = (fu_header & 0x40) != 0;
        uint8_t orig_nalu_hdr = (fu_indicator & 0xE0) | (fu_header & 0x1F);

        if (is_start) {
            fua_buffer_.clear();
            fua_buffer_.push_back(orig_nalu_hdr);
            fua_buffer_.insert(fua_buffer_.end(),
                               pkt.payload.begin() + 2,
                               pkt.payload.end());
            fua_timestamp_ = pkt.header.timestamp;
            in_fua_ = true;
        } else if (in_fua_ && fua_timestamp_ == pkt.header.timestamp) {
            fua_buffer_.insert(fua_buffer_.end(),
                               pkt.payload.begin() + 2,
                               pkt.payload.end());
            if (is_end) {
                result.push_back({fua_timestamp_, std::move(fua_buffer_)});
                fua_buffer_.clear();
                in_fua_ = false;
            }
        } else {
            in_fua_ = false;
            fua_buffer_.clear();
        }
    }

    return result;
}

RtpPacket AacRtpCodec::PacketizeRawAac(std::span<const uint8_t> raw_aac, uint32_t rtp_timestamp) {
    RtpPacket pkt;
    pkt.header.marker = true;
    pkt.header.payload_type = payload_type_;
    pkt.header.sequence = sequence_++;
    pkt.header.timestamp = rtp_timestamp;
    pkt.header.ssrc = ssrc_;

    // RFC 3640 AAC-hbr 4-byte AU header section:
    // 16 bits AU-headers-length = 16 (0x00, 0x10)
    // 16 bits AU-header = [13 bits frame size][3 bits AU-Index=0]
    uint16_t au_header = static_cast<uint16_t>((raw_aac.size() & 0x1FFF) << 3);
    pkt.payload.reserve(4 + raw_aac.size());
    pkt.payload.push_back(0x00);
    pkt.payload.push_back(0x10);
    pkt.payload.push_back(static_cast<uint8_t>((au_header >> 8) & 0xFF));
    pkt.payload.push_back(static_cast<uint8_t>(au_header & 0xFF));
    pkt.payload.insert(pkt.payload.end(), raw_aac.begin(), raw_aac.end());
    return pkt;
}

std::optional<std::vector<uint8_t>> AacRtpCodec::DepacketizeRawAac(const RtpPacket& pkt) {
    if (pkt.payload.size() < 4) return std::nullopt;
    uint16_t au_headers_bits = (static_cast<uint16_t>(pkt.payload[0]) << 8) | pkt.payload[1];
    size_t au_headers_bytes = (au_headers_bits + 7) / 8;
    if (2 + au_headers_bytes > pkt.payload.size() || au_headers_bytes < 2) {
        return std::nullopt;
    }
    uint16_t au_header = (static_cast<uint16_t>(pkt.payload[2]) << 8) | pkt.payload[3];
    uint16_t aac_size = au_header >> 3;
    size_t data_offset = 2 + au_headers_bytes;
    if (data_offset + aac_size > pkt.payload.size()) {
        return std::nullopt;
    }
    return std::vector<uint8_t>(
        pkt.payload.begin() + data_offset,
        pkt.payload.begin() + data_offset + aac_size);
}

std::vector<uint8_t> RtcpSenderReport::Serialize() const {
    // 28-byte RTCP Sender Report (PT=200, RC=0, length=6 words)
    std::vector<uint8_t> buf(28);
    buf[0] = 0x80;  // V=2, P=0, RC=0
    buf[1] = 200;   // PT=200 (Sender Report)
    buf[2] = 0x00;
    buf[3] = 0x06;  // Length in 32-bit words minus 1 = 6

    auto write_u32 = [&](size_t off, uint32_t v) {
        buf[off]     = static_cast<uint8_t>((v >> 24) & 0xFF);
        buf[off + 1] = static_cast<uint8_t>((v >> 16) & 0xFF);
        buf[off + 2] = static_cast<uint8_t>((v >> 8) & 0xFF);
        buf[off + 3] = static_cast<uint8_t>(v & 0xFF);
    };

    write_u32(4, ssrc);
    write_u32(8, static_cast<uint32_t>((ntp_timestamp >> 32) & 0xFFFFFFFFu));
    write_u32(12, static_cast<uint32_t>(ntp_timestamp & 0xFFFFFFFFu));
    write_u32(16, rtp_timestamp);
    write_u32(20, sender_packet_count);
    write_u32(24, sender_octet_count);
    return buf;
}

std::optional<RtcpSenderReport> RtcpSenderReport::Parse(std::span<const uint8_t> data) {
    if (data.size() < 28 || ((data[0] >> 6) & 0x03) != 2 || data[1] != 200) {
        return std::nullopt;
    }
    auto read_u32 = [&](size_t off) -> uint32_t {
        return (static_cast<uint32_t>(data[off]) << 24) |
               (static_cast<uint32_t>(data[off + 1]) << 16) |
               (static_cast<uint32_t>(data[off + 2]) << 8) |
               static_cast<uint32_t>(data[off + 3]);
    };

    RtcpSenderReport sr;
    sr.ssrc = read_u32(4);
    sr.ntp_timestamp = (static_cast<uint64_t>(read_u32(8)) << 32) | read_u32(12);
    sr.rtp_timestamp = read_u32(16);
    sr.sender_packet_count = read_u32(20);
    sr.sender_octet_count = read_u32(24);
    return sr;
}

}  // namespace nexus::protocol
