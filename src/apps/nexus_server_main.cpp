#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "nexus/codec/aac_parser.hpp"
#include "nexus/codec/test_stream_generator.hpp"
#include "nexus/core/logger.hpp"
#include "nexus/media/http_flv_server.hpp"
#include "nexus/media/media_hub.hpp"
#include "nexus/media/rtsp_server.hpp"

namespace {
std::atomic<bool> g_running{true};

void SignalHandler(int) {
    g_running.store(false);
}
}  // namespace

int main(int argc, char* argv[]) {
    uint16_t rtsp_port = 8554;
    uint16_t http_port = 8080;
    bool enable_demo_stream = true;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--rtsp-port" && i + 1 < argc) {
            rtsp_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--http-port" && i + 1 < argc) {
            http_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--no-demo") {
            enable_demo_stream = false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0]
                      << " [--rtsp-port 8554] [--http-port 8080] [--no-demo]\n";
            return 0;
        }
    }

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    auto hub = std::make_shared<nexus::media::MediaHub>();
    nexus::media::RtspServer rtsp_server("0.0.0.0", rtsp_port, hub);
    nexus::media::HttpFlvServer http_server("0.0.0.0", http_port, hub);

    if (!rtsp_server.Start()) {
        LOG_ERROR("Failed to bind RTSP server on port ", rtsp_port);
        return 1;
    }
    if (!http_server.Start()) {
        LOG_ERROR("Failed to bind HTTP-FLV server on port ", http_port);
        return 1;
    }

    LOG_INFO("==============================================================");
    LOG_INFO("  NexusStream C++20 Media Server Started Successfully!");
    LOG_INFO("  RTSP Push/Pull Endpoint : rtsp://127.0.0.1:", rtsp_server.BoundPort(), "/live/<stream>");
    LOG_INFO("  HTTP-FLV Live Endpoint  : http://127.0.0.1:", http_server.BoundPort(), "/live/<stream>.flv");
    LOG_INFO("  Web Telemetry Dashboard : http://127.0.0.1:", http_server.BoundPort(), "/");
    LOG_INFO("  JSON Streams API        : http://127.0.0.1:", http_server.BoundPort(), "/api/streams");
    LOG_INFO("==============================================================");

    std::thread demo_thread;
    if (enable_demo_stream) {
        demo_thread = std::thread([hub]() {
            auto session = hub->GetOrCreateSession("/live/demo");
            nexus::core::VideoTrackInfo vinfo;
            vinfo.width = 1280;
            vinfo.height = 720;
            vinfo.fps = 30;
            vinfo.sps = nexus::codec::TestStreamGenerator::BuildSpsNalu(1280, 720, 30);
            vinfo.pps = nexus::codec::TestStreamGenerator::BuildPpsNalu();
            session->SetVideoTrack(vinfo);

            nexus::core::AudioTrackInfo ainfo;
            ainfo.sample_rate = 44100;
            ainfo.channels = 2;
            ainfo.profile = 2;
            ainfo.audio_specific_config =
                nexus::codec::AacParser::BuildAudioSpecificConfig(2, 44100, 2);
            session->SetAudioTrack(ainfo);

            LOG_INFO("Built-in live synthetic stream publishing at /live/demo (1280x720@30fps H.264 + AAC)");
            uint32_t frame_idx = 0;
            uint64_t pts_ms = 0;
            while (g_running.load()) {
                bool is_idr = (frame_idx % 30 == 0);
                auto nalu = nexus::codec::TestStreamGenerator::BuildVideoSliceNalu(
                    is_idr, frame_idx, is_idr ? 4500 : 900);
                session->PublishH264Nalu(nalu, pts_ms, pts_ms);

                auto aac = nexus::codec::TestStreamGenerator::BuildRawAacFrame(frame_idx, 160);
                session->PublishRawAac(aac, pts_ms);

                ++frame_idx;
                pts_ms += 33;
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
            }
        });
    }

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("Shutting down NexusStream server...");
    if (demo_thread.joinable()) {
        demo_thread.join();
    }
    http_server.Stop();
    rtsp_server.Stop();
    return 0;
}
