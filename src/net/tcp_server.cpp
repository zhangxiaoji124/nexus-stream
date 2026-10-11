#include "nexus/net/tcp_server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nexus::net {

TcpConnection::TcpConnection(int fd, std::string peer_ip, uint16_t peer_port)
    : fd_(fd), peer_ip_(std::move(peer_ip)), peer_port_(peer_port) {}

bool TcpConnection::Send(std::span<const uint8_t> data) {
    if (!connected_.load() || !fd_) return false;
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!SocketUtils::SendAll(fd_.Get(), data, 2000)) {
        connected_.store(false);
        return false;
    }
    return true;
}

bool TcpConnection::Send(std::string_view text) {
    return Send(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

void TcpConnection::ForceClose() {
    connected_.store(false);
    if (fd_) {
        ::shutdown(fd_.Get(), SHUT_RDWR);
    }
}

TcpServer::TcpServer(std::string bind_ip, uint16_t port)
    : bind_ip_(std::move(bind_ip)), requested_port_(port) {}

TcpServer::~TcpServer() {
    Stop();
}

bool TcpServer::Start() {
    if (running_.load()) return true;

    listen_fd_ = SocketUtils::CreateTcpServer(bind_ip_, requested_port_, &bound_port_);
    if (!listen_fd_) {
        return false;
    }

    loop_ = std::make_unique<EventLoop>();
    loop_->AddOrUpdateFd(listen_fd_.Get(), EPOLLIN, [this](uint32_t) { HandleAccept(); });

    running_.store(true);
    loop_thread_ = std::thread([this]() { loop_->Loop(); });
    return true;
}

void TcpServer::Stop() {
    if (!running_.exchange(false)) return;

    if (loop_) {
        loop_->Quit();
    }
    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        for (auto& [fd, conn] : connections_) {
            conn->ForceClose();
        }
        connections_.clear();
    }
    listen_fd_.Reset();
    loop_.reset();
}

size_t TcpServer::ActiveConnectionCount() const {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    return connections_.size();
}

void TcpServer::HandleAccept() {
    while (true) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        int cfd = ::accept4(listen_fd_.Get(),
                            reinterpret_cast<sockaddr*>(&peer),
                            &len,
                            SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break;
        }

        SocketUtils::SetTcpNoDelay(cfd);

        char ip_buf[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip_buf, sizeof(ip_buf));
        uint16_t peer_port = ntohs(peer.sin_port);

        auto conn = std::make_shared<TcpConnection>(cfd, ip_buf, peer_port);
        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            connections_[cfd] = conn;
        }

        loop_->AddOrUpdateFd(cfd, EPOLLIN | EPOLLRDHUP, [this, cfd](uint32_t events) {
            if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                HandleRead(cfd);
                RemoveConnection(cfd);
                return;
            }
            if (events & EPOLLIN) {
                HandleRead(cfd);
            }
        });

        if (on_connect_) {
            on_connect_(conn);
        }
    }
}

void TcpServer::HandleRead(int client_fd) {
    TcpConnectionPtr conn;
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        auto it = connections_.find(client_fd);
        if (it == connections_.end()) return;
        conn = it->second;
    }

    uint8_t temp[16384];
    bool closed = false;

    while (true) {
        ssize_t n = ::recv(client_fd, temp, sizeof(temp), 0);
        if (n > 0) {
            conn->InputBuffer().Append(temp, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            closed = true;
            break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        closed = true;
        break;
    }

    if (conn->InputBuffer().ReadableBytes() > 0 && on_message_) {
        on_message_(conn, conn->InputBuffer());
    }

    if (closed || !conn->IsConnected()) {
        RemoveConnection(client_fd);
    }
}

void TcpServer::RemoveConnection(int client_fd) {
    TcpConnectionPtr conn;
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        auto it = connections_.find(client_fd);
        if (it == connections_.end()) return;
        conn = it->second;
        connections_.erase(it);
    }
    if (loop_) {
        loop_->RemoveFd(client_fd);
    }
    if (conn) {
        conn->ForceClose();
        if (on_close_) {
            on_close_(conn);
        }
    }
}

}  // namespace nexus::net
