#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <vector>

#include "nexus/codec/aac_parser.hpp"
#include "nexus/codec/bit_reader.hpp"
#include "nexus/codec/h264_parser.hpp"
#include "nexus/codec/test_stream_generator.hpp"
#include "nexus/core/bounded_queue.hpp"
#include "nexus/core/ring_buffer.hpp"
#include "nexus/media/gop_cache.hpp"
#include "nexus/media/http_flv_server.hpp"
#include "nexus/media/media_hub.hpp"
#include "nexus/media/rtsp_server.hpp"
#include "nexus/net/socket_utils.hpp"
#include "nexus/protocol/flv_muxer.hpp"
#include "nexus/protocol/rtp_packet.hpp"
#include "nexus/protocol/rtsp_message.hpp"
#include "nexus/protocol/sdp.hpp"

#define NEXUS_ASSERT(cond)                                                                 \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::cerr << "[FAIL] Assertion failed at " << __FILE__ << ":" << __LINE__      \
                      << " -> " << #cond << "\n";                                          \
            std::exit(1);                                                                  \
        }                                                                                  \
    } while (0)

namespace {

void TestBitReaderAndExpGolomb() {
    nexus::codec::BitWriter bw;
    bw.WriteBits(0xAB, 8);
    bw.WriteUE(0);
    bw.WriteUE(1);
    bw.WriteUE(7);
    bw.WriteUE(119);
    bw.WriteSE(-5);
    bw.WriteSE(12);
    bw.WriteRbspTrailingBits();

    nexus::codec::BitReader br(bw.Data());
    NEXUS_ASSERT(br.ReadBits(8) == 0xAB);
    NEXUS_ASSERT(br.ReadUE() == 0);
    NEXUS_ASSERT(br.ReadUE() == 1);
    NEXUS_ASSERT(br.ReadUE() == 7);
    NEXUS_ASSERT(br.ReadUE() == 119);
    NEXUS_ASSERT(br.ReadSE() == -5);
    NEXUS_ASSERT(br.ReadSE() == 12);
    std::cout << "[PASS] TestBitReaderAndExpGolomb\n";
}

void TestH264ParserAndSpsDecoding() {
    // 1. Test SPS generation and Exp-Golomb parsing for 720p, 1080p (cropped from 1088), and 4K
    {
        auto sps_720p = nexus::codec::TestStreamGenerator::BuildSpsNalu(1280, 720, 30);
        auto info_720p = nexus::codec::H264Parser::ParseSps(sps_720p);
        NEXUS_ASSERT(info_720p.has_value());
        NEXUS_ASSERT(info_720p->width == 1280);
        NEXUS_ASSERT(info_720p->height == 720);
        NEXUS_ASSERT(info_720p->fps == 30);

        auto sps_1080p = nexus::codec::TestStreamGenerator::BuildSpsNalu(1920, 1080, 60);
        auto info_1080p = nexus::codec::H264Parser::ParseSps(sps_1080p);
        NEXUS_ASSERT(info_1080p.has_value());
        NEXUS_ASSERT(info_1080p->width == 1920);
        NEXUS_ASSERT(info_1080p->height == 1080);
        NEXUS_ASSERT(info_1080p->fps == 60);

        auto sps_4k = nexus::codec::TestStreamGenerator::BuildSpsNalu(3840, 2160, 25);
        auto info_4k = nexus::codec::H264Parser::ParseSps(sps_4k);
        NEXUS_ASSERT(info_4k.has_value());
        NEXUS_ASSERT(info_4k->width == 3840);
        NEXUS_ASSERT(info_4k->height == 2160);
        NEXUS_ASSERT(info_4k->fps == 25);
    }

    // 2. Test Annex-B stream splitting and AVCC round-trip
    auto sps = nexus::codec::TestStreamGenerator::BuildSpsNalu(1280, 720, 30);
    auto pps = nexus::codec::TestStreamGenerator::BuildPpsNalu();
    auto idr = nexus::codec::TestStreamGenerator::BuildVideoSliceNalu(true, 0, 500);

    std::vector<uint8_t> annexb;
    for (const auto& nalu : {sps, pps, idr}) {
        auto wrapped = nexus::codec::TestStreamGenerator::WrapAnnexB(nalu);
        annexb.insert(annexb.end(), wrapped.begin(), wrapped.end());
    }

    auto nalus = nexus::codec::H264Parser::SplitAnnexBNalus(annexb);
    NEXUS_ASSERT(nalus.size() == 3);
    NEXUS_ASSERT(nalus[0].type == nexus::codec::H264NaluType::kSps);
    NEXUS_ASSERT(nalus[1].type == nexus::codec::H264NaluType::kPps);
    NEXUS_ASSERT(nalus[2].type == nexus::codec::H264NaluType::kIdrSlice);

    auto avcc_stream = nexus::codec::H264Parser::AnnexBToAvcc(annexb);
    auto annexb_restored = nexus::codec::H264Parser::AvccToAnnexB(avcc_stream);
    NEXUS_ASSERT(annexb_restored == annexb);

    // 3. Test AVCDecoderConfigurationRecord round-trip
    auto avcc_cfg = nexus::codec::H264Parser::BuildAvcDecoderConfigurationRecord(sps, pps);
    std::vector<uint8_t> out_sps, out_pps;
    NEXUS_ASSERT(nexus::codec::H264Parser::ParseAvcDecoderConfigurationRecord(avcc_cfg, out_sps, out_pps));
    NEXUS_ASSERT(out_sps == sps);
    NEXUS_ASSERT(out_pps == pps);

    std::cout << "[PASS] TestH264ParserAndSpsDecoding\n";
}

void TestAacAdtsAndAscCodec() {
    auto adts_frame = nexus::codec::TestStreamGenerator::BuildAdtsAacFrame(1, 48000, 2, 200);
    NEXUS_ASSERT(adts_frame.size() == 207);

    auto hdr = nexus::codec::AacParser::ParseAdtsHeader(adts_frame);
    NEXUS_ASSERT(hdr.has_value());
    NEXUS_ASSERT(hdr->sample_rate == 48000);
    NEXUS_ASSERT(hdr->channel_config == 2);
    NEXUS_ASSERT(hdr->frame_length == 207);

    auto asc = nexus::codec::AacParser::BuildAudioSpecificConfig(2, 48000, 2);
    NEXUS_ASSERT(asc.size() == 2);
    auto asc_parsed = nexus::codec::AacParser::ParseAudioSpecificConfig(asc);
    NEXUS_ASSERT(asc_parsed.has_value());
    NEXUS_ASSERT(asc_parsed->profile == 2);
    NEXUS_ASSERT(asc_parsed->sample_rate == 48000);
    NEXUS_ASSERT(asc_parsed->channel_config == 2);

    std::cout << "[PASS] TestAacAdtsAndAscCodec\n";
}

void TestRtpH264FuAAndAacPacketization() {
    // 1. Test large 8500-byte IDR NALU fragmented across 7 FU-A RTP packets and reassembled
    auto big_idr = nexus::codec::TestStreamGenerator::BuildVideoSliceNalu(true, 42, 8500);
    nexus::protocol::H264RtpPacketizer packetizer(0x11223344, 96, 1400);
    nexus::protocol::H264RtpDepacketizer depacketizer;

    auto pkts = packetizer.PacketizeNalu(big_idr, 90000, true);
    NEXUS_ASSERT(pkts.size() == 7);
    for (size_t i = 0; i < pkts.size(); ++i) {
        NEXUS_ASSERT(pkts[i].header.marker == (i + 1 == pkts.size()));
        auto raw_wire = pkts[i].Serialize();
        auto parsed_pkt = nexus::protocol::RtpPacket::Parse(raw_wire);
        NEXUS_ASSERT(parsed_pkt.has_value());
        auto out = depacketizer.ProcessPacket(*parsed_pkt);
        if (i + 1 < pkts.size()) {
            NEXUS_ASSERT(out.empty());
        } else {
            NEXUS_ASSERT(out.size() == 1);
            NEXUS_ASSERT(out[0].rtp_timestamp == 90000);
            NEXUS_ASSERT(out[0].nalu == big_idr);
        }
    }

    // 2. Test AAC RFC 3640 RTP packetizer/depacketizer
    auto raw_aac = nexus::codec::TestStreamGenerator::BuildRawAacFrame(7, 256);
    nexus::protocol::AacRtpCodec aac_rtp(0x55667788, 97);
    auto aac_pkt = aac_rtp.PacketizeRawAac(raw_aac, 44100);
    auto unpacked_aac = nexus::protocol::AacRtpCodec::DepacketizeRawAac(aac_pkt);
    NEXUS_ASSERT(unpacked_aac.has_value());
    NEXUS_ASSERT(*unpacked_aac == raw_aac);

    // 3. Test RTCP Sender Report
    nexus::protocol::RtcpSenderReport sr;
    sr.ssrc = 0x11223344;
    sr.ntp_timestamp = 0xE123456789ABCDEFULL;
    sr.rtp_timestamp = 180000;
    sr.sender_packet_count = 120;
    sr.sender_octet_count = 148000;
    auto sr_bytes = sr.Serialize();
    auto sr_parsed = nexus::protocol::RtcpSenderReport::Parse(sr_bytes);
    NEXUS_ASSERT(sr_parsed.has_value());
    NEXUS_ASSERT(sr_parsed->ssrc == sr.ssrc);
    NEXUS_ASSERT(sr_parsed->ntp_timestamp == sr.ntp_timestamp);
    NEXUS_ASSERT(sr_parsed->rtp_timestamp == sr.rtp_timestamp);

    std::cout << "[PASS] TestRtpH264FuAAndAacPacketization\n";
}

void TestSdpGenerationAndParsing() {
    nexus::core::VideoTrackInfo vinfo;
    vinfo.width = 1920;
    vinfo.height = 1080;
    vinfo.fps = 30;
    vinfo.sps = nexus::codec::TestStreamGenerator::BuildSpsNalu(1920, 1080, 30);
    vinfo.pps = nexus::codec::TestStreamGenerator::BuildPpsNalu();

    nexus::core::AudioTrackInfo ainfo;
    ainfo.sample_rate = 44100;
    ainfo.channels = 2;
    ainfo.profile = 2;
    ainfo.audio_specific_config = nexus::codec::AacParser::BuildAudioSpecificConfig(2, 44100, 2);

    std::string sdp = nexus::protocol::SdpCodec::Generate(vinfo, ainfo, "127.0.0.1", "UnitTestStream");
    auto parsed = nexus::protocol::SdpCodec::Parse(sdp);
    NEXUS_ASSERT(parsed.has_video);
    NEXUS_ASSERT(parsed.has_audio);
    NEXUS_ASSERT(parsed.video_track.width == 1920);
    NEXUS_ASSERT(parsed.video_track.height == 1080);
    NEXUS_ASSERT(parsed.video_track.fps == 30);
    NEXUS_ASSERT(parsed.video_track.sps == vinfo.sps);
    NEXUS_ASSERT(parsed.video_track.pps == vinfo.pps);
    NEXUS_ASSERT(parsed.audio_track.sample_rate == 44100);
    NEXUS_ASSERT(parsed.audio_track.channels == 2);

    std::cout << "[PASS] TestSdpGenerationAndParsing\n";
}

void TestFlvMuxerAndDemuxer() {
    auto sps = nexus::codec::TestStreamGenerator::BuildSpsNalu(1280, 720, 30);
    auto pps = nexus::codec::TestStreamGenerator::BuildPpsNalu();
    auto asc = nexus::codec::AacParser::BuildAudioSpecificConfig(2, 44100, 2);
    auto idr = nexus::codec::TestStreamGenerator::BuildVideoSliceNalu(true, 0, 2048);
    auto aac = nexus::codec::TestStreamGenerator::BuildRawAacFrame(0, 160);

    std::vector<uint8_t> stream;
    auto append = [&](const std::vector<uint8_t>& v) {
        stream.insert(stream.end(), v.begin(), v.end());
    };
    append(nexus::protocol::FlvMuxer::BuildFlvHeader(true, true));
    append(nexus::protocol::FlvMuxer::BuildVideoSequenceHeaderTag(sps, pps, 0));
    append(nexus::protocol::FlvMuxer::BuildAudioSequenceHeaderTag(asc, 0));
    append(nexus::protocol::FlvMuxer::BuildVideoNaluTag(idr, true, 33, 33));
    append(nexus::protocol::FlvMuxer::BuildAudioRawTag(aac, 33));

    auto tags = nexus::protocol::FlvMuxer::ParseFlvStream(stream);
    NEXUS_ASSERT(tags.size() == 4);
    NEXUS_ASSERT(tags[0].type == nexus::protocol::FlvTagType::kVideo && tags[0].is_sequence_header);
    NEXUS_ASSERT(tags[1].type == nexus::protocol::FlvTagType::kAudio && tags[1].is_sequence_header);
    NEXUS_ASSERT(tags[2].type == nexus::protocol::FlvTagType::kVideo && tags[2].is_keyframe && !tags[2].is_sequence_header);
    NEXUS_ASSERT(tags[3].type == nexus::protocol::FlvTagType::kAudio && !tags[3].is_sequence_header);

    std::cout << "[PASS] TestFlvMuxerAndDemuxer\n";
}

void TestBoundedQueueAndGopCache() {
    nexus::core::BoundedBlockingQueue<int> q(3);
    NEXUS_ASSERT(q.TryPush(10));
    NEXUS_ASSERT(q.TryPush(20));
    NEXUS_ASSERT(q.TryPush(30));
    NEXUS_ASSERT(!q.TryPush(40, false));
    NEXUS_ASSERT(q.TryPush(40, true /* drop_oldest */));
    NEXUS_ASSERT(q.DroppedCount() == 1);
    NEXUS_ASSERT(*q.Pop() == 20);
    NEXUS_ASSERT(*q.Pop() == 30);
    NEXUS_ASSERT(*q.Pop() == 40);

    nexus::media::GopCache gop_cache(50);
    uint8_t dummy[16] = {0x65, 1, 2, 3};
    auto idr1 = nexus::core::MediaFrame::Create(
        nexus::core::CodecType::kH264, nexus::core::FrameType::kKeyFrame, 0, 0, dummy);
    auto p1 = nexus::core::MediaFrame::Create(
        nexus::core::CodecType::kH264, nexus::core::FrameType::kInterFrame, 33, 33, dummy);
    auto idr2 = nexus::core::MediaFrame::Create(
        nexus::core::CodecType::kH264, nexus::core::FrameType::kKeyFrame, 66, 66, dummy);

    gop_cache.PushFrame(idr1);
    gop_cache.PushFrame(p1);
    NEXUS_ASSERT(gop_cache.CachedFrameCount() == 2);
    gop_cache.PushFrame(idr2);
    NEXUS_ASSERT(gop_cache.CachedFrameCount() == 1);
    NEXUS_ASSERT(gop_cache.GopCount() == 2);

    std::cout << "[PASS] TestBoundedQueueAndGopCache\n";
}

void TestEndToEndRtspAndHttpFlvStreaming() {
    auto hub = std::make_shared<nexus::media::MediaHub>();
    nexus::media::RtspServer rtsp_server("127.0.0.1", 0, hub);
    nexus::media::HttpFlvServer http_server("127.0.0.1", 0, hub);

    NEXUS_ASSERT(rtsp_server.Start());
    NEXUS_ASSERT(http_server.Start());

    uint16_t rtsp_port = rtsp_server.BoundPort();
    uint16_t http_port = http_server.BoundPort();
    NEXUS_ASSERT(rtsp_port > 0 && http_port > 0);

    nexus::core::VideoTrackInfo vtrack;
    vtrack.width = 1280;
    vtrack.height = 720;
    vtrack.fps = 30;
    vtrack.sps = nexus::codec::TestStreamGenerator::BuildSpsNalu(1280, 720, 30);
    vtrack.pps = nexus::codec::TestStreamGenerator::BuildPpsNalu();

    nexus::core::AudioTrackInfo atrack;
    atrack.sample_rate = 44100;
    atrack.channels = 2;
    atrack.profile = 2;
    atrack.audio_specific_config = nexus::codec::AacParser::BuildAudioSpecificConfig(2, 44100, 2);

    // 1. RTSP Pusher publishes 10 video + 10 audio frames
    nexus::media::RtspPusherClient pusher;
    NEXUS_ASSERT(pusher.ConnectAndPublish("127.0.0.1", rtsp_port, "/live/e2e", vtrack, atrack));

    for (uint32_t i = 0; i < 10; ++i) {
        bool is_idr = (i == 0);
        auto slice = nexus::codec::TestStreamGenerator::BuildVideoSliceNalu(
            is_idr, i, is_idr ? 4800 : 700);
        NEXUS_ASSERT(pusher.PushH264Nalu(slice, i * 33));

        auto aac = nexus::codec::TestStreamGenerator::BuildRawAacFrame(i, 160);
        NEXUS_ASSERT(pusher.PushRawAac(aac, i * 33));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    // 2. Late-joining RTSP Puller connects and immediately receives cached GOP via Instant-Play!
    nexus::media::RtspPullerClient puller;
    nexus::core::VideoTrackInfo desc_v;
    nexus::core::AudioTrackInfo desc_a;
    NEXUS_ASSERT(puller.ConnectAndPlay("127.0.0.1", rtsp_port, "/live/e2e", &desc_v, &desc_a));
    NEXUS_ASSERT(desc_v.width == 1280 && desc_v.height == 720);
    NEXUS_ASSERT(desc_a.sample_rate == 44100 && desc_a.channels == 2);

    size_t got_keyframes = 0;
    size_t got_video = 0;
    size_t got_audio = 0;
    size_t total = puller.ReceiveFrames(20, 500, [&](const nexus::core::MediaFrame& f) {
        if (f.IsVideo()) {
            ++got_video;
            if (f.IsKeyFrame()) {
                ++got_keyframes;
                NEXUS_ASSERT(f.Size() == 4800);  // Verified FU-A reassembled IDR size!
            }
        } else if (f.IsAudio()) {
            ++got_audio;
        }
    });
    NEXUS_ASSERT(total == 20);
    NEXUS_ASSERT(got_keyframes == 1);
    NEXUS_ASSERT(got_video == 10);
    NEXUS_ASSERT(got_audio == 10);

    // 3. HTTP-FLV client connects to /live/e2e.flv and receives valid FLV stream
    auto flv_sock = nexus::net::SocketUtils::ConnectTcp("127.0.0.1", http_port, 1000);
    NEXUS_ASSERT(flv_sock.Valid());
    std::string req = "GET /live/e2e.flv HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    NEXUS_ASSERT(nexus::net::SocketUtils::SendAll(flv_sock.Get(), req));

    std::vector<uint8_t> raw_http;
    uint8_t buf[8192];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    while (std::chrono::steady_clock::now() < deadline && raw_http.size() < 12000) {
        pollfd pfd{flv_sock.Get(), POLLIN, 0};
        if (::poll(&pfd, 1, 20) <= 0) break;
        ssize_t n = ::recv(flv_sock.Get(), buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw_http.insert(raw_http.end(), buf, buf + n);
    }

    std::string_view sv(reinterpret_cast<const char*>(raw_http.data()), raw_http.size());
    auto hdr_end = sv.find("\r\n\r\n");
    NEXUS_ASSERT(hdr_end != std::string_view::npos);
    NEXUS_ASSERT(sv.substr(0, hdr_end).find("200 OK") != std::string_view::npos);
    NEXUS_ASSERT(sv.substr(0, hdr_end).find("video/x-flv") != std::string_view::npos);

    std::span<const uint8_t> flv_data(raw_http.data() + hdr_end + 4, raw_http.size() - (hdr_end + 4));
    auto flv_tags = nexus::protocol::FlvMuxer::ParseFlvStream(flv_data);
    // 2 sequence headers (AVC + AAC) + 20 GOP frames = 22 FLV tags
    NEXUS_ASSERT(flv_tags.size() == 22);
    NEXUS_ASSERT(flv_tags[0].is_sequence_header);
    NEXUS_ASSERT(flv_tags[1].is_sequence_header);
    NEXUS_ASSERT(flv_tags[2].is_keyframe);

    flv_sock.Reset();
    puller.Close();
    pusher.Close();
    http_server.Stop();
    rtsp_server.Stop();

    std::cout << "[PASS] TestEndToEndRtspAndHttpFlvStreaming\n";
}

}  // namespace

int main() {
    std::cout << "Running NexusStream Unit & Integration Tests...\n";
    TestBitReaderAndExpGolomb();
    TestH264ParserAndSpsDecoding();
    TestAacAdtsAndAscCodec();
    TestRtpH264FuAAndAacPacketization();
    TestSdpGenerationAndParsing();
    TestFlvMuxerAndDemuxer();
    TestBoundedQueueAndGopCache();
    TestEndToEndRtspAndHttpFlvStreaming();
    std::cout << "All 8 test suites passed successfully!\n";
    return 0;
}
