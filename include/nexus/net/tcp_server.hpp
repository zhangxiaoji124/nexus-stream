#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "nexus/core/ring_buffer.hpp"
#include "nexus/net/event_loop.hpp"
#include "nexus/net/socket_utils.hpp"

namespace nexus::net {

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    TcpConnection(int fd, std::string peer_ip, uint16_t peer_port);
    ~TcpConnection() = default;

    int Fd() const noexcept { return fd_.Get(); }
    const std::string& PeerIp() const noexcept { return peer_ip_; }
    uint16_t PeerPort() const noexcept { return peer_port_; }
    bool IsConnected() const noexcept { return connected_.load(); }

    core::ByteBuffer& InputBuffer() noexcept { return input_buffer_; }

    bool Send(std::span<const uint8_t> data);
    bool Send(std::string_view text);
    void ForceClose();

    void SetContext(std::shared_ptr<void> ctx) { context_ = std::move(ctx); }
    template <typename T>
    std::shared_ptr<T> GetContext() const {
        return std::static_pointer_cast<T>(context_);
    }

private:
    ScopedFd fd_;
    std::string peer_ip_;
    uint16_t peer_port_;
    std::atomic<bool> connected_{true};
    core::ByteBuffer input_buffer_{16384};
    std::mutex write_mutex_;
    std::shared_ptr<void> context_;
};

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

class TcpServer {
public:
    using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
    using MessageCallback = std::function<void(const TcpConnectionPtr&, core::ByteBuffer&)>;
    using CloseCallback = std::function<void(const TcpConnectionPtr&)>;

    TcpServer(std::string bind_ip, uint16_t port);
    ~TcpServer();

    void SetConnectionCallback(ConnectionCallback cb) { on_connect_ = std::move(cb); }
    void SetMessageCallback(MessageCallback cb) { on_message_ = std::move(cb); }
    void SetCloseCallback(CloseCallback cb) { on_close_ = std::move(cb); }

    bool Start();
    void Stop();

    uint16_t BoundPort() const noexcept { return bound_port_; }
    size_t ActiveConnectionCount() const;

private:
    void HandleAccept();
    void HandleRead(int client_fd);
    void RemoveConnection(int client_fd);

    std::string bind_ip_;
    uint16_t requested_port_;
    uint16_t bound_port_ = 0;

    ScopedFd listen_fd_;
    std::unique_ptr<EventLoop> loop_;
    std::thread loop_thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex conn_mutex_;
    std::unordered_map<int, TcpConnectionPtr> connections_;

    ConnectionCallback on_connect_;
    MessageCallback on_message_;
    CloseCallback on_close_;
};

}  // namespace nexus::net
