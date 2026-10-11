#include "nexus/codec/test_stream_generator.hpp"
#include "nexus/codec/aac_parser.hpp"
#include "nexus/codec/bit_reader.hpp"
#include "nexus/codec/h264_parser.hpp"

namespace nexus::codec {

std::vector<uint8_t> TestStreamGenerator::BuildSpsNalu(
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    uint8_t profile_idc,
    uint8_t level_idc) {
    BitWriter bw;
    bw.WriteBits(profile_idc, 8);
    bw.WriteBits(0x00, 8);  // constraint_set flags
    bw.WriteBits(level_idc, 8);
    bw.WriteUE(0);          // seq_parameter_set_id = 0

    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244) {
        bw.WriteUE(1);   // chroma_format_idc = 1 (YUV 4:2:0)
        bw.WriteUE(0);   // bit_depth_luma_minus8 = 0
        bw.WriteUE(0);   // bit_depth_chroma_minus8 = 0
        bw.WriteBit(0);  // qpprime_y_zero_transform_bypass_flag = 0
        bw.WriteBit(0);  // seq_scaling_matrix_present_flag = 0
    }

    bw.WriteUE(0);  // log2_max_frame_num_minus4
    bw.WriteUE(0);  // pic_order_cnt_type = 0
    bw.WriteUE(0);  // log2_max_pic_order_cnt_lsb_minus4
    bw.WriteUE(1);  // max_num_ref_frames = 1
    bw.WriteBit(0); // gaps_in_frame_num_value_allowed_flag = 0

    uint32_t mb_width = (width + 15) / 16;
    uint32_t mb_height = (height + 15) / 16;
    bw.WriteUE(mb_width - 1);
    bw.WriteUE(mb_height - 1);
    bw.WriteBit(1);  // frame_mbs_only_flag = 1
    bw.WriteBit(1);  // direct_8x8_inference_flag = 1

    uint32_t pad_w = mb_width * 16 - width;
    uint32_t pad_h = mb_height * 16 - height;
    if (pad_w > 0 || pad_h > 0) {
        bw.WriteBit(1);  // frame_cropping_flag = 1
        bw.WriteUE(0);
        bw.WriteUE(pad_w / 2);
        bw.WriteUE(0);
        bw.WriteUE(pad_h / 2);
    } else {
        bw.WriteBit(0);  // frame_cropping_flag = 0
    }

    // VUI parameters with timing_info_present_flag = 1
    bw.WriteBit(1);  // vui_parameters_present_flag = 1
    bw.WriteBit(0);  // aspect_ratio_info_present_flag = 0
    bw.WriteBit(0);  // overscan_info_present_flag = 0
    bw.WriteBit(0);  // video_signal_type_present_flag = 0
    bw.WriteBit(0);  // chroma_loc_info_present_flag = 0
    bw.WriteBit(1);  // timing_info_present_flag = 1
    bw.WriteBits(1000, 32);              // num_units_in_tick = 1000
    bw.WriteBits(fps * 2000, 32);        // time_scale = fps * 2 * 1000
    bw.WriteBit(1);                      // fixed_frame_rate_flag = 1
    bw.WriteBit(0);                      // nal_hrd_parameters_present_flag
    bw.WriteBit(0);                      // vcl_hrd_parameters_present_flag
    bw.WriteBit(0);                      // pic_struct_present_flag
    bw.WriteBit(0);                      // bitstream_restriction_flag

    bw.WriteRbspTrailingBits();

    std::vector<uint8_t> ebsp = H264Parser::AddEmulationPreventionBytes(bw.Data());
    std::vector<uint8_t> nalu;
    nalu.reserve(1 + ebsp.size());
    nalu.push_back(0x67);  // forbidden_zero_bit=0, nal_ref_idc=3, type=7 (SPS)
    nalu.insert(nalu.end(), ebsp.begin(), ebsp.end());
    return nalu;
}

std::vector<uint8_t> TestStreamGenerator::BuildPpsNalu() {
    // Valid minimal H.264 PPS NALU (0x68 = ref_idc 3, type 8)
    return {0x68, 0xCE, 0x3C, 0x80};
}

std::vector<uint8_t> TestStreamGenerator::BuildVideoSliceNalu(
    bool is_idr,
    uint32_t frame_num,
    size_t payload_size) {
    if (payload_size == 0) {
        payload_size = is_idr ? 4200 : 800;
    }

    std::vector<uint8_t> nalu(payload_size);
    // NALU header: 0x65 (ref_idc=3, type=5 IDR) or 0x41 (ref_idc=2, type=1 Non-IDR P-slice)
    nalu[0] = is_idr ? 0x65 : 0x41;

    // Encode frame_num and deterministic pattern avoiding accidental 00 00 01 start code
    for (size_t i = 1; i < payload_size; ++i) {
        uint8_t v = static_cast<uint8_t>(((frame_num * 31u + i * 17u) & 0x7F) | 0x20);
        nalu[i] = v;
    }
    return nalu;
}

std::vector<uint8_t> TestStreamGenerator::WrapAnnexB(std::span<const uint8_t> raw_nalu) {
    std::vector<uint8_t> out;
    out.reserve(4 + raw_nalu.size());
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x01);
    out.insert(out.end(), raw_nalu.begin(), raw_nalu.end());
    return out;
}

std::vector<uint8_t> TestStreamGenerator::BuildRawAacFrame(uint32_t frame_index, size_t size) {
    std::vector<uint8_t> raw(size);
    for (size_t i = 0; i < size; ++i) {
        raw[i] = static_cast<uint8_t>(((frame_index * 13u + i * 7u) & 0x7F) | 0x10);
    }
    return raw;
}

std::vector<uint8_t> TestStreamGenerator::BuildAdtsAacFrame(
    uint32_t frame_index,
    uint32_t sample_rate,
    uint8_t channels,
    size_t raw_size) {
    auto raw = BuildRawAacFrame(frame_index, raw_size);
    auto hdr = AacParser::BuildAdtsHeader(2 /* AAC-LC */, sample_rate, channels, raw.size());
    hdr.insert(hdr.end(), raw.begin(), raw.end());
    return hdr;
}

}  // namespace nexus::codec
