#include "nexus/media/http_flv_server.hpp"
#include "nexus/codec/aac_parser.hpp"
#include "nexus/protocol/flv_muxer.hpp"

#include <sstream>

namespace nexus::media {

struct HttpFlvServer::HttpClientContext {
    MediaSessionPtr media_session;
    MediaSession::SubscriberId subscriber_id = 0;
};

HttpFlvServer::HttpFlvServer(std::string bind_ip, uint16_t port, std::shared_ptr<MediaHub> hub)
    : hub_(std::move(hub)), tcp_server_(std::move(bind_ip), port) {
    tcp_server_.SetMessageCallback([this](const net::TcpConnectionPtr& conn, core::ByteBuffer& buf) {
        OnMessage(conn, buf);
    });
    tcp_server_.SetCloseCallback([this](const net::TcpConnectionPtr& conn) {
        OnClose(conn);
    });
}

HttpFlvServer::~HttpFlvServer() {
    Stop();
}

bool HttpFlvServer::Start() {
    return tcp_server_.Start();
}

void HttpFlvServer::Stop() {
    tcp_server_.Stop();
}

void HttpFlvServer::OnClose(const net::TcpConnectionPtr& conn) {
    auto ctx = conn->GetContext<HttpClientContext>();
    if (ctx && ctx->media_session && ctx->subscriber_id != 0) {
        ctx->media_session->Unsubscribe(ctx->subscriber_id);
        ctx->subscriber_id = 0;
    }
}

void HttpFlvServer::OnMessage(const net::TcpConnectionPtr& conn, core::ByteBuffer& buffer) {
    auto view = buffer.ReadableStringView();
    auto hdr_end = view.find("\r\n\r\n");
    if (hdr_end == std::string_view::npos) {
        return;
    }

    auto line_end = view.find("\r\n");
    std::string first_line(view.substr(0, line_end));
    buffer.Retrieve(hdr_end + 4);

    std::istringstream iss(first_line);
    std::string method, path;
    if (!(iss >> method >> path)) {
        return;
    }

    // Strip query string if present
    auto qpos = path.find('?');
    if (qpos != std::string::npos) {
        path = path.substr(0, qpos);
    }

    if (path.size() > 4 && path.substr(path.size() - 4) == ".flv") {
        std::string stream_path = path.substr(0, path.size() - 4);
        HandleFlvStreamRequest(conn, stream_path);
        return;
    }

    if (path == "/api/streams") {
        HandleApiStreamsRequest(conn);
        return;
    }

    HandleDashboardRequest(conn);
}

void HttpFlvServer::HandleFlvStreamRequest(const net::TcpConnectionPtr& conn,
                                           std::string_view stream_path) {
    auto session = hub_->FindSession(stream_path);
    if (!session) {
        std::string not_found =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 16\r\n"
            "Connection: close\r\n\r\n"
            "Stream Not Found";
        conn->Send(not_found);
        conn->ForceClose();
        return;
    }

    auto ctx = std::make_shared<HttpClientContext>();
    ctx->media_session = session;
    conn->SetContext(ctx);

    std::string http_hdr =
        "HTTP/1.1 200 OK\r\n"
        "Server: NexusStream/1.0\r\n"
        "Content-Type: video/x-flv\r\n"
        "Cache-Control: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: keep-alive\r\n\r\n";
    if (!conn->Send(http_hdr)) return;

    auto vtrack = session->GetVideoTrack();
    auto atrack = session->GetAudioTrack();

    // 1. Send 13-byte FLV Header
    auto flv_hdr = protocol::FlvMuxer::BuildFlvHeader(vtrack.has_value(), atrack.has_value());
    if (!conn->Send(flv_hdr)) return;

    // 2. Send FLV Video Sequence Header (AVCDecoderConfigurationRecord)
    if (vtrack && !vtrack->sps.empty() && !vtrack->pps.empty()) {
        auto vseq = protocol::FlvMuxer::BuildVideoSequenceHeaderTag(vtrack->sps, vtrack->pps, 0);
        if (!conn->Send(vseq)) return;
    }

    // 3. Send FLV Audio Sequence Header (AudioSpecificConfig)
    if (atrack) {
        std::vector<uint8_t> asc = atrack->audio_specific_config;
        if (asc.empty()) {
            asc = codec::AacParser::BuildAudioSpecificConfig(
                atrack->profile, atrack->sample_rate, atrack->channels);
        }
        auto aseq = protocol::FlvMuxer::BuildAudioSequenceHeaderTag(asc, 0);
        if (!conn->Send(aseq)) return;
    }

    // 4. Subscribe to live session with GOP Cache instant-play enabled!
    std::weak_ptr<net::TcpConnection> weak_conn = conn;
    ctx->subscriber_id = session->Subscribe(
        [weak_conn](const core::MediaFrame& frame) -> bool {
            auto c = weak_conn.lock();
            if (!c || !c->IsConnected()) return false;
            auto tag = protocol::FlvMuxer::MuxFrame(frame);
            if (tag.empty()) return true;
            return c->Send(tag);
        },
        true /* replay_gop_cache */);
}

void HttpFlvServer::HandleApiStreamsRequest(const net::TcpConnectionPtr& conn) {
    auto stats = hub_->GetAllStats();
    std::ostringstream json;
    json << "{\"server\":\"NexusStream/1.0\",\"streams\":[";
    for (size_t i = 0; i < stats.size(); ++i) {
        const auto& s = stats[i];
        if (i > 0) json << ",";
        json << "{"
             << "\"path\":\"" << s.stream_path << "\","
             << "\"width\":" << s.width << ","
             << "\"height\":" << s.height << ","
             << "\"fps\":" << s.fps << ","
             << "\"audio_sample_rate\":" << s.audio_sample_rate << ","
             << "\"audio_channels\":" << static_cast<int>(s.audio_channels) << ","
             << "\"video_frames\":" << s.video_frames << ","
             << "\"audio_frames\":" << s.audio_frames << ","
             << "\"total_bytes\":" << s.total_bytes << ","
             << "\"gop_cache_frames\":" << s.gop_cache_frames << ","
             << "\"gop_count\":" << s.gop_count << ","
             << "\"subscribers\":" << s.subscribers
             << "}";
    }
    json << "]}";
    std::string body = json.str();

    std::ostringstream resp;
    resp << "HTTP/1.1 200 OK\r\n"
         << "Content-Type: application/json; charset=utf-8\r\n"
         << "Access-Control-Allow-Origin: *\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Connection: close\r\n\r\n"
         << body;
    conn->Send(resp.str());
    conn->ForceClose();
}

void HttpFlvServer::HandleDashboardRequest(const net::TcpConnectionPtr& conn) {
    static const std::string kHtml = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<title>NexusStream C++20 Media Server Dashboard</title>
<style>
  body { font-family: system-ui, sans-serif; background: #0f172a; color: #f8fafc; margin: 0; padding: 24px; }
  h1 { margin-top: 0; color: #38bdf8; }
  .card { background: #1e293b; border: 1px solid #334155; border-radius: 10px; padding: 18px; margin-bottom: 16px; }
  table { width: 100%; border-collapse: collapse; margin-top: 12px; }
  th, td { text-align: left; padding: 10px 12px; border-bottom: 1px solid #334155; }
  th { color: #94a3b8; font-weight: 600; }
  .badge { background: #0284c7; color: #fff; padding: 3px 8px; border-radius: 4px; font-size: 12px; }
</style>
</head>
<body>
  <h1>NexusStream C++20 Streaming Media Server</h1>
  <div class="card">
    <p>Protocols: <b>RTSP/1.0 (Push &amp; Pull)</b> | <b>RTP/RTCP (RFC 6184 FU-A &amp; RFC 3640)</b> | <b>HTTP-FLV Live</b> | <b>GOP Cache Instant-Play</b></p>
    <table>
      <thead>
        <tr>
          <th>Stream Path</th>
          <th>Video</th>
          <th>Audio</th>
          <th>Video Frames</th>
          <th>Audio Frames</th>
          <th>GOP Cache</th>
          <th>Subscribers</th>
          <th>Total Traffic</th>
        </tr>
      </thead>
      <tbody id="streams-body">
        <tr><td colspan="8">Loading live telemetry...</td></tr>
      </tbody>
    </table>
  </div>
  <script>
    async function refresh() {
      try {
        const res = await fetch('/api/streams');
        const data = await res.json();
        const tbody = document.getElementById('streams-body');
        if (!data.streams || data.streams.length === 0) {
          tbody.innerHTML = '<tr><td colspan="8">No active streams yet. Run <code>./nexus_pusher</code> to publish!</td></tr>';
          return;
        }
        tbody.innerHTML = data.streams.map(s => `
          <tr>
            <td><span class="badge">LIVE</span> <b>${s.path}</b></td>
            <td>H.264 ${s.width}x${s.height} @ ${s.fps}fps</td>
            <td>AAC ${s.audio_sample_rate}Hz ${s.audio_channels}ch</td>
            <td>${s.video_frames}</td>
            <td>${s.audio_frames}</td>
            <td>${s.gop_cache_frames} frames (GOP #${s.gop_count})</td>
            <td>${s.subscribers}</td>
            <td>${(s.total_bytes / 1024).toFixed(1)} KB</td>
          </tr>
        `).join('');
      } catch (e) {}
    }
    refresh();
    setInterval(refresh, 1000);
  </script>
</body>
</html>)HTML";

    std::ostringstream resp;
    resp << "HTTP/1.1 200 OK\r\n"
         << "Content-Type: text/html; charset=utf-8\r\n"
         << "Content-Length: " << kHtml.size() << "\r\n"
         << "Connection: close\r\n\r\n"
         << kHtml;
    conn->Send(resp.str());
    conn->ForceClose();
}

}  // namespace nexus::media
