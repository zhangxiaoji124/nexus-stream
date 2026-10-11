#include "nexus/net/socket_utils.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nexus::net {

void ScopedFd::Reset(int new_fd) noexcept {
    if (fd_ >= 0 && fd_ != new_fd) {
        ::close(fd_);
    }
    fd_ = new_fd;
}

bool SocketUtils::SetNonBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool SocketUtils::SetReuseAddr(int fd) {
    int opt = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        return false;
    }
#ifdef SO_REUSEPORT
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
    return true;
}

bool SocketUtils::SetTcpNoDelay(int fd) {
    int opt = 1;
    return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == 0;
}

ScopedFd SocketUtils::CreateTcpServer(std::string_view bind_ip, uint16_t port,
                                      uint16_t* out_bound_port) {
    ScopedFd fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd) return {};

    SetReuseAddr(fd.Get());

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (bind_ip.empty() || bind_ip == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        std::string ip_str(bind_ip);
        if (::inet_pton(AF_INET, ip_str.c_str(), &addr.sin_addr) <= 0) {
            return {};
        }
    }

    if (::bind(fd.Get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        return {};
    }
    if (::listen(fd.Get(), SOMAXCONN) < 0) {
        return {};
    }

    if (out_bound_port) {
        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(fd.Get(), reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
            *out_bound_port = ntohs(bound.sin_port);
        }
    }
    return fd;
}

ScopedFd SocketUtils::ConnectTcp(std::string_view host, uint16_t port, int timeout_ms) {
    ScopedFd fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd) return {};

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::string host_str(host);
    if (::inet_pton(AF_INET, host_str.c_str(), &addr.sin_addr) <= 0) {
        return {};
    }

    int rc = ::connect(fd.Get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc < 0) {
        if (errno != EINPROGRESS) {
            return {};
        }
        pollfd pfd{};
        pfd.fd = fd.Get();
        pfd.events = POLLOUT;
        if (::poll(&pfd, 1, timeout_ms) <= 0) {
            return {};
        }
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (::getsockopt(fd.Get(), SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
            return {};
        }
    }

    // Restore blocking mode for synchronous client operations if desired, or keep TCP_NODELAY
    int flags = ::fcntl(fd.Get(), F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd.Get(), F_SETFL, flags & ~O_NONBLOCK);
    }
    SetTcpNoDelay(fd.Get());
    return fd;
}

bool SocketUtils::SendAll(int fd, std::span<const uint8_t> data, int timeout_ms) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            if (::poll(&pfd, 1, timeout_ms) <= 0) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

}  // namespace nexus::net
