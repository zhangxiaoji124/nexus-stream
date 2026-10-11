#include "nexus/protocol/flv_muxer.hpp"
#include "nexus/codec/h264_parser.hpp"
#include "nexus/core/ring_buffer.hpp"

namespace nexus::protocol {

std::vector<uint8_t> FlvMuxer::BuildFlvHeader(bool has_video, bool has_audio) {
    uint8_t flags = (has_audio ? 0x04 : 0x00) | (has_video ? 0x01 : 0x00);
    return {
        'F', 'L', 'V',
        0x01,                   // Version 1
        flags,                  // Audio/Video flags
        0x00, 0x00, 0x00, 0x09, // Header size = 9
        0x00, 0x00, 0x00, 0x00  // PreviousTagSize0 = 0
    };
}

std::vector<uint8_t> FlvMuxer::WrapFlvTag(
    FlvTagType type,
    uint32_t timestamp_ms,
    std::span<const uint8_t> body) {
    core::ByteBuffer buf(11 + body.size() + 4);
    buf.WriteU8(static_cast<uint8_t>(type));
    buf.WriteU24BE(static_cast<uint32_t>(body.size()));
    buf.WriteU24BE(timestamp_ms & 0x00FFFFFFu);
    buf.WriteU8(static_cast<uint8_t>((timestamp_ms >> 24) & 0xFF));
    buf.WriteU24BE(0);  // StreamID = 0
    buf.Append(body);
    buf.WriteU32BE(static_cast<uint32_t>(11 + body.size()));  // PreviousTagSizeN
    return buf.ToVector();
}

std::vector<uint8_t> FlvMuxer::BuildVideoSequenceHeaderTag(
    std::span<const uint8_t> sps,
    std::span<const uint8_t> pps,
    uint32_t timestamp_ms) {
    auto avcc = codec::H264Parser::BuildAvcDecoderConfigurationRecord(sps, pps);
    std::vector<uint8_t> body;
    body.reserve(5 + avcc.size());
    body.push_back(0x17);  // Keyframe (1) << 4 | AVC (7)
    body.push_back(0x00);  // AVCPacketType = 0 (AVC sequence header)
    body.push_back(0x00);  // CompositionTime = 0
    body.push_back(0x00);
    body.push_back(0x00);
    body.insert(body.end(), avcc.begin(), avcc.end());
    return WrapFlvTag(FlvTagType::kVideo, timestamp_ms, body);
}

std::vector<uint8_t> FlvMuxer::BuildAudioSequenceHeaderTag(
    std::span<const uint8_t> audio_specific_config,
    uint32_t timestamp_ms) {
    std::vector<uint8_t> body;
    body.reserve(2 + audio_specific_config.size());
    body.push_back(0xAF);  // SoundFormat=10(AAC), SoundRate=3(44kHz), SoundSize=1(16-bit), SoundType=1(Stereo)
    body.push_back(0x00);  // AACPacketType = 0 (AAC sequence header)
    body.insert(body.end(), audio_specific_config.begin(), audio_specific_config.end());
    return WrapFlvTag(FlvTagType::kAudio, timestamp_ms, body);
}

std::vector<uint8_t> FlvMuxer::BuildVideoNaluTag(
    std::span<const uint8_t> raw_nalu,
    bool is_keyframe,
    uint32_t dts_ms,
    uint32_t pts_ms) {
    int32_t cts = static_cast<int32_t>(pts_ms) - static_cast<int32_t>(dts_ms);
    if (cts < 0) cts = 0;

    uint32_t nalu_len = static_cast<uint32_t>(raw_nalu.size());
    std::vector<uint8_t> body;
    body.reserve(5 + 4 + raw_nalu.size());
    body.push_back(is_keyframe ? 0x17 : 0x27);  // 1=Keyframe, 2=Interframe | 7=AVC
    body.push_back(0x01);                       // AVCPacketType = 1 (AVC NALU)
    body.push_back(static_cast<uint8_t>((cts >> 16) & 0xFF));
    body.push_back(static_cast<uint8_t>((cts >> 8) & 0xFF));
    body.push_back(static_cast<uint8_t>(cts & 0xFF));

    // 4-byte Big-Endian NALU length (AVCC format)
    body.push_back(static_cast<uint8_t>((nalu_len >> 24) & 0xFF));
    body.push_back(static_cast<uint8_t>((nalu_len >> 16) & 0xFF));
    body.push_back(static_cast<uint8_t>((nalu_len >> 8) & 0xFF));
    body.push_back(static_cast<uint8_t>(nalu_len & 0xFF));
    body.insert(body.end(), raw_nalu.begin(), raw_nalu.end());

    return WrapFlvTag(FlvTagType::kVideo, dts_ms, body);
}

std::vector<uint8_t> FlvMuxer::BuildAudioRawTag(
    std::span<const uint8_t> raw_aac,
    uint32_t timestamp_ms) {
    std::vector<uint8_t> body;
    body.reserve(2 + raw_aac.size());
    body.push_back(0xAF);  // AAC
    body.push_back(0x01);  // AACPacketType = 1 (AAC raw frame)
    body.insert(body.end(), raw_aac.begin(), raw_aac.end());
    return WrapFlvTag(FlvTagType::kAudio, timestamp_ms, body);
}

std::vector<uint8_t> FlvMuxer::MuxFrame(const core::MediaFrame& frame) {
    if (frame.IsVideo()) {
        return BuildVideoNaluTag(
            frame.Data(),
            frame.IsKeyFrame(),
            static_cast<uint32_t>(frame.dts_ms),
            static_cast<uint32_t>(frame.pts_ms));
    }
    if (frame.IsAudio()) {
        return BuildAudioRawTag(
            frame.Data(),
            static_cast<uint32_t>(frame.dts_ms));
    }
    return {};
}

std::vector<FlvTagView> FlvMuxer::ParseFlvStream(std::span<const uint8_t> flv_bytes) {
    std::vector<FlvTagView> tags;
    size_t offset = 0;

    // Skip 9-byte FLV header + 4-byte PreviousTagSize0 if present
    if (flv_bytes.size() >= 13 &&
        flv_bytes[0] == 'F' && flv_bytes[1] == 'L' && flv_bytes[2] == 'V') {
        offset = 13;
    }

    while (offset + 11 <= flv_bytes.size()) {
        uint8_t tag_type = flv_bytes[offset];
        uint32_t data_size = (static_cast<uint32_t>(flv_bytes[offset + 1]) << 16) |
                             (static_cast<uint32_t>(flv_bytes[offset + 2]) << 8) |
                             static_cast<uint32_t>(flv_bytes[offset + 3]);
        uint32_t ts = (static_cast<uint32_t>(flv_bytes[offset + 4]) << 16) |
                      (static_cast<uint32_t>(flv_bytes[offset + 5]) << 8) |
                      static_cast<uint32_t>(flv_bytes[offset + 6]) |
                      (static_cast<uint32_t>(flv_bytes[offset + 7]) << 24);

        size_t body_start = offset + 11;
        if (body_start + data_size > flv_bytes.size()) {
            break;
        }

        FlvTagView view;
        view.type = static_cast<FlvTagType>(tag_type);
        view.timestamp_ms = ts;
        view.tag_body = flv_bytes.subspan(body_start, data_size);

        if (view.type == FlvTagType::kVideo && data_size >= 2) {
            uint8_t frame_type = (view.tag_body[0] >> 4) & 0x0F;
            uint8_t avc_pkt_type = view.tag_body[1];
            view.is_keyframe = (frame_type == 1);
            view.is_sequence_header = (avc_pkt_type == 0);
        } else if (view.type == FlvTagType::kAudio && data_size >= 2) {
            uint8_t aac_pkt_type = view.tag_body[1];
            view.is_sequence_header = (aac_pkt_type == 0);
        }

        tags.push_back(view);
        offset = body_start + data_size + 4;  // Skip 4-byte PreviousTagSize
    }

    return tags;
}

}  // namespace nexus::protocol
