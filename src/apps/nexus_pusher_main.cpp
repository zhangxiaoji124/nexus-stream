#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "nexus/codec/aac_parser.hpp"
#include "nexus/codec/test_stream_generator.hpp"
#include "nexus/core/logger.hpp"
#include "nexus/ffmpeg/ffmpeg_raii.hpp"
#include "nexus/media/rtsp_server.hpp"
#include "nexus/protocol/flv_muxer.hpp"

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = 8554;
    std::string stream_path = "/live/stream1";
    std::string export_flv_path;
    std::string export_h264_path;
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 30;
    uint32_t num_frames = 90;
    bool realtime_pacing = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--path" && i + 1 < argc) {
            stream_path = argv[++i];
        } else if (arg == "--frames" && i + 1 < argc) {
            num_frames = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--export-flv" && i + 1 < argc) {
            export_flv_path = argv[++i];
        } else if (arg == "--export-h264" && i + 1 < argc) {
            export_h264_path = argv[++i];
        } else if (arg == "--realtime") {
            realtime_pacing = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "  --host <ip>            RTSP server IP (default: 127.0.0.1)\n"
                      << "  --port <port>          RTSP server port (default: 8554)\n"
                      << "  --path <stream_path>   Stream path (default: /live/stream1)\n"
                      << "  --frames <count>       Number of frames to push/export (default: 90)\n"
                      << "  --realtime             Pace frames at real-time FPS interval\n"
                      << "  --export-flv <file>    Mux H.264 + AAC stream to an FLV file\n"
                      << "  --export-h264 <file>   Write Annex-B H.264 bitstream to file\n";
            return 0;
        }
    }

    auto sps = nexus::codec::TestStreamGenerator::BuildSpsNalu(width, height, fps);
    auto pps = nexus::codec::TestStreamGenerator::BuildPpsNalu();
    auto asc = nexus::codec::AacParser::BuildAudioSpecificConfig(2, 44100, 2);

    if (!export_h264_path.empty()) {
        std::ofstream ofs(export_h264_path, std::ios::binary);
        auto write_vec = [&](const std::vector<uint8_t>& v) {
            ofs.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size()));
        };
        write_vec(nexus::codec::TestStreamGenerator::WrapAnnexB(sps));
        write_vec(nexus::codec::TestStreamGenerator::WrapAnnexB(pps));
        for (uint32_t i = 0; i < num_frames; ++i) {
            bool is_idr = (i % fps == 0);
            if (is_idr && i > 0) {
                write_vec(nexus::codec::TestStreamGenerator::WrapAnnexB(sps));
                write_vec(nexus::codec::TestStreamGenerator::WrapAnnexB(pps));
            }
            auto slice = nexus::codec::TestStreamGenerator::BuildVideoSliceNalu(is_idr, i);
            write_vec(nexus::codec::TestStreamGenerator::WrapAnnexB(slice));
        }
        LOG_INFO("Exported Annex-B H.264 stream (", num_frames, " frames) to ", export_h264_path);
    }

    if (!export_flv_path.empty()) {
        std::ofstream ofs(export_flv_path, std::ios::binary);
        auto write_vec = [&](const std::vector<uint8_t>& v) {
            ofs.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size()));
        };
        write_vec(nexus::protocol::FlvMuxer::BuildFlvHeader(true, true));
        write_vec(nexus::protocol::FlvMuxer::BuildVideoSequenceHeaderTag(sps, pps, 0));
        write_vec(nexus::protocol::FlvMuxer::BuildAudioSequenceHeaderTag(asc, 0));

        uint32_t ts_ms = 0;
        for (uint32_t i = 0; i < num_frames; ++i) {
            bool is_idr = (i % fps == 0);
            auto slice = nexus::codec::TestStreamGenerator::BuildVideoSliceNalu(is_idr, i);
            write_vec(nexus::protocol::FlvMuxer::BuildVideoNaluTag(slice, is_idr, ts_ms, ts_ms));

            auto aac = nexus::codec::TestStreamGenerator::BuildRawAacFrame(i, 160);
            write_vec(nexus::protocol::FlvMuxer::BuildAudioRawTag(aac, ts_ms));
            ts_ms += 1000 / fps;
        }
        LOG_INFO("Exported FLV container file (", num_frames, " A/V frames) to ", export_flv_path);
    }

    if (!export_flv_path.empty() || !export_h264_path.empty()) {
        bool explicit_push = false;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--host" || a == "--port" || a == "--path") {
                explicit_push = true;
            }
        }
        if (!explicit_push) {
            return 0;
        }
    }

    nexus::core::VideoTrackInfo vtrack;
    vtrack.width = width;
    vtrack.height = height;
    vtrack.fps = fps;
    vtrack.sps = sps;
    vtrack.pps = pps;

    nexus::core::AudioTrackInfo atrack;
    atrack.sample_rate = 44100;
    atrack.channels = 2;
    atrack.profile = 2;
    atrack.audio_specific_config = asc;

    nexus::media::RtspPusherClient pusher;
    LOG_INFO("Connecting RTSP Pusher to rtsp://", host, ":", port, stream_path, " ...");
    if (!pusher.ConnectAndPublish(host, port, stream_path, vtrack, atrack)) {
        LOG_ERROR("Failed to connect and publish to rtsp://", host, ":", port, stream_path);
        return 1;
    }

    uint64_t pts_ms = 0;
    for (uint32_t i = 0; i < num_frames; ++i) {
        bool is_idr = (i % fps == 0);
        auto slice = nexus::codec::TestStreamGenerator::BuildVideoSliceNalu(
            is_idr, i, is_idr ? 5200 : 950);
        if (!pusher.PushH264Nalu(slice, pts_ms)) {
            LOG_ERROR("Failed to push video frame #", i);
            return 1;
        }

        auto aac = nexus::codec::TestStreamGenerator::BuildRawAacFrame(i, 172);
        if (!pusher.PushRawAac(aac, pts_ms)) {
            LOG_ERROR("Failed to push audio frame #", i);
            return 1;
        }

        pts_ms += 1000 / fps;
        if (realtime_pacing) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 / fps));
        }
    }

    LOG_INFO("Successfully pushed ", num_frames, " H.264 + AAC frames via RTSP/RTP (TCP interleaved)!");
    pusher.Close();
    return 0;
}
