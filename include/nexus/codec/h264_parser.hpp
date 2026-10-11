#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nexus::codec {

enum class H264NaluType : uint8_t {
    kUnspecified = 0,
    kNonIdrSlice = 1,
    kSliceDataA = 2,
    kSliceDataB = 3,
    kSliceDataC = 4,
    kIdrSlice = 5,
    kSei = 6,
    kSps = 7,
    kPps = 8,
    kAud = 9,
    kStapA = 24,  // RFC 6184 STAP-A
    kFuA = 28     // RFC 6184 FU-A
};

struct H264SpsInfo {
    uint8_t profile_idc = 0;
    uint8_t constraint_set_flags = 0;
    uint8_t level_idc = 0;
    uint32_t sps_id = 0;
    uint32_t chroma_format_idc = 1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;  // 0 if timing_info_present_flag == 0
};

struct H264NaluView {
    H264NaluType type = H264NaluType::kUnspecified;
    uint8_t nal_ref_idc = 0;
    std::span<const uint8_t> data;  // Raw NALU bytes WITHOUT Annex-B start code
};

class H264Parser {
public:
    static constexpr H264NaluType GetNaluType(uint8_t header_byte) noexcept {
        return static_cast<H264NaluType>(header_byte & 0x1F);
    }

    static constexpr uint8_t GetNalRefIdc(uint8_t header_byte) noexcept {
        return static_cast<uint8_t>((header_byte >> 5) & 0x03);
    }

    static constexpr bool IsKeyFrameNalu(H264NaluType type) noexcept {
        return type == H264NaluType::kIdrSlice;
    }

    /**
     * @brief Splits an Annex-B byte stream (00 00 01 or 00 00 00 01 delimited) into individual NALUs.
     */
    static std::vector<H264NaluView> SplitAnnexBNalus(std::span<const uint8_t> annexb_data);

    /**
     * @brief Strips Emulation Prevention Bytes (0x00 0x00 0x03 -> 0x00 0x00) to convert EBSP to RBSP.
     */
    static std::vector<uint8_t> RemoveEmulationPreventionBytes(std::span<const uint8_t> ebsp);

    /**
     * @brief Inserts Emulation Prevention Bytes (0x00 0x00 [00..03] -> 0x00 0x00 0x03 [00..03]) for RBSP to EBSP.
     */
    static std::vector<uint8_t> AddEmulationPreventionBytes(std::span<const uint8_t> rbsp);

    /**
     * @brief Parses an H.264 SPS NALU (with or without NALU header byte, without start code)
     *        using Exp-Golomb decoding per ITU-T H.264 Sec 7.3.2.1.1.
     */
    static std::optional<H264SpsInfo> ParseSps(std::span<const uint8_t> sps_nalu);

    /**
     * @brief Builds an ISO/IEC 14496-15 AVCDecoderConfigurationRecord from raw SPS and PPS NALUs.
     *        Used in FLV sequence headers and MP4 avcC boxes.
     */
    static std::vector<uint8_t> BuildAvcDecoderConfigurationRecord(
        std::span<const uint8_t> sps, std::span<const uint8_t> pps);

    /**
     * @brief Parses an AVCDecoderConfigurationRecord and extracts SPS and PPS NALUs.
     */
    static bool ParseAvcDecoderConfigurationRecord(
        std::span<const uint8_t> avcc,
        std::vector<uint8_t>& out_sps,
        std::vector<uint8_t>& out_pps);

    /**
     * @brief Converts a single NALU or Annex-B stream to 4-byte big-endian length-prefixed AVCC format.
     */
    static std::vector<uint8_t> AnnexBToAvcc(std::span<const uint8_t> annexb_data);

    /**
     * @brief Converts 4-byte length-prefixed AVCC payload into Annex-B (00 00 00 01) format.
     */
    static std::vector<uint8_t> AvccToAnnexB(std::span<const uint8_t> avcc_data);
};

}  // namespace nexus::codec
