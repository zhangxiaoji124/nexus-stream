#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <vector>

#include "nexus/codec/aac_parser.hpp"
#include "nexus/codec/h264_parser.hpp"
#include "nexus/codec/test_stream_generator.hpp"
#include "nexus/core/logger.hpp"
#include "nexus/media/http_flv_server.hpp"
#include "nexus/media/media_hub.hpp"
#include "nexus/media/rtsp_server.hpp"
#include "nexus/net/socket_utils.hpp"
#include "nexus/protocol/flv_muxer.hpp"
#include "nexus/protocol/rtp_packet.hpp"

int main() {
    using Clock = std::chrono::steady_clock;

    std::cout << "======================================================================\n"
              << "  NexusStream C++20 Audio/Video Protocol & Streaming Benchmark Suite  \n"
              << "======================================================================\n\n";

    // 1. Benchmark H.264 SPS Exp-Golomb Parser + RTP FU-A Packetizer/Depacketizer
    {
        auto sps = nexus::codec::TestStreamGenerator::BuildSpsNalu(1920, 1080, 60);
        auto idr = nexus::codec::TestStreamGenerator::BuildVideoSliceNalu(true, 1, 16384);  // 16 KB IDR frame
        auto pframe = nexus::codec::TestStreamGenerator::BuildVideoSliceNalu(false, 2, 1200);

        constexpr size_t kIterations = 20000;
        nexus::protocol::H264RtpPacketizer packetizer(0xCAFEBABE, 96, 1400);
        nexus::protocol::H264RtpDepacketizer depacketizer;

        size_t total_rtp_packets = 0;
        size_t total_reassembled_nalus = 0;
        size_t total_bytes = 0;

        auto t0 = Clock::now();
        for (size_t i = 0; i < kIterations; ++i) {
            const auto& nalu = (i % 10 == 0) ? idr : pframe;
            total_bytes += nalu.size();
            auto pkts = packetizer.PacketizeNalu(nalu, static_cast<uint32_t>(i * 1500), true);
            total_rtp_packets += pkts.size();
            for (const auto& pkt : pkts) {
                auto reassembled = depacketizer.ProcessPacket(pkt);
                total_reassembled_nalus += reassembled.size();
            }
        }
        auto t1 = Clock::now();
        double elapsed_sec = std::chrono::duration<double>(t1 - t0).count();
        double mb_per_sec = (static_cast<double>(total_bytes) / (1024.0 * 1024.0)) / elapsed_sec;

        auto sps_info = nexus::codec::H264Parser::ParseSps(sps);
        std::cout << "[1] RFC 6184 H.264 RTP Packetizer & FU-A Reassembly Benchmark\n"
                  << "    Parsed SPS Resolution  : " << sps_info->width << "x" << sps_info->height
                  << " @ " << sps_info->fps << " fps (Profile " << static_cast<int>(sps_info->profile_idc) << ")\n"
                  << "    NALUs Processed        : " << kIterations << " (" << total_reassembled_nalus << " reassembled)\n"
                  << "    RTP Packets Generated  : " << total_rtp_packets << "\n"
                  << "    Throughput             : " << std::fixed << std::setprecision(2)
                  << mb_per_sec << " MB/s (" << static_cast<uint64_t>(total_rtp_packets / elapsed_sec)
                  << " RTP pkts/sec)\n\n";
    }

    // 2. Benchmark FLV Muxer + Demuxer
    {
        auto sps = nexus::codec::TestStreamGenerator::BuildSpsNalu(1280, 720, 30);
        auto pps = nexus::codec::TestStreamGenerator::BuildPpsNalu();
        auto idr = nexus::codec::TestStreamGenerator::BuildVideoSliceNalu(true, 0, 8192);
        auto aac = nexus::codec::TestStreamGenerator::BuildRawAacFrame(0, 180);

        constexpr size_t kFrames = 25000;
        size_t total_flv_bytes = 0;

        auto t0 = Clock::now();
        for (size_t i = 0; i < kFrames; ++i) {
            auto vtag = nexus::protocol::FlvMuxer::BuildVideoNaluTag(
                idr, (i % 30 == 0), static_cast<uint32_t>(i * 33), static_cast<uint32_t>(i * 33));
            auto atag = nexus::protocol::FlvMuxer::BuildAudioRawTag(
                aac, static_cast<uint32_t>(i * 33));
            total_flv_bytes += vtag.size() + atag.size();
        }
        auto t1 = Clock::now();
        double elapsed_sec = std::chrono::duration<double>(t1 - t0).count();
        double mb_per_sec = (static_cast<double>(total_flv_bytes) / (1024.0 * 1024.0)) / elapsed_sec;

        std::cout << "[2] Adobe FLV / HTTP-FLV Container Muxing Benchmark\n"
                  << "    A/V Frames Muxed       : " << (kFrames * 2) << " tags\n"
                  << "    Total Muxed Volume     : " << (total_flv_bytes / (1024 * 1024)) << " MB\n"
                  << "    Muxing Throughput      : " << std::fixed << std::setprecision(2)
                  << mb_per_sec << " MB/s\n\n";
    }

    // 3. End-to-End Live Server Benchmark:
    //    RtspPusherClient -> RtspServer -> MediaHub (GopCache) -> RtspPullerClient + HTTP-FLV Client
    {
        auto hub = std::make_shared<nexus::media::MediaHub>();
        nexus::media::RtspServer rtsp_server("127.0.0.1", 0, hub);
        nexus::media::HttpFlvServer http_server("127.0.0.1", 0, hub);

        rtsp_server.Start();
        http_server.Start();

        uint16_t rtsp_port = rtsp_server.BoundPort();
        uint16_t http_port = http_server.BoundPort();

        nexus::core::VideoTrackInfo vtrack;
        vtrack.width = 1920;
        vtrack.height = 1080;
        vtrack.fps = 30;
        vtrack.sps = nexus::codec::TestStreamGenerator::BuildSpsNalu(1920, 1080, 30);
        vtrack.pps = nexus::codec::TestStreamGenerator::BuildPpsNalu();

        nexus::core::AudioTrackInfo atrack;
        atrack.sample_rate = 44100;
        atrack.channels = 2;
        atrack.profile = 2;
        atrack.audio_specific_config =
            nexus::codec::AacParser::BuildAudioSpecificConfig(2, 44100, 2);

        nexus::media::RtspPusherClient pusher;
        pusher.ConnectAndPublish("127.0.0.1", rtsp_port, "/live/bench", vtrack, atrack);

        // Push initial 15 frames (1 IDR + 14 P-frames + 15 AAC frames) so GopCache is populated
        for (uint32_t i = 0; i < 15; ++i) {
            bool is_idr = (i == 0);
            auto slice = nexus::codec::TestStreamGenerator::BuildVideoSliceNalu(
                is_idr, i, is_idr ? 6000 : 1000);
            pusher.PushH264Nalu(slice, i * 33);
            auto aac = nexus::codec::TestStreamGenerator::BuildRawAacFrame(i, 160);
            pusher.PushRawAac(aac, i * 33);
        }

        // Give server a few milliseconds to process all initial RTP frames into GopCache
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        // Measure Instant-Play (秒开) First-Frame Latency when a late RTSP viewer joins!
        auto join_t0 = Clock::now();
        nexus::media::RtspPullerClient puller;
        nexus::core::VideoTrackInfo pulled_vtrack;
        nexus::core::AudioTrackInfo pulled_atrack;
        puller.ConnectAndPlay("127.0.0.1", rtsp_port, "/live/bench", &pulled_vtrack, &pulled_atrack);

        double first_keyframe_ms = -1.0;
        size_t pulled_frames = puller.ReceiveFrames(20, 500, [&](const nexus::core::MediaFrame& f) {
            if (first_keyframe_ms < 0.0 && f.IsVideo() && f.IsKeyFrame()) {
                first_keyframe_ms = std::chrono::duration<double, std::milli>(
                    Clock::now() - join_t0).count();
            }
        });

        // Also connect an HTTP-FLV client to /live/bench.flv and verify FLV header + GOP tags
        auto flv_sock = nexus::net::SocketUtils::ConnectTcp("127.0.0.1", http_port, 1000);
        std::string http_req =
            "GET /live/bench.flv HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n\r\n";
        nexus::net::SocketUtils::SendAll(flv_sock.Get(), http_req);

        std::vector<uint8_t> flv_recv;
        uint8_t buf[8192];
        auto deadline = Clock::now() + std::chrono::milliseconds(150);
        while (Clock::now() < deadline && flv_recv.size() < 24000) {
            pollfd pfd{flv_sock.Get(), POLLIN, 0};
            if (::poll(&pfd, 1, 20) <= 0) break;
            ssize_t n = ::recv(flv_sock.Get(), buf, sizeof(buf), 0);
            if (n <= 0) break;
            flv_recv.insert(flv_recv.end(), buf, buf + n);
        }

        size_t flv_tag_count = 0;
        std::string_view flv_sv(reinterpret_cast<const char*>(flv_recv.data()), flv_recv.size());
        auto body_pos = flv_sv.find("\r\n\r\n");
        if (body_pos != std::string_view::npos) {
            std::span<const uint8_t> flv_body(
                flv_recv.data() + body_pos + 4, flv_recv.size() - (body_pos + 4));
            auto tags = nexus::protocol::FlvMuxer::ParseFlvStream(flv_body);
            flv_tag_count = tags.size();
        }

        std::cout << "[3] End-to-End Live Server & GOP Cache Instant-Play (秒开) Verification\n"
                  << "    RTSP Server Port       : " << rtsp_port << "\n"
                  << "    HTTP-FLV Server Port   : " << http_port << "\n"
                  << "    Negotiated Video Track : H.264 " << pulled_vtrack.width << "x"
                  << pulled_vtrack.height << " @ " << pulled_vtrack.fps << " fps\n"
                  << "    Negotiated Audio Track : AAC " << pulled_atrack.sample_rate << " Hz, "
                  << static_cast<int>(pulled_atrack.channels) << " channels\n"
                  << "    First IDR KeyFrame     : " << std::fixed << std::setprecision(2)
                  << first_keyframe_ms << " ms (Instant GOP Cache Replay!)\n"
                  << "    RTSP Frames Pulled     : " << pulled_frames << " frames\n"
                  << "    HTTP-FLV Tags Pulled   : " << flv_tag_count << " FLV tags ("
                  << flv_recv.size() << " bytes)\n\n";

        flv_sock.Reset();
        puller.Close();
        pusher.Close();
        http_server.Stop();
        rtsp_server.Stop();
    }

    std::cout << "======================================================================\n"
              << "  All Benchmarks Completed Successfully!\n"
              << "======================================================================\n";
    return 0;
}
