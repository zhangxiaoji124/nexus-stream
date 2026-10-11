#include "nexus/media/rtsp_server.hpp"
#include "nexus/codec/h264_parser.hpp"
#include "nexus/core/logger.hpp"
#include "nexus/protocol/sdp.hpp"

#include <chrono>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>

namespace nexus::media {

struct RtspServer::ClientSessionContext {
    std::string session_id;
    std::string stream_path;
    MediaSessionPtr media_session;
    MediaSession::SubscriberId subscriber_id = 0;

    bool is_publisher = false;
    uint8_t video_rtp_channel = 0;
    uint8_t video_rtcp_channel = 1;
    uint8_t audio_rtp_channel = 2;
    uint8_t audio_rtcp_channel = 3;

    protocol::H264RtpPacketizer video_packetizer{0x12345678};
    protocol::H264RtpDepacketizer video_depacketizer;
    protocol::AacRtpCodec audio_rtp_codec{0x87654321};
};

RtspServer::RtspServer(std::string bind_ip, uint16_t port, std::shared_ptr<MediaHub> hub)
    : hub_(std::move(hub)), tcp_server_(std::move(bind_ip), port) {
    tcp_server_.SetConnectionCallback([this](const net::TcpConnectionPtr& conn) {
        OnConnect(conn);
    });
    tcp_server_.SetMessageCallback([this](const net::TcpConnectionPtr& conn, core::ByteBuffer& buf) {
        OnMessage(conn, buf);
    });
    tcp_server_.SetCloseCallback([this](const net::TcpConnectionPtr& conn) {
        OnClose(conn);
    });
}

RtspServer::~RtspServer() {
    Stop();
}

bool RtspServer::Start() {
    return tcp_server_.Start();
}

void RtspServer::Stop() {
    tcp_server_.Stop();
}

void RtspServer::OnConnect(const net::TcpConnectionPtr& conn) {
    auto ctx = std::make_shared<ClientSessionContext>();
    static std::atomic<uint64_t> next_sid{100001};
    ctx->session_id = "NEXUS_" + std::to_string(next_sid.fetch_add(1));
    conn->SetContext(ctx);
}

void RtspServer::OnClose(const net::TcpConnectionPtr& conn) {
    auto ctx = conn->GetContext<ClientSessionContext>();
    if (ctx && ctx->media_session && ctx->subscriber_id != 0) {
        ctx->media_session->Unsubscribe(ctx->subscriber_id);
        ctx->subscriber_id = 0;
    }
}

void RtspServer::OnMessage(const net::TcpConnectionPtr& conn, core::ByteBuffer& buffer) {
    auto ctx = conn->GetContext<ClientSessionContext>();
    if (!ctx) return;

    while (buffer.ReadableBytes() > 0) {
        const uint8_t* p = buffer.Peek();

        // Check for RTP/RTCP over TCP Interleaved frame: '$' + 1B channel + 2B length
        if (p[0] == '$') {
            if (buffer.ReadableBytes() < 4) {
                break;  // Wait for 4-byte interleaved header
            }
            uint8_t channel = p[1];
            uint16_t payload_len = (static_cast<uint16_t>(p[2]) << 8) | p[3];
            if (buffer.ReadableBytes() < 4 + static_cast<size_t>(payload_len)) {
                break;  // Wait for full RTP/RTCP packet
            }
            std::span<const uint8_t> rtp_span(p + 4, payload_len);
            HandleInterleavedRtp(ctx, channel, rtp_span);
            buffer.Retrieve(4 + payload_len);
        } else {
            // RTSP text message
            size_t consumed = 0;
            auto req = protocol::RtspRequest::Parse(buffer.ReadableStringView(), &consumed);
            if (!req) {
                break;  // Wait for complete RTSP headers/body
            }
            buffer.Retrieve(consumed);
            HandleRtspRequest(conn, ctx, *req);
        }
    }
}

void RtspServer::HandleInterleavedRtp(std::shared_ptr<ClientSessionContext> ctx,
                                      uint8_t channel,
                                      std::span<const uint8_t> rtp_bytes) {
    if (!ctx->media_session) return;

    if (channel == ctx->video_rtp_channel) {
        auto pkt = protocol::RtpPacket::Parse(rtp_bytes);
        if (!pkt) return;
        auto nalus = ctx->video_depacketizer.ProcessPacket(*pkt);
        uint64_t pts_ms = (static_cast<uint64_t>(pkt->header.timestamp) * 1000) / 90000;
        for (const auto& item : nalus) {
            ctx->media_session->PublishH264Nalu(item.nalu, pts_ms, pts_ms);
        }
    } else if (channel == ctx->audio_rtp_channel) {
        auto pkt = protocol::RtpPacket::Parse(rtp_bytes);
        if (!pkt) return;
        auto raw_aac = protocol::AacRtpCodec::DepacketizeRawAac(*pkt);
        if (!raw_aac) return;
        uint32_t sr = 44100;
        if (auto at = ctx->media_session->GetAudioTrack()) {
            sr = at->sample_rate;
        }
        uint64_t pts_ms = (static_cast<uint64_t>(pkt->header.timestamp) * 1000) / sr;
        ctx->media_session->PublishRawAac(*raw_aac, pts_ms);
    }
}

void RtspServer::HandleRtspRequest(const net::TcpConnectionPtr& conn,
                                   std::shared_ptr<ClientSessionContext> ctx,
                                   const protocol::RtspRequest& req) {
    protocol::RtspResponse resp;
    resp.cseq = req.cseq;
    resp.session = ctx->session_id;

    if (req.method == "OPTIONS") {
        resp.headers["Public"] = "OPTIONS, DESCRIBE, ANNOUNCE, SETUP, PLAY, RECORD, TEARDOWN, GET_PARAMETER";
        conn->Send(resp.Serialize());
        return;
    }

    if (req.method == "ANNOUNCE") {
        ctx->is_publisher = true;
        ctx->stream_path = req.ExtractStreamPath();
        ctx->media_session = hub_->GetOrCreateSession(ctx->stream_path);

        auto sdp_info = protocol::SdpCodec::Parse(req.body);
        if (sdp_info.has_video) {
            ctx->media_session->SetVideoTrack(sdp_info.video_track);
        }
        if (sdp_info.has_audio) {
            ctx->media_session->SetAudioTrack(sdp_info.audio_track);
        }
        conn->Send(resp.Serialize());
        return;
    }

    if (req.method == "DESCRIBE") {
        ctx->is_publisher = false;
        ctx->stream_path = req.ExtractStreamPath();
        ctx->media_session = hub_->FindSession(ctx->stream_path);
        if (!ctx->media_session) {
            resp.status_code = 404;
            resp.reason = "Stream Not Found";
            conn->Send(resp.Serialize());
            return;
        }
        resp.headers["Content-Type"] = "application/sdp";
        resp.body = protocol::SdpCodec::Generate(
            ctx->media_session->GetVideoTrack(),
            ctx->media_session->GetAudioTrack(),
            "127.0.0.1",
            ctx->stream_path);
        conn->Send(resp.Serialize());
        return;
    }

    if (req.method == "SETUP") {
        if (ctx->stream_path.empty()) {
            ctx->stream_path = req.ExtractStreamPath();
        }
        if (!ctx->media_session) {
            ctx->media_session = hub_->GetOrCreateSession(ctx->stream_path);
        }

        int track_id = req.ExtractTrackId();
        auto tr = req.ParseTransport();
        if (track_id == 0) {
            ctx->video_rtp_channel = tr.rtp_channel;
            ctx->video_rtcp_channel = tr.rtcp_channel;
        } else {
            ctx->audio_rtp_channel = tr.rtp_channel;
            ctx->audio_rtcp_channel = tr.rtcp_channel;
        }

        std::ostringstream tr_hdr;
        tr_hdr << "RTP/AVP/TCP;unicast;interleaved="
               << static_cast<int>(tr.rtp_channel) << "-"
               << static_cast<int>(tr.rtcp_channel);
        if (tr.mode == "RECORD" || ctx->is_publisher) {
            tr_hdr << ";mode=record";
        }
        resp.headers["Transport"] = tr_hdr.str();
        conn->Send(resp.Serialize());
        return;
    }

    if (req.method == "RECORD") {
        resp.headers["RTP-Info"] = "url=" + req.url + ";seq=0;rtptime=0";
        conn->Send(resp.Serialize());
        return;
    }

    if (req.method == "PLAY") {
        if (!ctx->media_session) {
            resp.status_code = 404;
            resp.reason = "Stream Not Found";
            conn->Send(resp.Serialize());
            return;
        }
        resp.headers["Range"] = "npt=0.000-";
        conn->Send(resp.Serialize());

        // Subscribe to MediaSession (with instant GOP Cache replay!)
        std::weak_ptr<net::TcpConnection> weak_conn = conn;
        ctx->subscriber_id = ctx->media_session->Subscribe(
            [weak_conn, ctx](const core::MediaFrame& frame) -> bool {
                auto c = weak_conn.lock();
                if (!c || !c->IsConnected()) return false;

                if (frame.IsVideo()) {
                    auto pkts = ctx->video_packetizer.PacketizeNalu(
                        frame.Data(), frame.rtp_timestamp, true);
                    for (const auto& pkt : pkts) {
                        auto raw = pkt.SerializeInterleaved(ctx->video_rtp_channel);
                        if (!c->Send(raw)) return false;
                    }
                } else if (frame.IsAudio()) {
                    auto pkt = ctx->audio_rtp_codec.PacketizeRawAac(
                        frame.Data(), frame.rtp_timestamp);
                    auto raw = pkt.SerializeInterleaved(ctx->audio_rtp_channel);
                    if (!c->Send(raw)) return false;
                }
                return true;
            },
            true /* replay_gop_cache */);
        return;
    }

    if (req.method == "TEARDOWN") {
        if (ctx->media_session && ctx->subscriber_id != 0) {
            ctx->media_session->Unsubscribe(ctx->subscriber_id);
            ctx->subscriber_id = 0;
        }
        conn->Send(resp.Serialize());
        return;
    }

    if (req.method == "GET_PARAMETER") {
        conn->Send(resp.Serialize());
        return;
    }

    resp.status_code = 405;
    resp.reason = "Method Not Allowed";
    conn->Send(resp.Serialize());
}

// ============================================================================
// RtspPusherClient Implementation
// ============================================================================

std::optional<protocol::RtspResponse> RtspPusherClient::SendRequestAndWait(protocol::RtspRequest& req) {
    if (!sock_) return std::nullopt;
    req.cseq = cseq_++;
    if (!session_id_.empty()) {
        req.session = session_id_;
    }

    std::string raw_req = req.Serialize();
    if (!net::SocketUtils::SendAll(sock_.Get(), raw_req)) {
        return std::nullopt;
    }

    core::ByteBuffer buf(4096);
    uint8_t tmp[2048];
    while (true) {
        pollfd pfd{sock_.Get(), POLLIN, 0};
        if (::poll(&pfd, 1, 2000) <= 0) {
            return std::nullopt;
        }
        ssize_t n = ::recv(sock_.Get(), tmp, sizeof(tmp), 0);
        if (n <= 0) return std::nullopt;
        buf.Append(tmp, static_cast<size_t>(n));

        size_t consumed = 0;
        auto resp = protocol::RtspResponse::Parse(buf.ReadableStringView(), &consumed);
        if (resp) {
            if (!resp->session.empty()) {
                session_id_ = resp->session;
            }
            return resp;
        }
    }
}

bool RtspPusherClient::ConnectAndPublish(
    std::string_view host,
    uint16_t port,
    std::string_view stream_path,
    const core::VideoTrackInfo& video_track,
    const std::optional<core::AudioTrackInfo>& audio_track) {
    sock_ = net::SocketUtils::ConnectTcp(host, port, 3000);
    if (!sock_) return false;

    std::string path(stream_path);
    if (path.empty() || path[0] != '/') path = "/" + path;
    base_url_ = "rtsp://" + std::string(host) + ":" + std::to_string(port) + path;

    // 1. OPTIONS
    {
        protocol::RtspRequest req;
        req.method = "OPTIONS";
        req.url = base_url_;
        auto resp = SendRequestAndWait(req);
        if (!resp || resp->status_code != 200) return false;
    }

    // 2. ANNOUNCE
    {
        protocol::RtspRequest req;
        req.method = "ANNOUNCE";
        req.url = base_url_;
        req.headers["Content-Type"] = "application/sdp";
        req.body = protocol::SdpCodec::Generate(video_track, audio_track, host, path);
        auto resp = SendRequestAndWait(req);
        if (!resp || resp->status_code != 200) return false;
    }

    // 3. SETUP Video (trackID=0, interleaved=0-1)
    {
        protocol::RtspRequest req;
        req.method = "SETUP";
        req.url = base_url_ + "/trackID=0";
        req.headers["Transport"] = "RTP/AVP/TCP;unicast;interleaved=0-1;mode=record";
        auto resp = SendRequestAndWait(req);
        if (!resp || resp->status_code != 200) return false;
    }

    // 4. SETUP Audio (trackID=1, interleaved=2-3) if present
    if (audio_track) {
        audio_sample_rate_ = audio_track->sample_rate;
        protocol::RtspRequest req;
        req.method = "SETUP";
        req.url = base_url_ + "/trackID=1";
        req.headers["Transport"] = "RTP/AVP/TCP;unicast;interleaved=2-3;mode=record";
        auto resp = SendRequestAndWait(req);
        if (!resp || resp->status_code != 200) return false;
    }

    // 5. RECORD
    {
        protocol::RtspRequest req;
        req.method = "RECORD";
        req.url = base_url_;
        req.headers["Range"] = "npt=0.000-";
        auto resp = SendRequestAndWait(req);
        if (!resp || resp->status_code != 200) return false;
    }

    return true;
}

bool RtspPusherClient::PushH264Nalu(std::span<const uint8_t> raw_nalu, uint64_t pts_ms) {
    if (!sock_) return false;
    uint32_t rtp_ts = static_cast<uint32_t>((pts_ms * 90000) / 1000);
    auto pkts = video_packetizer_.PacketizeNalu(raw_nalu, rtp_ts, true);
    for (const auto& pkt : pkts) {
        auto frame = pkt.SerializeInterleaved(video_rtp_channel_);
        if (!net::SocketUtils::SendAll(sock_.Get(), frame)) {
            return false;
        }
    }
    return true;
}

bool RtspPusherClient::PushRawAac(std::span<const uint8_t> raw_aac, uint64_t pts_ms) {
    if (!sock_) return false;
    uint32_t rtp_ts = static_cast<uint32_t>((pts_ms * audio_sample_rate_) / 1000);
    auto pkt = audio_packetizer_.PacketizeRawAac(raw_aac, rtp_ts);
    auto frame = pkt.SerializeInterleaved(audio_rtp_channel_);
    return net::SocketUtils::SendAll(sock_.Get(), frame);
}

void RtspPusherClient::Close() {
    if (sock_) {
        protocol::RtspRequest req;
        req.method = "TEARDOWN";
        req.url = base_url_;
        req.cseq = cseq_++;
        req.session = session_id_;
        net::SocketUtils::SendAll(sock_.Get(), req.Serialize(), 200);
        sock_.Reset();
    }
}

// ============================================================================
// RtspPullerClient Implementation
// ============================================================================

std::optional<protocol::RtspResponse> RtspPullerClient::SendRequestAndWait(protocol::RtspRequest& req) {
    if (!sock_) return std::nullopt;
    req.cseq = cseq_++;
    if (!session_id_.empty()) {
        req.session = session_id_;
    }

    std::string raw_req = req.Serialize();
    if (!net::SocketUtils::SendAll(sock_.Get(), raw_req)) {
        return std::nullopt;
    }

    uint8_t tmp[4096];
    while (true) {
        size_t consumed = 0;
        auto resp = protocol::RtspResponse::Parse(recv_buf_.ReadableStringView(), &consumed);
        if (resp) {
            recv_buf_.Retrieve(consumed);
            if (!resp->session.empty()) {
                session_id_ = resp->session;
            }
            return resp;
        }

        pollfd pfd{sock_.Get(), POLLIN, 0};
        if (::poll(&pfd, 1, 2000) <= 0) {
            return std::nullopt;
        }
        ssize_t n = ::recv(sock_.Get(), tmp, sizeof(tmp), 0);
        if (n <= 0) return std::nullopt;
        recv_buf_.Append(tmp, static_cast<size_t>(n));
    }
}

bool RtspPullerClient::ConnectAndPlay(
    std::string_view host,
    uint16_t port,
    std::string_view stream_path,
    core::VideoTrackInfo* out_video_track,
    core::AudioTrackInfo* out_audio_track) {
    sock_ = net::SocketUtils::ConnectTcp(host, port, 3000);
    if (!sock_) return false;
    recv_buf_.RetrieveAll();

    std::string path(stream_path);
    if (path.empty() || path[0] != '/') path = "/" + path;
    base_url_ = "rtsp://" + std::string(host) + ":" + std::to_string(port) + path;

    // 1. OPTIONS
    {
        protocol::RtspRequest req;
        req.method = "OPTIONS";
        req.url = base_url_;
        auto resp = SendRequestAndWait(req);
        if (!resp || resp->status_code != 200) return false;
    }

    // 2. DESCRIBE
    protocol::SdpSessionInfo sdp_info;
    {
        protocol::RtspRequest req;
        req.method = "DESCRIBE";
        req.url = base_url_;
        req.headers["Accept"] = "application/sdp";
        auto resp = SendRequestAndWait(req);
        if (!resp || resp->status_code != 200) return false;
        sdp_info = protocol::SdpCodec::Parse(resp->body);
        if (out_video_track && sdp_info.has_video) {
            *out_video_track = sdp_info.video_track;
        }
        if (out_audio_track && sdp_info.has_audio) {
            *out_audio_track = sdp_info.audio_track;
        }
        if (sdp_info.has_audio) {
            audio_sample_rate_ = sdp_info.audio_track.sample_rate;
        }
    }

    // 3. SETUP Video
    if (sdp_info.has_video) {
        protocol::RtspRequest req;
        req.method = "SETUP";
        req.url = base_url_ + "/trackID=0";
        req.headers["Transport"] = "RTP/AVP/TCP;unicast;interleaved=0-1";
        auto resp = SendRequestAndWait(req);
        if (!resp || resp->status_code != 200) return false;
    }

    // 4. SETUP Audio
    if (sdp_info.has_audio) {
        protocol::RtspRequest req;
        req.method = "SETUP";
        req.url = base_url_ + "/trackID=1";
        req.headers["Transport"] = "RTP/AVP/TCP;unicast;interleaved=2-3";
        auto resp = SendRequestAndWait(req);
        if (!resp || resp->status_code != 200) return false;
    }

    // 5. PLAY
    {
        protocol::RtspRequest req;
        req.method = "PLAY";
        req.url = base_url_;
        req.headers["Range"] = "npt=0.000-";
        auto resp = SendRequestAndWait(req);
        if (!resp || resp->status_code != 200) return false;
    }

    return true;
}

size_t RtspPullerClient::ReceiveFrames(size_t max_frames, int timeout_ms, FrameHandler on_frame) {
    if (!sock_) return 0;
    size_t count = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    uint8_t tmp[16384];

    while (count < max_frames) {
        // Parse any buffered interleaved RTP packets first
        while (recv_buf_.ReadableBytes() >= 4 && count < max_frames) {
            const uint8_t* p = recv_buf_.Peek();
            if (p[0] != '$') {
                recv_buf_.Retrieve(1);
                continue;
            }
            uint8_t channel = p[1];
            uint16_t len = (static_cast<uint16_t>(p[2]) << 8) | p[3];
            if (recv_buf_.ReadableBytes() < 4 + static_cast<size_t>(len)) {
                break;
            }

            std::span<const uint8_t> rtp_span(p + 4, len);
            auto pkt = protocol::RtpPacket::Parse(rtp_span);
            if (pkt) {
                if (channel == video_rtp_channel_) {
                    auto nalus = video_depacketizer_.ProcessPacket(*pkt);
                    uint64_t pts_ms = (static_cast<uint64_t>(pkt->header.timestamp) * 1000) / 90000;
                    for (const auto& item : nalus) {
                        auto type = codec::H264Parser::GetNaluType(item.nalu[0]);
                        bool is_idr = (type == codec::H264NaluType::kIdrSlice);
                        auto frame = core::MediaFrame::Create(
                            core::CodecType::kH264,
                            is_idr ? core::FrameType::kKeyFrame : core::FrameType::kInterFrame,
                            pts_ms,
                            pts_ms,
                            item.nalu,
                            90000);
                        if (on_frame) on_frame(frame);
                        ++count;
                    }
                } else if (channel == audio_rtp_channel_) {
                    auto raw_aac = protocol::AacRtpCodec::DepacketizeRawAac(*pkt);
                    if (raw_aac) {
                        uint64_t pts_ms = (static_cast<uint64_t>(pkt->header.timestamp) * 1000) / audio_sample_rate_;
                        auto frame = core::MediaFrame::Create(
                            core::CodecType::kAAC,
                            core::FrameType::kAudioFrame,
                            pts_ms,
                            pts_ms,
                            *raw_aac,
                            audio_sample_rate_);
                        if (on_frame) on_frame(frame);
                        ++count;
                    }
                }
            }
            recv_buf_.Retrieve(4 + len);
        }

        if (count >= max_frames) break;

        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        int remain_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

        pollfd pfd{sock_.Get(), POLLIN, 0};
        if (::poll(&pfd, 1, std::max(1, remain_ms)) <= 0) {
            break;
        }
        ssize_t n = ::recv(sock_.Get(), tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        recv_buf_.Append(tmp, static_cast<size_t>(n));
    }

    return count;
}

void RtspPullerClient::Close() {
    sock_.Reset();
}

}  // namespace nexus::media
