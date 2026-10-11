#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "nexus/media/media_hub.hpp"
#include "nexus/net/socket_utils.hpp"
#include "nexus/net/tcp_server.hpp"
#include "nexus/protocol/rtp_packet.hpp"
#include "nexus/protocol/rtsp_message.hpp"

namespace nexus::media {

/**
 * @brief RTSP/1.0 Live Streaming Server supporting both:
 *        - RTSP Push (ANNOUNCE -> SETUP -> RECORD -> RTP over TCP Interleaved)
 *        - RTSP Pull (OPTIONS -> DESCRIBE -> SETUP -> PLAY -> RTP over TCP Interleaved)
 */
class RtspServer {
public:
    RtspServer(std::string bind_ip, uint16_t port, std::shared_ptr<MediaHub> hub);
    ~RtspServer();

    bool Start();
    void Stop();

    uint16_t BoundPort() const noexcept { return tcp_server_.BoundPort(); }

private:
    struct ClientSessionContext;

    void OnConnect(const net::TcpConnectionPtr& conn);
    void OnMessage(const net::TcpConnectionPtr& conn, core::ByteBuffer& buffer);
    void OnClose(const net::TcpConnectionPtr& conn);

    void HandleRtspRequest(const net::TcpConnectionPtr& conn,
                           std::shared_ptr<ClientSessionContext> ctx,
                           const protocol::RtspRequest& req);

    void HandleInterleavedRtp(std::shared_ptr<ClientSessionContext> ctx,
                              uint8_t channel,
                              std::span<const uint8_t> rtp_bytes);

    std::shared_ptr<MediaHub> hub_;
    net::TcpServer tcp_server_;
};

/**
 * @brief RTSP Push Client (ANNOUNCE -> SETUP -> RECORD) over TCP interleaved transport.
 */
class RtspPusherClient {
public:
    RtspPusherClient() = default;
    ~RtspPusherClient() { Close(); }

    bool ConnectAndPublish(std::string_view host,
                           uint16_t port,
                           std::string_view stream_path,
                           const core::VideoTrackInfo& video_track,
                           const std::optional<core::AudioTrackInfo>& audio_track = std::nullopt);

    bool PushH264Nalu(std::span<const uint8_t> raw_nalu, uint64_t pts_ms);
    bool PushRawAac(std::span<const uint8_t> raw_aac, uint64_t pts_ms);
    void Close();

private:
    std::optional<protocol::RtspResponse> SendRequestAndWait(protocol::RtspRequest& req);

    net::ScopedFd sock_;
    std::string base_url_;
    std::string session_id_;
    uint32_t cseq_ = 1;
    uint32_t audio_sample_rate_ = 44100;
    uint8_t video_rtp_channel_ = 0;
    uint8_t audio_rtp_channel_ = 2;

    protocol::H264RtpPacketizer video_packetizer_{0x11223344};
    protocol::AacRtpCodec audio_packetizer_{0x55667788};
};

/**
 * @brief RTSP Pull Client (OPTIONS -> DESCRIBE -> SETUP -> PLAY) over TCP interleaved transport.
 */
class RtspPullerClient {
public:
    using FrameHandler = std::function<void(const core::MediaFrame&)>;

    RtspPullerClient() = default;
    ~RtspPullerClient() { Close(); }

    bool ConnectAndPlay(std::string_view host,
                        uint16_t port,
                        std::string_view stream_path,
                        core::VideoTrackInfo* out_video_track = nullptr,
                        core::AudioTrackInfo* out_audio_track = nullptr);

    /**
     * @brief Reads interleaved RTP packets for up to `max_frames` or `timeout_ms`,
     *        depacketizing H.264 (Single NALU & FU-A) and AAC frames.
     */
    size_t ReceiveFrames(size_t max_frames, int timeout_ms, FrameHandler on_frame);

    void Close();

private:
    std::optional<protocol::RtspResponse> SendRequestAndWait(protocol::RtspRequest& req);

    net::ScopedFd sock_;
    core::ByteBuffer recv_buf_{32768};
    std::string base_url_;
    std::string session_id_;
    uint32_t cseq_ = 1;
    uint32_t audio_sample_rate_ = 44100;
    uint8_t video_rtp_channel_ = 0;
    uint8_t audio_rtp_channel_ = 2;

    protocol::H264RtpDepacketizer video_depacketizer_;
};

}  // namespace nexus::media
