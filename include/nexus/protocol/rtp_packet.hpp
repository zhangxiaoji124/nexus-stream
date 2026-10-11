#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nexus::protocol {

constexpr uint8_t kRtpVersion = 2;
constexpr size_t kRtpHeaderSize = 12;
constexpr size_t kDefaultRtpMtu = 1400;
constexpr uint8_t kPayloadTypeH264 = 96;
constexpr uint8_t kPayloadTypeAAC = 97;

struct RtpHeader {
    uint8_t version = kRtpVersion;
    bool padding = false;
    bool extension = false;
    uint8_t csrc_count = 0;
    bool marker = false;
    uint8_t payload_type = kPayloadTypeH264;
    uint16_t sequence = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
};

struct RtpPacket {
    RtpHeader header;
    std::vector<uint8_t> payload;

    std::vector<uint8_t> Serialize() const;
    std::vector<uint8_t> SerializeInterleaved(uint8_t channel) const;

    static std::optional<RtpPacket> Parse(std::span<const uint8_t> raw_bytes);
};

/**
 * @brief RFC 6184 H.264 RTP Packetizer (Single NALU + FU-A Fragmentation Units).
 */
class H264RtpPacketizer {
public:
    H264RtpPacketizer(uint32_t ssrc = 0x12345678,
                      uint8_t payload_type = kPayloadTypeH264,
                      size_t max_payload_size = kDefaultRtpMtu)
        : ssrc_(ssrc), payload_type_(payload_type), max_payload_size_(max_payload_size) {}

    std::vector<RtpPacket> PacketizeNalu(std::span<const uint8_t> raw_nalu,
                                         uint32_t rtp_timestamp,
                                         bool is_last_nalu_of_access_unit = true);

    uint16_t CurrentSequence() const noexcept { return sequence_; }

private:
    uint32_t ssrc_;
    uint8_t payload_type_;
    size_t max_payload_size_;
    uint16_t sequence_ = 0;
};

/**
 * @brief RFC 6184 H.264 RTP Depacketizer (Single NALU, STAP-A, and FU-A reassembly).
 */
class H264RtpDepacketizer {
public:
    struct ReassembledNalu {
        uint32_t rtp_timestamp = 0;
        std::vector<uint8_t> nalu;  // Raw NALU without Annex-B start code
    };

    std::vector<ReassembledNalu> ProcessPacket(const RtpPacket& pkt);

private:
    std::vector<uint8_t> fua_buffer_;
    uint32_t fua_timestamp_ = 0;
    bool in_fua_ = false;
};

/**
 * @brief RFC 3640 AAC (mpeg4-generic, AAC-hbr) RTP Packetizer & Depacketizer.
 *        Uses 16-bit AU-headers-length (0x0010) + 16-bit AU-header (13-bit size + 3-bit index).
 */
class AacRtpCodec {
public:
    explicit AacRtpCodec(uint32_t ssrc = 0x87654321, uint8_t payload_type = kPayloadTypeAAC)
        : ssrc_(ssrc), payload_type_(payload_type) {}

    RtpPacket PacketizeRawAac(std::span<const uint8_t> raw_aac, uint32_t rtp_timestamp);
    static std::optional<std::vector<uint8_t>> DepacketizeRawAac(const RtpPacket& pkt);

private:
    uint32_t ssrc_;
    uint8_t payload_type_;
    uint16_t sequence_ = 0;
};

/**
 * @brief RFC 3550 RTCP Sender Report (SR) for Audio-Video Lip-Sync (NTP <-> RTP clock mapping).
 */
struct RtcpSenderReport {
    uint32_t ssrc = 0;
    uint64_t ntp_timestamp = 0;
    uint32_t rtp_timestamp = 0;
    uint32_t sender_packet_count = 0;
    uint32_t sender_octet_count = 0;

    std::vector<uint8_t> Serialize() const;
    static std::optional<RtcpSenderReport> Parse(std::span<const uint8_t> data);
};

}  // namespace nexus::protocol
