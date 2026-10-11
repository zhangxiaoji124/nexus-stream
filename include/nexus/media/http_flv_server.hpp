#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "nexus/media/media_hub.hpp"
#include "nexus/net/tcp_server.hpp"

namespace nexus::media {

/**
 * @brief HTTP-FLV Live Streaming Server + Real-Time Web Telemetry Dashboard.
 *        Endpoints:
 *        - GET /<app>/<stream>.flv  -> Low-latency HTTP-FLV live stream (with GOP cache instant play)
 *        - GET /api/streams         -> JSON telemetry for all active media sessions
 *        - GET /                    -> Built-in HTML5 Live Media Server Dashboard
 */
class HttpFlvServer {
public:
    HttpFlvServer(std::string bind_ip, uint16_t port, std::shared_ptr<MediaHub> hub);
    ~HttpFlvServer();

    bool Start();
    void Stop();

    uint16_t BoundPort() const noexcept { return tcp_server_.BoundPort(); }

private:
    struct HttpClientContext;

    void OnMessage(const net::TcpConnectionPtr& conn, core::ByteBuffer& buffer);
    void OnClose(const net::TcpConnectionPtr& conn);

    void HandleFlvStreamRequest(const net::TcpConnectionPtr& conn, std::string_view stream_path);
    void HandleApiStreamsRequest(const net::TcpConnectionPtr& conn);
    void HandleDashboardRequest(const net::TcpConnectionPtr& conn);

    std::shared_ptr<MediaHub> hub_;
    net::TcpServer tcp_server_;
};

}  // namespace nexus::media
